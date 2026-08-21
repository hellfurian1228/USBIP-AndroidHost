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
#include <queue>
#include <memory>

#define LOG_TAG "usbip_server"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define USBIP_PORT 3240
#define USBIP_VERSION 0x0111
#define FLAG_REQUEST_UDP 0x01

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

void notify_performance_locks(bool acquire) {
    if (!g_jvm || !g_service_obj) return;
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
    const char* method = acquire ? "acquirePerformanceLocks" : "releasePerformanceLocks";
    jmethodID mid = env->GetMethodID(cls, method, "()V");
    if (mid) {
        env->CallVoidMethod(g_service_obj, mid);
    }
    if (attached) g_jvm->DetachCurrentThread();
}

void set_keepalive(int fd) {
    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
        LOGW("Warning: Failed to set SO_KEEPALIVE: %s", strerror(errno));
    }

    int idle = 2;    // Start sending keep-alive probes after 2 seconds of silence
    int intvl = 1;   // Send subsequent probes every 1 second
    int cnt = 3;     // Kill the socket after 3 failed probes

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
} __attribute__((packed));

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
    int udp_fd; // -1 for TCP, >= 0 for UDP
    struct sockaddr_in client_addr;
    uint32_t udp_seq_id;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    uint8_t* payload_buffer;
    struct usbdevfs_urb urb; // Must be at the end
} __attribute__((packed));

// Forward declaration of send_all for use in Session TX Queueing
ssize_t send_all(int fd, const void *buf, size_t len);

// --- High-Throughput TX Queueing ---
struct tx_packet {
    usbip_ret_submit header;
    uint8_t* payload;
    uint32_t payload_len;

    tx_packet() : payload(nullptr), payload_len(0) {
        memset(&header, 0, sizeof(header));
    }
    ~tx_packet() {
        if (payload) delete[] payload;
    }
};

struct session_context {
    int client_fd;
    std::shared_ptr<std::atomic<bool>> is_connected;
    std::queue<tx_packet*> tx_queue;
    std::mutex tx_mutex;
    std::condition_variable tx_cv;

    session_context(int fd, std::shared_ptr<std::atomic<bool>> conn)
        : client_fd(fd), is_connected(conn) {}
};

void tcp_tx_thread(std::shared_ptr<session_context> ctx) {
    notify_performance_locks(true);
    LOGI("TCP TX Thread started for fd %d", ctx->client_fd);
    while (ctx->is_connected->load()) {
        tx_packet* pkt = nullptr;
        {
            std::unique_lock<std::mutex> lock(ctx->tx_mutex);
            ctx->tx_cv.wait_for(lock, std::chrono::milliseconds(100), [&]{
                return !ctx->tx_queue.empty() || !ctx->is_connected->load();
            });
            if (!ctx->tx_queue.empty()) {
                pkt = ctx->tx_queue.front();
                ctx->tx_queue.pop();
            }
        }

        if (pkt) {
            if (send_all(ctx->client_fd, &pkt->header, sizeof(pkt->header)) != (ssize_t)sizeof(pkt->header)) {
                LOGE("TX Thread: Failed to send header to fd %d", ctx->client_fd);
                ctx->is_connected->store(false);
            } else if (pkt->payload_len > 0) {
                if (send_all(ctx->client_fd, pkt->payload, pkt->payload_len) != (ssize_t)pkt->payload_len) {
                    LOGE("TX Thread: Failed to send payload to fd %d", ctx->client_fd);
                    ctx->is_connected->store(false);
                }
            }
            delete pkt;
        }
    }

    // Cleanup queue on exit
    std::lock_guard<std::mutex> lock(ctx->tx_mutex);
    while(!ctx->tx_queue.empty()) {
        delete ctx->tx_queue.front();
        ctx->tx_queue.pop();
    }
    LOGI("TCP TX Thread exiting for fd %d", ctx->client_fd);
    notify_performance_locks(false);
}
// --- End TX Queueing ---

std::atomic<int> in_flight_urbs_count{0};
std::mutex in_flight_mutex;
std::unordered_map<uint32_t, async_urb_context*> active_urbs;

void cleanup_zombie_urbs(int device_fd, int client_fd) {
    std::lock_guard<std::mutex> lock(in_flight_mutex);
    int discarded_count = 0;

    for (auto const& item : active_urbs) {
        async_urb_context* ctx = item.second;
        if (ctx->client_fd == client_fd) {
            ioctl(device_fd, USBDEVFS_DISCARDURB, (void*)&ctx->urb);
            discarded_count++;
        }
    }
    LOGI("Triggered hardware discard for %d zombie URBs.", discarded_count);
}

ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            return (n == 0) ? (ssize_t)total : n;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

void reap_thread(std::string busid, int device_fd, std::shared_ptr<session_context> session) {
    notify_performance_locks(true);
    LOGI("URB Reaper thread started for bus %s.", busid.c_str());
    auto is_connected = session->is_connected;
    while (is_connected->load() || in_flight_urbs_count.load() > 0) {
        struct usbdevfs_urb *urb = nullptr;
        int res = ioctl(device_fd, USBDEVFS_REAPURBNDELAY, &urb);
        if (res < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            LOGE("REAPURB failed on bus %s: %s", busid.c_str(), strerror(errno));
            if (errno == ENODEV) {
                LOGI("reap_thread: Hardware physically detached on bus %s, terminating reaper.", busid.c_str());
                break;
            }

            if (errno == EBADF) {
                LOGW("reap_thread: Device FD invalid on bus %s. Waiting for new FD...", busid.c_str());
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
                LOGW("Endpoint stalled (EPIPE) on bus %s, ep=0x%02x, seq=%u. Clearing halt and reporting to client.",
                     busid.c_str(), ctx->urb.endpoint, ntohl(ctx->seqnum));

                uint32_t ep_addr = ctx->urb.endpoint;
                if (ioctl(device_fd, USBDEVFS_CLEAR_HALT, &ep_addr) < 0) {
                    LOGE("USBDEVFS_CLEAR_HALT failed for ep 0x%02x: %s", ep_addr, strerror(errno));
                }
            } else {
                LOGE("URB failed with status %d (ep=%u, seq=%u) on bus %s", (int)urb->status, ntohl(ctx->ep), ntohl(ctx->seqnum), busid.c_str());
            }
        }

        if (ntohl(ctx->ep) == 0) {
            LOGI("<<< USBIP_RET_SUBMIT (EP0): seq=%u, status=%d, actual_len=%u, bus=%s",
                 ntohl(ctx->seqnum), (int32_t)ntohl(ret.status), (uint32_t)ntohl(ret.actual_length), busid.c_str());
        }

        if (is_connected->load()) {
            if (ctx->udp_fd != -1) {
            // UDP path remains direct (low latency)
            uint8_t out_buf[65536 + 64];
            uint32_t seq_net = htonl(ctx->udp_seq_id);
            memcpy(out_buf, &seq_net, 4);
            memcpy(out_buf + 4, &ret, sizeof(ret));
            size_t out_len = 4 + sizeof(ret);

            if (urb->actual_length > 0 && ntohl(ctx->direction) == 1) {
                uint8_t* data_ptr = ctx->payload_buffer;
                if (ctx->urb.type == USBDEVFS_URB_TYPE_CONTROL) data_ptr += 8;
                memcpy(out_buf + out_len, data_ptr, urb->actual_length);
                out_len += urb->actual_length;
            }
            struct sockaddr_in client_addr = ctx->client_addr;
            sendto(ctx->udp_fd, out_buf, out_len, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
        } else {
                // TCP path now decoupled via TX Queue
                tx_packet* pkt = new tx_packet();
                pkt->header = ret;
                if (urb->actual_length > 0 && ntohl(ctx->direction) == 1) {
                    uint8_t* data_ptr = ctx->payload_buffer;
                    if (ctx->urb.type == USBDEVFS_URB_TYPE_CONTROL) data_ptr += 8;
                    pkt->payload = new uint8_t[urb->actual_length];
                    memcpy(pkt->payload, data_ptr, urb->actual_length);
                    pkt->payload_len = urb->actual_length;
                }
                {
                    std::lock_guard<std::mutex> lock(session->tx_mutex);
                    session->tx_queue.push(pkt);
                }
                session->tx_cv.notify_one();
            }
            LOGI("<<< USBIP_RET_SUBMIT (%s-QUEUED): seq=%u, status=%d, len=%u, bus=%s",
                 (ctx->udp_fd != -1 ? "UDP" : "TCP"),
                 ntohl(ctx->seqnum), (int32_t)ntohl(ret.status), (uint32_t)ntohl(ret.actual_length), busid.c_str());
        }

        delete[] ctx->payload_buffer;
        free(ctx);
    }
    LOGI("URB Reaper thread exiting.");
    notify_performance_locks(false);
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

void udp_worker_loop(int udp_fd, int device_fd, std::vector<endpoint_info> eps, std::string busid, std::shared_ptr<std::atomic<bool>> is_connected, int client_tcp_fd) {
    notify_performance_locks(true);
    LOGI("UDP Worker loop started for bus %s", busid.c_str());

    // Enforce strict non-blocking timeouts
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; // 500ms
    setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in from_addr;
    socklen_t addr_len = sizeof(from_addr);
    uint8_t packet[65536 + 4];
    int timeout_count = 0;

    while (is_connected->load()) {
        struct pollfd pfd;
        pfd.fd = udp_fd;
        pfd.events = POLLIN;

        int poll_ret = poll(&pfd, 1, 500);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            LOGE("UDP poll error on bus %s: %s", busid.c_str(), strerror(errno));
            break;
        }

        if (poll_ret == 0) {
            timeout_count++;
            if (timeout_count > 60) { // 30 seconds of silence
                LOGW("UDP worker: No data for 30s. Terminating UDP path for bus %s.", busid.c_str());
                break;
            }
            continue;
        }

        timeout_count = 0;
        ssize_t n = recvfrom(udp_fd, packet, sizeof(packet), 0, (struct sockaddr*)&from_addr, &addr_len);
        if (n < 4 + (ssize_t)sizeof(struct usbip_header)) continue;

        // Strip 4-byte Sequence ID header
        uint32_t udp_seq_id = ntohl(*(uint32_t*)packet);
        struct usbip_header* header = (struct usbip_header*)(packet + 4);
        uint32_t command = ntohl(header->command);

        if (command == USBIP_CMD_SUBMIT) {
            // Check for device FD update before submitting (Shared lock)
            {
                std::shared_lock<std::shared_mutex> lock(g_devices_rw_mutex);
                if (g_active_devices.count(busid) && g_active_devices[busid] != -1 && g_active_devices[busid] != device_fd) {
                    device_fd = g_active_devices[busid];
                }
            }

            if (device_fd == -1) continue;

            uint32_t ep = ntohl(header->ep) & 0x7F;
            uint32_t dir = ntohl(header->direction);
            uint32_t transfer_len = ntohl(header->transfer_buffer_length);

            auto *ctx = (async_urb_context *)calloc(1, sizeof(async_urb_context));
            if (!ctx) continue;
            ctx->client_fd = client_tcp_fd;
            ctx->udp_fd = udp_fd;
            ctx->client_addr = from_addr;
            ctx->udp_seq_id = udp_seq_id;
            ctx->seqnum = header->seqnum;
            ctx->devid = header->devid;
            ctx->direction = header->direction;
            ctx->ep = header->ep;

            if (ep == 0) {
                ctx->payload_buffer = new uint8_t[8 + (size_t)transfer_len];
                memcpy(ctx->payload_buffer, header->setup, 8);
                if (dir == 0 && transfer_len > 0) {
                    if (n >= (ssize_t)(4 + sizeof(struct usbip_header) + transfer_len)) {
                        memcpy(ctx->payload_buffer + 8, packet + 4 + sizeof(struct usbip_header), transfer_len);
                    }
                }
                ctx->urb.type = USBDEVFS_URB_TYPE_CONTROL;
                ctx->urb.buffer = ctx->payload_buffer;
                ctx->urb.buffer_length = (int)(8 + transfer_len);
            } else {
                if (transfer_len > 0) {
                    ctx->payload_buffer = new uint8_t[transfer_len];
                    if (dir == 0 && n >= (ssize_t)(4 + sizeof(struct usbip_header) + transfer_len)) {
                        memcpy(ctx->payload_buffer, packet + 4 + sizeof(struct usbip_header), transfer_len);
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

            // Synchronous EP0 handling
            if (ep == 0) {
                uint8_t bmRequestType = header->setup[0];
                uint8_t bRequest      = header->setup[1];
                uint16_t wValue = (uint16_t)(header->setup[2] | (header->setup[3] << 8));
                uint16_t wIndex = (uint16_t)(header->setup[4] | (header->setup[5] << 8));
                uint16_t wLength = (uint16_t)transfer_len;

                LOGI("UDP EP0 Control Request: bmRequestType=0x%02x, bRequest=0x%02x, wValue=0x%04x, wIndex=0x%04x, wLength=%u",
                     bmRequestType, bRequest, wValue, wIndex, wLength);

                if (bmRequestType == 0x00 && bRequest == 0x09) { // SET_CONFIGURATION
                    LOGI("UDP: Intercepted SET_CONFIGURATION");
                    for (int i = 0; i < 16; i++) { int intf = i; ioctl(device_fd, USBDEVFS_RELEASEINTERFACE, &intf); }
                    unsigned int config_val = wValue;
                    int res_sc = ioctl(device_fd, USBDEVFS_SETCONFIGURATION, &config_val);
                    for (int i = 0; i < 16; i++) { int intf = i; ioctl(device_fd, USBDEVFS_CLAIMINTERFACE, &intf); }

                    struct usbip_ret_submit ret = {0};
                    ret.command = htonl(USBIP_RET_SUBMIT);
                    ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;
                    ret.status = (res_sc < 0) ? htonl((uint32_t)-errno) : 0;

                    uint8_t out_buf[52];
                    uint32_t s_net = htonl(udp_seq_id);
                    memcpy(out_buf, &s_net, 4);
                    memcpy(out_buf + 4, &ret, sizeof(ret));
                    sendto(udp_fd, out_buf, 52, 0, (struct sockaddr*)&from_addr, sizeof(from_addr));
                    delete[] ctx->payload_buffer; free(ctx); continue;
                } else if (bmRequestType == 0x01 && bRequest == 0x0B) { // SET_INTERFACE
                    LOGI("UDP: Intercepted SET_INTERFACE");
                    struct usbdevfs_setinterface setintf = {0};
                    setintf.interface = wIndex; setintf.altsetting = wValue;
                    int res_si = ioctl(device_fd, USBDEVFS_SETINTERFACE, &setintf);

                    struct usbip_ret_submit ret = {0};
                    ret.command = htonl(USBIP_RET_SUBMIT);
                    ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;
                    ret.status = (res_si < 0) ? htonl((uint32_t)-errno) : 0;

                    uint8_t out_buf[52];
                    uint32_t s_net = htonl(udp_seq_id);
                    memcpy(out_buf, &s_net, 4);
                    memcpy(out_buf + 4, &ret, sizeof(ret));
                    sendto(udp_fd, out_buf, 52, 0, (struct sockaddr*)&from_addr, sizeof(from_addr));
                    delete[] ctx->payload_buffer; free(ctx); continue;
                } else if (bmRequestType == 0x00 && bRequest == 0x05) { // SET_ADDRESS
                    LOGI("UDP: Intercepted SET_ADDRESS");
                    struct usbip_ret_submit ret = {0};
                    ret.command = htonl(USBIP_RET_SUBMIT);
                    ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;

                    uint8_t out_buf[52];
                    uint32_t s_net = htonl(udp_seq_id);
                    memcpy(out_buf, &s_net, 4);
                    memcpy(out_buf + 4, &ret, sizeof(ret));
                    sendto(udp_fd, out_buf, 52, 0, (struct sockaddr*)&from_addr, sizeof(from_addr));
                    delete[] ctx->payload_buffer; free(ctx); continue;
                } else if (bmRequestType == 0x02 && bRequest == 0x01 && wValue == 0x0000) { // CLEAR_FEATURE
                    uint32_t target_endpoint = wIndex & 0xFF;
                    LOGI("UDP: Intercepted CLEAR_FEATURE (ENDPOINT_HALT) for ep 0x%02x", target_endpoint);
                    ioctl(device_fd, USBDEVFS_CLEAR_HALT, &target_endpoint);
                }
            }

            { std::lock_guard<std::mutex> lock(in_flight_mutex); active_urbs[ntohl(ctx->seqnum)] = ctx; in_flight_urbs_count++; }
            if (ioctl(device_fd, USBDEVFS_SUBMITURB, (void*)&ctx->urb) < 0) {
                { std::lock_guard<std::mutex> lock(in_flight_mutex); active_urbs.erase(ntohl(ctx->seqnum)); in_flight_urbs_count--; }
                struct usbip_ret_submit ret_err = {0};
                ret_err.command = htonl(USBIP_RET_SUBMIT);
                ret_err.seqnum = ctx->seqnum; ret_err.devid = ctx->devid; ret_err.direction = ctx->direction; ret_err.ep = ctx->ep;
                ret_err.status = htonl((uint32_t)-errno);

                uint8_t out_buf[52];
                uint32_t s_net = htonl(udp_seq_id);
                memcpy(out_buf, &s_net, 4);
                memcpy(out_buf + 4, &ret_err, sizeof(ret_err));
                sendto(udp_fd, out_buf, 52, 0, (struct sockaddr*)&from_addr, sizeof(from_addr));

                delete[] ctx->payload_buffer; free(ctx);
            }
        }
    }

    LOGI("Gracefully releasing UDP resources for bus %s", busid.c_str());
    close(udp_fd);
    notify_performance_locks(false);
}

void handle_client(int client_fd, int device_fd) {
    notify_performance_locks(true);
    auto is_connected = std::make_shared<std::atomic<bool>>(true);
    auto session = std::make_shared<session_context>(client_fd, is_connected);
    std::thread(tcp_tx_thread, session).detach();

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
        char busid_buf[32];
        if (recv_all(client_fd, busid_buf, 32) < 32) {
            is_connected->store(false);
            return;
        }
        std::string busid(busid_buf, strnlen(busid_buf, 32));

        uint32_t flags = 0;
        struct pollfd pfd_flags;
        pfd_flags.fd = client_fd;
        pfd_flags.events = POLLIN;
        if (poll(&pfd_flags, 1, 100) > 0) {
            recv(client_fd, &flags, sizeof(flags), 0);
            flags = ntohl(flags);
        }

        bool use_udp = (flags & FLAG_REQUEST_UDP) != 0;
        int udp_fd = -1;
        uint16_t udp_port = 0;

        if (use_udp) {
            udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (udp_fd >= 0) {
                struct sockaddr_in udp_addr = {0};
                udp_addr.sin_family = AF_INET;
                udp_addr.sin_addr.s_addr = INADDR_ANY;
                udp_addr.sin_port = htons(3241);
                if (bind(udp_fd, (struct sockaddr*)&udp_addr, sizeof(udp_addr)) < 0) {
                    udp_addr.sin_port = 0;
                    if (bind(udp_fd, (struct sockaddr*)&udp_addr, sizeof(udp_addr)) < 0) {
                        close(udp_fd); udp_fd = -1; use_udp = false;
                    }
                }
                if (udp_fd != -1) {
                    socklen_t slen = sizeof(udp_addr);
                    getsockname(udp_fd, (struct sockaddr*)&udp_addr, &slen);
                    udp_port = ntohs(udp_addr.sin_port);
                    LOGI("Experimental UDP path requested. Bound to port %d", udp_port);
                }
            } else {
                use_udp = false;
            }
        }

        int resolved_fd = get_int_for_busid("getFdForBusId", busid);
        if (resolved_fd == -1) {
            struct op_common err_header = {0};
            err_header.version = htons(USBIP_VERSION);
            err_header.code = htons(OP_REP_IMPORT);
            err_header.status = htonl(1);
            send(client_fd, &err_header, sizeof(err_header), 0);
            is_connected->store(false);
            if (udp_fd != -1) close(udp_fd);
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
        if (use_udp) {
            uint32_t p_net = htonl((uint32_t)udp_port);
            send(client_fd, &p_net, sizeof(p_net), 0);
        }

        std::thread(reap_thread, current_busid, device_fd, session).detach();

        if (use_udp) {
            std::thread(udp_worker_loop, udp_fd, device_fd, eps, busid, is_connected, client_fd).detach();
        }

        for (int i = 0; i < 16; i++) {
            struct usbdevfs_getdriver get_driver = {0};
            get_driver.interface = i;

            if (ioctl(device_fd, USBDEVFS_GETDRIVER, &get_driver) == 0) {
                LOGI("Interface %d has active driver: %s. Detaching...", i, get_driver.driver);
                struct usbdevfs_ioctl disconnect = {0};
                disconnect.ifno = i;
                disconnect.ioctl_code = USBDEVFS_DISCONNECT;
                if (ioctl(device_fd, USBDEVFS_IOCTL, &disconnect) < 0) {
                    LOGE("Failed to detach kernel driver on interface %d: %s", i, strerror(errno));
                }
            }

            int intf = i;
            if (ioctl(device_fd, USBDEVFS_CLAIMINTERFACE, &intf) == 0) {
                LOGI("Successfully claimed interface %d", i);
            }
        }

        while (is_connected->load()) {
            struct usbip_header cmd_header = {0};
            if (recv_all(client_fd, &cmd_header, sizeof(cmd_header)) < (ssize_t)sizeof(cmd_header)) break;

            uint32_t command = ntohl(cmd_header.command);

            if (use_udp && command == USBIP_CMD_SUBMIT) {
                // Skip payload if any (it will be handled via UDP)
                uint32_t tlen = ntohl(cmd_header.transfer_buffer_length);
                if (tlen > 0 && ntohl(cmd_header.direction) == 0) {
                    uint8_t* dummy = new uint8_t[tlen];
                    recv_all(client_fd, dummy, tlen);
                    delete[] dummy;
                }
                continue;
            }

            uint32_t ep = ntohl(cmd_header.ep) & 0x7F;
            uint32_t dir = ntohl(cmd_header.direction);
            uint32_t transfer_len = ntohl(cmd_header.transfer_buffer_length);

            if (command == USBIP_CMD_SUBMIT) {
                // Check for device FD update before submitting (Shared lock)
                {
                    std::shared_lock<std::shared_mutex> lock(g_devices_rw_mutex);
                    if (g_active_devices.count(current_busid) && g_active_devices[current_busid] != -1 && g_active_devices[current_busid] != device_fd) {
                        device_fd = g_active_devices[current_busid];
                    }
                }

                auto *ctx = (async_urb_context *)calloc(1, sizeof(async_urb_context));
                if (!ctx) break;
                ctx->client_fd = client_fd;
                ctx->udp_fd = -1; // TCP path
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
                    uint8_t bmRequestType = cmd_header.setup[0];
                    uint8_t bRequest      = cmd_header.setup[1];
                    uint16_t wValue = (uint16_t)(cmd_header.setup[2] | (cmd_header.setup[3] << 8));
                    uint16_t wIndex = (uint16_t)(cmd_header.setup[4] | (cmd_header.setup[5] << 8));
                    uint16_t wLength = (uint16_t)transfer_len;

                    LOGI("TCP EP0 Control Request: bmRequestType=0x%02x, bRequest=0x%02x, wValue=0x%04x, wIndex=0x%04x, wLength=%u",
                         bmRequestType, bRequest, wValue, wIndex, wLength);

                    if (cmd_header.setup[0] == 0x00 && bRequest == 0x09) {
                        for (int i = 0; i < 16; i++) { int intf = i; ioctl(device_fd, USBDEVFS_RELEASEINTERFACE, &intf); }
                        unsigned int config_val = wValue;
                        int res_sc = ioctl(device_fd, USBDEVFS_SETCONFIGURATION, &config_val);
                        for (int i = 0; i < 16; i++) { int intf = i; ioctl(device_fd, USBDEVFS_CLAIMINTERFACE, &intf); }
                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;
                        ret.status = (res_sc < 0) ? htonl((uint32_t)-errno) : 0;

                        tx_packet* pkt = new tx_packet();
                        pkt->header = ret;
                        {
                            std::lock_guard<std::mutex> lock(session->tx_mutex);
                            session->tx_queue.push(pkt);
                        }
                        session->tx_cv.notify_one();

                        delete[] ctx->payload_buffer; free(ctx); continue;
                    } else if (cmd_header.setup[0] == 0x01 && bRequest == 0x0B) {
                        struct usbdevfs_setinterface setintf = {0};
                        setintf.interface = wIndex; setintf.altsetting = wValue;
                        int res_si = ioctl(device_fd, USBDEVFS_SETINTERFACE, &setintf);
                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;
                        ret.status = (res_si < 0) ? htonl((uint32_t)-errno) : 0;

                        tx_packet* pkt = new tx_packet();
                        pkt->header = ret;
                        {
                            std::lock_guard<std::mutex> lock(session->tx_mutex);
                            session->tx_queue.push(pkt);
                        }
                        session->tx_cv.notify_one();

                        delete[] ctx->payload_buffer; free(ctx); continue;
                    } else if (cmd_header.setup[0] == 0x00 && bRequest == 0x05) {
                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;

                        tx_packet* pkt = new tx_packet();
                        pkt->header = ret;
                        {
                            std::lock_guard<std::mutex> lock(session->tx_mutex);
                            session->tx_queue.push(pkt);
                        }
                        session->tx_cv.notify_one();

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
                if (ioctl(device_fd, USBDEVFS_SUBMITURB, (void*)&ctx->urb) < 0) {
                    { std::lock_guard<std::mutex> lock(in_flight_mutex); active_urbs.erase(ntohl(ctx->seqnum)); in_flight_urbs_count--; }
                    struct usbip_ret_submit ret_err = {0};
                    ret_err.command = htonl(USBIP_RET_SUBMIT);
                    ret_err.seqnum = ctx->seqnum; ret_err.devid = ctx->devid; ret_err.direction = ctx->direction; ret_err.ep = ctx->ep;
                    ret_err.status = htonl((uint32_t)-errno);

                    tx_packet* pkt = new tx_packet();
                    pkt->header = ret_err;
                    {
                        std::lock_guard<std::mutex> lock(session->tx_mutex);
                        session->tx_queue.push(pkt);
                    }
                    session->tx_cv.notify_one();

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
    session->tx_cv.notify_all(); // Wake up TX thread to exit

    { std::lock_guard<std::mutex> lock(g_client_map_mutex); if (g_busid_to_client_fd.count(current_busid) && g_busid_to_client_fd[current_busid] == client_fd) g_busid_to_client_fd.erase(current_busid); }
    cleanup_zombie_urbs(device_fd, client_fd);
    notify_performance_locks(false);
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

        int buf_size = 2 * 1024 * 1024; // 2MB
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

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
