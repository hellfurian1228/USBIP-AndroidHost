#include <jni.h>
#include <android/log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <cstring>
#include <cerrno>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>
#include <sys/ioctl.h>
#include <vector>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <condition_variable>
#include <shared_mutex>
#include <netinet/tcp.h>
#include <csignal>
#include <sys/poll.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define LOG_TAG "usbip_server"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define USBIP_PORT 3240
#define USBIP_VERSION 0x0111

static int g_server_socket = -1;
static std::mutex g_socket_mutex;
static std::thread g_server_thread;
static std::vector<int> g_client_sockets;
static std::mutex g_clients_mutex;
static std::atomic<bool> g_device_fatal_error{false};

static std::unordered_map<std::string, int> g_active_devices;
static std::shared_mutex g_devices_rw_mutex;
static std::condition_variable_any g_device_update_cv;

// Track which client socket is currently bound to which Bus ID
static std::unordered_map<std::string, int> g_busid_to_client_fd;
static std::mutex g_client_map_mutex;

static JavaVM* g_jvm = nullptr;
static jobject g_service_obj = nullptr;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

int get_int_for_busid(const char* method_name, const std::string& busid) {
    if (!g_jvm || !g_service_obj) return 0;
    JNIEnv* env;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        #ifdef __ANDROID__
            g_jvm->AttachCurrentThread(&env, nullptr);
        #else
            g_jvm->AttachCurrentThread((void**)&env, nullptr);
        #endif
        attached = true;
    }
    jclass cls = env->GetObjectClass(g_service_obj);
    jmethodID mid = env->GetMethodID(cls, method_name, "(Ljava/lang/String;)I");
    jstring jbusid = env->NewStringUTF(busid.c_str());
    int result = env->CallIntMethod(g_service_obj, mid, jbusid);
    env->DeleteLocalRef(jbusid);
    if (attached) g_jvm->DetachCurrentThread();
    return result;
}

void set_keepalive(int fd) {
    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
        LOGW("Warning: Failed to set SO_KEEPALIVE: %s", strerror(errno));
    }

    int idle = 30;    // 30 seconds idle before first probe
    int intvl = 5;     // 5 seconds between probes
    int cnt = 3;       // 3 failed probes before disconnect

#ifdef __ANDROID__
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

// USB/IP OP codes (handshake)
#define OP_REQ_IMPORT 0x8003
#define OP_REP_IMPORT 0x0003
#define OP_REQ_DEVLIST 0x8005
#define OP_REP_DEVLIST 0x0005

// USB/IP Commands
#define USBIP_CMD_SUBMIT 0x0001
#define USBIP_RET_SUBMIT 0x0003
#define USBIP_CMD_UNLINK 0x0002
#define USBIP_RET_UNLINK 0x0004

struct op_common {
    uint16_t version;
    uint16_t code;
    uint32_t status;
} __attribute__((packed));

struct op_req_import {
    char busid[32];
} __attribute__((packed));

struct usbip_usb_device {
    char path[256];
    char busid[32];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bConfigurationValue;
    uint8_t bNumConfigurations;
    uint8_t bNumInterfaces;
} __attribute__((packed));

struct usbip_usb_interface {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;
} __attribute__((packed));

struct endpoint_info {
    uint8_t addr;
    uint8_t type;
    uint16_t max_packet_size;
};

struct usbip_header {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    uint32_t transfer_flags;
    uint32_t transfer_buffer_length;
    uint32_t start_frame;
    uint32_t number_of_packets;
    uint32_t interval;
    uint8_t setup[8];
} __attribute__((packed));

struct usbip_ret_submit {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    uint32_t status;
    uint32_t actual_length;
    uint32_t start_frame;
    uint32_t number_of_packets;
    uint32_t error_count;
    uint8_t padding[8];
} __attribute__((packed));

struct async_urb_context {
    int client_fd;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    uint8_t* payload_buffer;
    struct usbdevfs_urb urb; // Must be at the end
};

std::atomic<int> in_flight_urbs_count{0};
std::mutex in_flight_mutex;
std::unordered_map<uint32_t, async_urb_context*> active_urbs;

void cleanup_zombie_urbs(int device_fd, int client_fd) {
    std::lock_guard<std::mutex> lock(in_flight_mutex);
    int discarded_count = 0;

    for (auto const& item : active_urbs) {
        async_urb_context* ctx = item.second;
        if (ctx->client_fd == client_fd) {
            ioctl(device_fd, USBDEVFS_DISCARDURB, &ctx->urb);
            discarded_count++;
        }
    }
    LOGI("Triggered hardware discard for %d zombie URBs.", discarded_count);
}

void reap_thread(std::string busid, int device_fd, const std::shared_ptr<std::atomic<bool>>& is_connected) {
    LOGI("URB Reaper thread started for bus %s.", busid.c_str());
    while (is_connected->load() || in_flight_urbs_count.load() > 0) {
        struct usbdevfs_urb *urb = nullptr;
        int res = ioctl(device_fd, USBDEVFS_REAPURBNDELAY, &urb);
        if (res < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            LOGE("REAPURB failed on bus %s: %s", busid.c_str(), strerror(errno));
            if (errno == ENODEV || errno == EBADF) {
                LOGW("reap_thread: Device lost on bus %s. Waiting for new FD...", busid.c_str());
                std::unique_lock<std::shared_mutex> lock(g_devices_rw_mutex);
                if (g_device_update_cv.wait_for(lock, std::chrono::seconds(10), [&busid]{
                    return g_active_devices.count(busid) && g_active_devices[busid] != -1;
                })) {
                    device_fd = g_active_devices[busid];
                    LOGI("reap_thread: Resuming on bus %s with new FD %d", busid.c_str(), device_fd);
                    continue;
                }
                LOGE("reap_thread: Timeout waiting for new device FD on bus %s.", busid.c_str());
                g_device_fatal_error.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        auto *ctx = (struct async_urb_context *)urb->usercontext;
        if (!ctx) continue;

        {
            std::lock_guard<std::mutex> lock(in_flight_mutex);
            active_urbs.erase(ntohl(ctx->seqnum));
            in_flight_urbs_count--;
        }

        struct usbip_ret_submit ret = {0};
        ret.command = htonl(USBIP_RET_SUBMIT);
        ret.seqnum = ctx->seqnum;
        ret.devid = ctx->devid;
        ret.direction = ctx->direction;
        ret.ep = ctx->ep;
        ret.status = htonl((uint32_t)urb->status);
        ret.actual_length = htonl(urb->actual_length);
        ret.number_of_packets = htonl(0xFFFFFFFF);

        if (urb->status != 0 && urb->status != -ENOENT) {
            if (urb->status == -EPIPE) {
                LOGW("Endpoint stalled (EPIPE) on bus %s, ep=%u, seq=%u. Reporting to client.", busid.c_str(), ntohl(ctx->ep), ntohl(ctx->seqnum));
            } else {
                LOGE("URB failed with status %d (ep=%u, seq=%u) on bus %s", (int)urb->status, ntohl(ctx->ep), ntohl(ctx->seqnum), busid.c_str());
            }
        }

        if (is_connected->load()) {
            if (send(ctx->client_fd, &ret, sizeof(ret), 0) < 0) {
                LOGE("Failed to send RET_SUBMIT header");
            } else if (urb->actual_length > 0 && ntohl(ctx->direction) == 1) {
                uint8_t* data_ptr = ctx->payload_buffer;
                if (ctx->urb.type == USBDEVFS_URB_TYPE_CONTROL) {
                    data_ptr += 8;
                }
                if (send(ctx->client_fd, data_ptr, urb->actual_length, 0) < 0) {
                    LOGE("Failed to send RET_SUBMIT data");
                }
            }
            LOGI("<<< USBIP_RET_SUBMIT: seq=%u, status=%d, len=%u, bus=%s",
                 ntohl(ctx->seqnum), (int32_t)ntohl(ret.status), (uint32_t)ntohl(ret.actual_length), busid.c_str());
        }

        delete[] ctx->payload_buffer;
        free(ctx);
    }
    LOGI("URB Reaper thread exiting.");
}

ssize_t recv_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    auto *p = (char *)buf;
    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, MSG_WAITALL);
        if (n <= 0) return (n == 0) ? (ssize_t)total : n;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

void get_device_info(int device_fd, struct usbip_usb_device *dev, std::vector<struct usbip_usb_interface> *intfs, std::vector<endpoint_info> *eps, const char *busid) {
    struct usb_device_descriptor desc = {0};
    struct usbdevfs_ctrltransfer ctrl = {0};
    ctrl.bRequestType = 0x80;
    ctrl.bRequest = 0x06;
    ctrl.wValue = 0x0100;
    ctrl.wLength = sizeof(desc);
    ctrl.timeout = 1000;
    ctrl.data = &desc;

    memset(dev, 0, sizeof(*dev));
    snprintf(dev->path, sizeof(dev->path), "/sys/devices/virtual/usbip/%s", busid);
    strncpy(dev->busid, busid, sizeof(dev->busid) - 1);
    dev->busnum = htonl(1);
    dev->devnum = htonl(2);
    dev->speed = htonl(3);

    if (device_fd == -1 || ioctl(device_fd, USBDEVFS_CONTROL, &ctrl) < 0) {
        std::string s_busid(busid);
        dev->idVendor = htons((uint16_t)get_int_for_busid("getVidForBusId", s_busid));
        dev->idProduct = htons((uint16_t)get_int_for_busid("getPidForBusId", s_busid));
        dev->bcdDevice = htons(0x0111);
        dev->bDeviceClass = 0;
        dev->bDeviceSubClass = 0;
        dev->bDeviceProtocol = 0;
        dev->bConfigurationValue = 1;
        dev->bNumConfigurations = 1;
        dev->bNumInterfaces = (uint8_t)get_int_for_busid("getInterfaceCountForBusId", s_busid);
        if (dev->bNumInterfaces == 0) dev->bNumInterfaces = 1;

        intfs->clear();
        struct usbip_usb_interface i = {0};
        i.bInterfaceClass = 3;
        intfs->push_back(i);
        return;
    }

    dev->idVendor = htons(desc.idVendor);
    dev->idProduct = htons(desc.idProduct);
    dev->bcdDevice = htons(desc.bcdDevice);
    dev->bDeviceClass = desc.bDeviceClass;
    dev->bDeviceSubClass = desc.bDeviceSubClass;
    dev->bDeviceProtocol = desc.bDeviceProtocol;
    dev->bConfigurationValue = 1;
    dev->bNumConfigurations = desc.bNumConfigurations;

    intfs->clear();
    eps->clear();
    std::vector<uint8_t> config_desc(1024);
    ctrl.wValue = 0x0200;
    ctrl.wLength = (uint16_t)config_desc.size();
    ctrl.data = config_desc.data();
    int len = ioctl(device_fd, USBDEVFS_CONTROL, &ctrl);
    if (len >= 9) {
        int pos = 0;
        while (pos + 1 < len) {
            uint8_t d_len = config_desc[(size_t)pos];
            if (d_len < 2 || pos + d_len > len) break;
            uint8_t d_type = config_desc[(size_t)pos + 1];
            if (d_type == 0x04 && d_len >= 9) {
                struct usbip_usb_interface i = {0};
                i.bInterfaceClass = config_desc[(size_t)pos + 5];
                i.bInterfaceSubClass = config_desc[(size_t)pos + 6];
                i.bInterfaceProtocol = config_desc[(size_t)pos + 7];
                intfs->push_back(i);
            } else if (d_type == 0x05 && d_len >= 7) {
                endpoint_info info_item = {0};
                info_item.addr = config_desc[(size_t)pos + 2];
                info_item.type = config_desc[(size_t)pos + 3] & 0x03;
                info_item.max_packet_size = (uint16_t)(config_desc[(size_t)pos + 4] | (config_desc[(size_t)pos + 5] << 8));
                eps->push_back(info_item);
            }
            pos += (int)d_len;
        }
    }
    dev->bNumInterfaces = (uint8_t)intfs->size();
    if (dev->bNumInterfaces == 0) dev->bNumInterfaces = 1;
}

void handle_client(int client_fd, int device_fd) {
    auto is_connected = std::make_shared<std::atomic<bool>>(true);
    std::string current_busid = "1-1";

    struct op_common header = {0};
    if (recv_all(client_fd, &header, sizeof(header)) < (ssize_t)sizeof(header)) {
        is_connected->store(false);
        return;
    }

    uint16_t code = ntohs(header.code);
    if (code == OP_REQ_DEVLIST) {
        LOGI("Handling OP_REQ_DEVLIST");
        JNIEnv* env;
        bool attached = false;
        if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            #ifdef __ANDROID__
                g_jvm->AttachCurrentThread(&env, nullptr);
            #else
                g_jvm->AttachCurrentThread((void**)&env, nullptr);
            #endif
            attached = true;
        }
        jclass cls = env->GetObjectClass(g_service_obj);
        jmethodID mid = env->GetMethodID(cls, "getExportedDevicesPayload", "()[B");
        jbyteArray jpayload = (jbyteArray)env->CallObjectMethod(g_service_obj, mid);

        struct op_common reply_header = {0};
        reply_header.version = htons(USBIP_VERSION);
        reply_header.code = htons(OP_REP_DEVLIST);
        reply_header.status = htonl(0);

        if (send(client_fd, &reply_header, sizeof(reply_header), 0) < 0) {
            LOGE("OP_REQ_DEVLIST: Failed to send header");
        } else if (jpayload) {
            jsize len = env->GetArrayLength(jpayload);
            jbyte* body = env->GetByteArrayElements(jpayload, nullptr);
            send(client_fd, body, (size_t)len, 0);
            env->ReleaseByteArrayElements(jpayload, body, JNI_ABORT);
            env->DeleteLocalRef(jpayload);
        }
        if (attached) g_jvm->DetachCurrentThread();
        is_connected->store(false);
        return;
    }

    if (code == OP_REQ_IMPORT) {
        struct op_req_import import_req = {0};
        if (recv_all(client_fd, &import_req, sizeof(import_req)) < (ssize_t)sizeof(import_req)) {
            is_connected->store(false);
            return;
        }
        std::string busid(import_req.busid, strnlen(import_req.busid, 32));
        int resolved_fd = get_int_for_busid("getFdForBusId", busid);
        if (resolved_fd == -1) {
            struct op_common err_header = {0};
            err_header.version = htons(USBIP_VERSION);
            err_header.code = htons(OP_REP_IMPORT);
            err_header.status = htonl(1);
            send(client_fd, &err_header, sizeof(err_header), 0);
            is_connected->store(false);
            return;
        }
        device_fd = resolved_fd;
        current_busid = busid;
        {
            std::lock_guard<std::mutex> lock(g_client_map_mutex);
            g_busid_to_client_fd[current_busid] = client_fd;
        }
        ioctl(device_fd, USBDEVFS_RESET);

        struct usbip_usb_device dev = {0};
        std::vector<struct usbip_usb_interface> intfs;
        std::vector<endpoint_info> eps;
        get_device_info(device_fd, &dev, &intfs, &eps, busid.c_str());

        struct op_common reply_header = {0};
        reply_header.version = htons(USBIP_VERSION);
        reply_header.code = htons(OP_REP_IMPORT);
        reply_header.status = htonl(0);

        send(client_fd, &reply_header, sizeof(reply_header), 0);
        send(client_fd, &dev, sizeof(dev), 0);

        std::thread(reap_thread, current_busid, device_fd, is_connected).detach();

        for (int i = 0; i < 16; i++) {
            struct usbdevfs_ioctl disconnect = {0};
            disconnect.ifno = i;
            disconnect.ioctl_code = USBDEVFS_DISCONNECT;
            ioctl(device_fd, USBDEVFS_IOCTL, &disconnect);
            int intf = i;
            ioctl(device_fd, USBDEVFS_CLAIMINTERFACE, &intf);
        }

        while (is_connected->load()) {
            struct usbip_header cmd_header = {0};
            if (recv_all(client_fd, &cmd_header, sizeof(cmd_header)) < (ssize_t)sizeof(cmd_header)) break;

            uint32_t command = ntohl(cmd_header.command);
            uint32_t ep = ntohl(cmd_header.ep) & 0x7F;
            uint32_t dir = ntohl(cmd_header.direction);
            uint32_t transfer_len = ntohl(cmd_header.transfer_buffer_length);

            if (command == USBIP_CMD_SUBMIT) {
                auto *ctx = (async_urb_context *)calloc(1, sizeof(async_urb_context));
                if (!ctx) break;
                ctx->client_fd = client_fd;
                ctx->seqnum = cmd_header.seqnum;
                ctx->devid = cmd_header.devid;
                ctx->direction = cmd_header.direction;
                ctx->ep = cmd_header.ep;

                if (ep == 0) {
                    ctx->payload_buffer = new uint8_t[8 + (size_t)transfer_len];
                    memcpy(ctx->payload_buffer, cmd_header.setup, 8);
                    if (dir == 0 && transfer_len > 0) {
                        recv_all(client_fd, ctx->payload_buffer + 8, transfer_len);
                    }
                    ctx->urb.type = USBDEVFS_URB_TYPE_CONTROL;
                    ctx->urb.buffer = ctx->payload_buffer;
                    ctx->urb.buffer_length = (int)(8 + transfer_len);
                } else {
                    if (transfer_len > 0) {
                        ctx->payload_buffer = new uint8_t[transfer_len];
                        if (dir == 0) {
                            recv_all(client_fd, ctx->payload_buffer, transfer_len);
                        }
                    } else {
                        ctx->payload_buffer = nullptr;
                    }
                    ctx->urb.buffer = ctx->payload_buffer;
                    ctx->urb.buffer_length = (int)transfer_len;

                    uint8_t ep_addr = (uint8_t)(ep | (dir ? 0x80 : 0));
                    uint8_t ep_type = 0x02;
                    for (const auto& item : eps) {
                        if (item.addr == ep_addr) { ep_type = item.type; break; }
                    }
                    ctx->urb.type = (ep_type == 0x01) ? 0 : ((ep_type == 0x03) ? 1 : 3);
                }
                ctx->urb.usercontext = ctx;
                ctx->urb.endpoint = (unsigned char)(ep | (dir == 1 ? 0x80 : 0));

                if (ep == 0) {
                    uint8_t bRequest = cmd_header.setup[1];
                    uint16_t wValue = (uint16_t)(cmd_header.setup[2] | (cmd_header.setup[3] << 8));
                    uint16_t wIndex = (uint16_t)(cmd_header.setup[4] | (cmd_header.setup[5] << 8));
                    if (cmd_header.setup[0] == 0x00 && bRequest == 0x09) {
                        for (int i = 0; i < 16; i++) { int intf = i; ioctl(device_fd, USBDEVFS_RELEASEINTERFACE, &intf); }
                        unsigned int config_val = wValue;
                        int res_sc = ioctl(device_fd, USBDEVFS_SETCONFIGURATION, &config_val);
                        for (int i = 0; i < 16; i++) { int intf = i; ioctl(device_fd, USBDEVFS_CLAIMINTERFACE, &intf); }
                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;
                        ret.status = (res_sc < 0) ? htonl((uint32_t)-errno) : 0;
                        send(client_fd, &ret, sizeof(ret), 0);
                        delete[] ctx->payload_buffer; free(ctx); continue;
                    } else if (cmd_header.setup[0] == 0x01 && bRequest == 0x0B) {
                        struct usbdevfs_setinterface setintf = {0};
                        setintf.interface = wIndex; setintf.altsetting = wValue;
                        int res_si = ioctl(device_fd, USBDEVFS_SETINTERFACE, &setintf);
                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;
                        ret.status = (res_si < 0) ? htonl((uint32_t)-errno) : 0;
                        send(client_fd, &ret, sizeof(ret), 0);
                        delete[] ctx->payload_buffer; free(ctx); continue;
                    } else if (cmd_header.setup[0] == 0x00 && bRequest == 0x05) {
                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;
                        send(client_fd, &ret, sizeof(ret), 0);
                        delete[] ctx->payload_buffer; free(ctx); continue;
                    } else if (cmd_header.setup[0] == 0x02 && bRequest == 0x01 && wValue == 0x0000) {
                        // CLEAR_FEATURE (ENDPOINT_HALT)
                        uint32_t target_endpoint = wIndex & 0xFF;
                        LOGI("Intercepted CLEAR_FEATURE (ENDPOINT_HALT) for ep 0x%02x", target_endpoint);
                        if (ioctl(device_fd, USBDEVFS_CLEAR_HALT, &target_endpoint) < 0) {
                            LOGW("USBDEVFS_CLEAR_HALT failed: %s", strerror(errno));
                        }
                        // Allow to fall through to SUBMITURB so Windows gets response
                    }
                }

                { std::lock_guard<std::mutex> lock(in_flight_mutex); active_urbs[ntohl(ctx->seqnum)] = ctx; in_flight_urbs_count++; }
                if (ioctl(device_fd, USBDEVFS_SUBMITURB, &ctx->urb) < 0) {
                    { std::lock_guard<std::mutex> lock(in_flight_mutex); active_urbs.erase(ntohl(ctx->seqnum)); in_flight_urbs_count--; }
                    struct usbip_ret_submit ret_err = {0};
                    ret_err.command = htonl(USBIP_RET_SUBMIT);
                    ret_err.seqnum = ctx->seqnum; ret_err.devid = ctx->devid; ret_err.direction = ctx->direction; ret_err.ep = ctx->ep;
                    ret_err.status = htonl((uint32_t)-errno);
                    send(client_fd, &ret_err, sizeof(ret_err), 0);
                    delete[] ctx->payload_buffer; free(ctx);
                }
            } else if (command == USBIP_CMD_UNLINK) {
                uint32_t target_seqnum = ntohl(cmd_header.transfer_flags);
                struct usbip_ret_submit ret_unlink = {0};
                ret_unlink.command = htonl(USBIP_RET_UNLINK);
                ret_unlink.seqnum  = cmd_header.seqnum;
                send(client_fd, &ret_unlink, sizeof(ret_unlink), 0);
                std::lock_guard<std::mutex> lock(in_flight_mutex);
                auto it = active_urbs.find(target_seqnum);
                if (it != active_urbs.end()) { ioctl(device_fd, USBDEVFS_DISCARDURB, &it->second->urb); }
            } else break;
        }
    }

    is_connected->store(false);
    { std::lock_guard<std::mutex> lock(g_client_map_mutex); if (g_busid_to_client_fd.count(current_busid) && g_busid_to_client_fd[current_busid] == client_fd) g_busid_to_client_fd.erase(current_busid); }
    cleanup_zombie_urbs(device_fd, client_fd);
}

void run_server(int device_fd_raw) {
    int device_fd = (device_fd_raw != -1) ? dup(device_fd_raw) : -1;
    int server_fd, client_fd;
    struct sockaddr_in address = {0};
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_keepalive(server_fd);
    int nodelay = 1;
    setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    { std::lock_guard<std::mutex> lock(g_socket_mutex); g_server_socket = server_fd; }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(USBIP_PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) { close(server_fd); return; }
    if (listen(server_fd, 3) < 0) { close(server_fd); return; }

    std::vector<std::thread> sessions;
    while (true) {
        client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (client_fd < 0) {
            std::lock_guard<std::mutex> lock(g_socket_mutex);
            if (g_server_socket == -1) break;
            continue;
        }
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        set_keepalive(client_fd);
        { std::lock_guard<std::mutex> lock(g_clients_mutex); g_client_sockets.push_back(client_fd); }
        sessions.emplace_back([client_fd]() {
            int current_fd = -1;
            { std::shared_lock<std::shared_mutex> lock(g_devices_rw_mutex); if (!g_active_devices.empty()) current_fd = g_active_devices.begin()->second; }
            handle_client(client_fd, current_fd);
            { std::lock_guard<std::mutex> lock(g_clients_mutex); g_client_sockets.erase(std::remove(g_client_sockets.begin(), g_client_sockets.end(), client_fd), g_client_sockets.end()); }
            close(client_fd);
        });
    }
    for (auto& t : sessions) if (t.joinable()) t.join();
    close(server_fd);
    if (device_fd >= 0) close(device_fd);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_UsbServerService_startNativeServer(JNIEnv *env, jobject thiz, jint device_fd) {
    signal(SIGPIPE, SIG_IGN);
    if (g_service_obj) env->DeleteGlobalRef(g_service_obj);
    g_service_obj = env->NewGlobalRef(thiz);
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_server_socket >= 0) { shutdown(g_server_socket, SHUT_RDWR); close(g_server_socket); g_server_socket = -1; }
    { std::unique_lock<std::shared_mutex> dev_lock(g_devices_rw_mutex); if (device_fd != -1) g_active_devices["1-1"] = device_fd; }
    if (g_server_thread.joinable()) g_server_thread.join();
    g_server_thread = std::thread(run_server, device_fd);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_UsbServerService_stopNativeServer(JNIEnv *env, jobject thiz) {
    if (g_service_obj) { env->DeleteGlobalRef(g_service_obj); g_service_obj = nullptr; }
    { std::lock_guard<std::mutex> lock(g_socket_mutex); if (g_server_socket >= 0) { shutdown(g_server_socket, SHUT_RDWR); close(g_server_socket); g_server_socket = -1; } }
    std::vector<int> clients; { std::lock_guard<std::mutex> lock(g_clients_mutex); clients = g_client_sockets; }
    for (int fd : clients) shutdown(fd, SHUT_RDWR);
    if (g_server_thread.joinable()) g_server_thread.join();
    { std::unique_lock<std::shared_mutex> dev_lock(g_devices_rw_mutex); for (auto const& item : g_active_devices) { if (item.second != -1) close(item.second); } g_active_devices.clear(); }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_UsbServerService_updateDeviceFd(JNIEnv *env, jobject thiz, jstring jbusid, jint new_fd) {
    const char* busid_ptr = env->GetStringUTFChars(jbusid, nullptr);
    std::string busid(busid_ptr);
    env->ReleaseStringUTFChars(jbusid, busid_ptr);
    { std::unique_lock<std::shared_mutex> lock(g_devices_rw_mutex); g_active_devices[busid] = new_fd; }
    g_device_update_cv.notify_all();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_UsbServerService_invalidateDeviceFd(JNIEnv *env, jobject thiz, jstring jbusid) {
    const char* busid_ptr = env->GetStringUTFChars(jbusid, nullptr);
    std::string busid(busid_ptr);
    env->ReleaseStringUTFChars(jbusid, busid_ptr);
    int client_fd = -1; { std::lock_guard<std::mutex> lock(g_client_map_mutex); if (g_busid_to_client_fd.count(busid)) { client_fd = g_busid_to_client_fd[busid]; g_busid_to_client_fd.erase(busid); } }
    if (client_fd != -1) { shutdown(client_fd, SHUT_RDWR); close(client_fd); }
    int usb_fd = -1; { std::unique_lock<std::shared_mutex> lock(g_devices_rw_mutex); if (g_active_devices.count(busid)) { usb_fd = g_active_devices[busid]; g_active_devices[busid] = -1; } }
    if (usb_fd != -1) close(usb_fd);
    g_device_update_cv.notify_all();
}
