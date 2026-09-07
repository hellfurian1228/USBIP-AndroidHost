#include <jni.h>
#include <android/log.h>
#include <sys/socket.h>
#include <sys/uio.h>
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
#include <new>
#include <deque>
#include <numeric>
#include <cmath>
#include <pthread.h>

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
// Tracks detached worker threads (per-connection handle_client threads,
// reap_thread, tcp_tx_thread) that call JNI methods on
// g_service_obj. stopNativeServer() must wait for this to reach zero before
// deleting the global reference - otherwise a thread still finishing its
// shutdown work (e.g. notify_performance_locks(false)) can dereference an
// already-deleted global ref and crash the process.
static std::atomic<int> g_active_workers{0};

static std::unordered_map<std::string, int> g_active_devices;
static std::shared_mutex g_devices_rw_mutex;
static std::condition_variable_any g_device_update_cv;

// Track which client socket is currently bound to which Bus ID
static std::unordered_map<std::string, int> g_busid_to_client_fd;
static std::mutex g_client_map_mutex;

struct TransferStats {
    std::atomic<uint64_t> bytes_transferred{0};
    uint64_t last_bytes = 0;
    double speed_mbps = 0.0;
    std::chrono::steady_clock::time_point last_tp = std::chrono::steady_clock::now();
};
static std::unordered_map<std::string, TransferStats> g_transfer_stats;
static std::mutex g_stats_mutex;

// --- Throughput / pipeline diagnostics (no payload data is ever logged) ---
// These are process-wide counters (not per-busid) to keep the hot path cheap:
// updates are single atomic ops, and nothing here allocates or touches the
// USB/network payload bytes.
static std::atomic<uint64_t> g_usb_wait_ns_total{0};   // sum of submit->reap latency (pure USB completion wait)
static std::atomic<uint64_t> g_usb_wait_ns_max{0};
static std::atomic<uint64_t> g_usb_wait_samples{0};

static std::atomic<uint64_t> g_tcp_read_wait_ns_total{0};  // time blocked in recv_all() for OUT payload bytes
static std::atomic<uint64_t> g_tcp_read_wait_samples{0};
static std::atomic<uint64_t> g_tcp_write_wait_ns_total{0}; // time blocked in send_all() for IN payload/response bytes
static std::atomic<uint64_t> g_tcp_write_wait_samples{0};

static std::atomic<int> g_in_flight_peak{0};      // peak concurrent in-flight URBs observed
static std::atomic<int> g_tx_queue_depth_peak{0};  // peak pending-response queue depth observed

static std::atomic<uint64_t> g_short_transfer_count{0}; // status==0 but actual_length < requested length
static std::atomic<uint64_t> g_usb_error_count{0};       // urb->status != 0 (excluding normal -ENOENT cancellation)
static std::atomic<uint64_t> g_socket_error_count{0};    // hard recv()/send() failures or write timeouts
static std::atomic<uint64_t> g_submit_retry_count{0};    // reserved: no automatic resubmission exists today; stays 0

template <typename T>
static void update_peak(std::atomic<T>& peak, T value) {
    T prev = peak.load(std::memory_order_relaxed);
    while (value > prev && !peak.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {}
}

void record_transfer_bytes(const std::string& busid, uint32_t bytes) {
    if (bytes == 0) return;
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    g_transfer_stats[busid].bytes_transferred += bytes;
}

struct DeviceTelemetryStats {
    std::string busid;
    std::string name;
    int vendor_id = 0;
    int product_id = 0;
    std::string usb_class;
    bool active = true;
    uint64_t transfer_count = 0;
    uint64_t completed_transfer_count = 0;
    uint64_t failed_transfer_count = 0;
    uint64_t bytes_to_client = 0;
    uint64_t bytes_from_client = 0;

    std::deque<uint64_t> latency_samples;
    uint64_t total_latency_us = 0;
    uint64_t latency_us_max = 0;
    uint64_t last_transfer_unix_ms = 0;
};

static std::unordered_map<std::string, DeviceTelemetryStats> g_telemetry_map;
static std::mutex g_telemetry_mutex;

void record_urb_submission(const std::string& busid, uint32_t vendor_id, uint32_t product_id, const std::string& name) {
    std::lock_guard<std::mutex> lock(g_telemetry_mutex);
    auto& stats = g_telemetry_map[busid];
    stats.busid = busid;
    stats.active = true;
    if (vendor_id > 0) stats.vendor_id = vendor_id;
    if (product_id > 0) stats.product_id = product_id;
    if (!name.empty()) stats.name = name;
    stats.transfer_count++;
}

void record_urb_completion(const std::string& busid, int status, uint32_t direction, uint32_t data_len,
                            int64_t start_time_ns, int64_t usb_wait_ns, uint32_t requested_len) {
    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t latency_us = (now_ns > start_time_ns) ? (uint64_t)((now_ns - start_time_ns) / 1000) : 0;
    uint64_t now_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    if (usb_wait_ns > 0) {
        g_usb_wait_ns_total.fetch_add((uint64_t)usb_wait_ns, std::memory_order_relaxed);
        g_usb_wait_samples.fetch_add(1, std::memory_order_relaxed);
        update_peak(g_usb_wait_ns_max, (uint64_t)usb_wait_ns);
    }

    std::lock_guard<std::mutex> lock(g_telemetry_mutex);
    auto& stats = g_telemetry_map[busid];
    stats.last_transfer_unix_ms = now_wall_ms;

    if (status == 0) {
        stats.completed_transfer_count++;
        stats.total_latency_us += latency_us;
        if (latency_us > stats.latency_us_max) {
            stats.latency_us_max = latency_us;
        }
        stats.latency_samples.push_back(latency_us);
        if (stats.latency_samples.size() > 256) {
            stats.latency_samples.pop_front();
        }
        // A short transfer (actual_length < requested) is a normal way to
        // signal end-of-data on a bulk endpoint, but tracking it helps spot
        // unexpected fragmentation that would otherwise silently multiply
        // the number of USB/IP round trips needed per SCSI command.
        if (requested_len > 0 && data_len < requested_len) {
            g_short_transfer_count.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        stats.failed_transfer_count++;
        if (status != -ENOENT) { // -ENOENT is the expected status for a discarded/cancelled URB, not a real error
            g_usb_error_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (direction == 1) { // IN (host -> client)
        stats.bytes_to_client += data_len;
    } else { // OUT (client -> host)
        stats.bytes_from_client += data_len;
    }
}

uint64_t calculate_jitter(const std::deque<uint64_t>& samples) {
    if (samples.size() < 2) return 0;
    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    double mean = sum / samples.size();
    double sq_sum = 0.0;
    for (auto val : samples) {
        sq_sum += (val - mean) * (val - mean);
    }
    double variance = sq_sum / samples.size();
    return (uint64_t)(std::sqrt(variance) + 0.5);
}

static JavaVM* g_jvm = nullptr;
static jobject g_service_obj = nullptr;

// Cached jmethodIDs for the UsbServerService busid-lookup methods. Looking a
// method up by name via GetMethodID() is a comparatively slow reflective
// string search; these are resolved once (in startNativeServer) instead of
// on every get_int_for_busid() call. jmethodIDs are tied to the class (not
// the instance), so they stay valid for the process lifetime and are safe
// to share across threads without holding a global ref to them.
static jmethodID g_mid_getVid = nullptr;
static jmethodID g_mid_getPid = nullptr;
static jmethodID g_mid_getSpeed = nullptr;
static jmethodID g_mid_getInterfaceCount = nullptr;
static jmethodID g_mid_getFd = nullptr;
static jmethodID g_mid_acquireLocks = nullptr;
static jmethodID g_mid_releaseLocks = nullptr;
static jmethodID g_mid_getPayloadDirect = nullptr;
static jmethodID g_mid_getPayload = nullptr;

// Per-thread JNI attachment caching: attaching a thread to the JVM is
// expensive, and several of our long-lived worker threads (reap_thread,
// tcp_tx_thread, per-connection handle_client threads) previously paid an
// attach+detach round trip for *every* JNI call they made, even when the
// same thread made multiple JNI calls back to back over its lifetime.
// Instead, get_cached_env() attaches a thread at most once and stashes a
// non-null marker in this TLS key. When the thread eventually exits (client
// disconnect, daemon stop, etc.), pthread automatically invokes
// detach_thread_destructor(), which detaches the thread from the JVM at
// that point - so we still never leave a thread attached forever.
static pthread_key_t g_jni_tls_key;

static void detach_thread_destructor(void* arg) {
    // If 'arg' is not null, it means WE attached this thread and must detach it.
    if (arg != nullptr && g_jvm != nullptr) {
        LOGI("Thread exiting: Detaching from JVM.");
        g_jvm->DetachCurrentThread();
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_jvm = vm;
    pthread_key_create(&g_jni_tls_key, detach_thread_destructor);
    return JNI_VERSION_1_6;
}

// Returns a JNIEnv* valid for the calling thread, attaching it to the JVM
// only if it isn't already attached. Do NOT call DetachCurrentThread()
// after using the returned env - the TLS destructor above handles that
// automatically when the thread exits.
JNIEnv* get_cached_env() {
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    int status = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);

    if (status == JNI_EDETACHED) {
#ifdef __ANDROID__
        if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
#else
        if (g_jvm->AttachCurrentThread((void**)&env, nullptr) == JNI_OK) {
#endif
            // Mark this thread in TLS. The value (void*)1 triggers the destructor on exit.
            pthread_setspecific(g_jni_tls_key, (void*)1);
        }
    }
    // If status == JNI_OK, it was already attached (either by us previously, or by Java).
    return env;
}

int get_int_for_busid(jmethodID mid, const std::string& busid) {
    if (!g_jvm || !g_service_obj || !mid) return 0;
    JNIEnv* env = get_cached_env();
    if (!env) return 0;
    jstring jbusid = env->NewStringUTF(busid.c_str());

    int result = 0;
    if (jbusid) {
        result = env->CallIntMethod(g_service_obj, mid, jbusid);
    }

    // Clean up local references immediately to avoid OOM limits in active threads
    if (jbusid) env->DeleteLocalRef(jbusid);
    return result;
}

void notify_performance_locks(bool acquire) {
    if (!g_jvm || !g_service_obj) return;
    JNIEnv* env = get_cached_env();
    if (!env) return;
    jmethodID mid = acquire ? g_mid_acquireLocks : g_mid_releaseLocks;
    if (mid) env->CallVoidMethod(g_service_obj, mid);
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

enum class RequestState {
    PENDING,
    UNLINKING,
    COMPLETED
};

struct async_urb_context;

struct PendingRequest {
    uint32_t submit_seqnum = 0;   // Original submit sequence number (network order)
    uint32_t unlink_seqnum = 0;   // Unlink command sequence number (network order)
    uint32_t devid = 0;
    uint32_t direction = 0;
    uint32_t ep = 0;
    RequestState state = RequestState::PENDING;
    int64_t start_time_ns = 0;
    std::string busid;
    async_urb_context* ctx = nullptr;
};

struct session_context : public std::enable_shared_from_this<session_context> {
    int client_fd;
    std::shared_ptr<std::atomic<bool>> is_connected;
    std::queue<tx_packet*> tx_queue;
    std::mutex tx_mutex;
    std::condition_variable tx_cv;

    std::mutex request_mutex;
    std::unordered_map<uint32_t, PendingRequest*> pending_requests;

    session_context(int fd, std::shared_ptr<std::atomic<bool>> conn)
            : client_fd(fd), is_connected(conn) {}

    ~session_context() {
        std::lock_guard<std::mutex> lock(request_mutex);
        for (auto& pair : pending_requests) {
            delete pair.second;
        }
        pending_requests.clear();
    }

    void enqueue_response(tx_packet* pkt) {
        std::lock_guard<std::mutex> lock(tx_mutex);
        tx_queue.push(pkt);
        update_peak(g_tx_queue_depth_peak, (int)tx_queue.size());
        tx_cv.notify_one();
    }
};

struct async_urb_context {
    std::shared_ptr<session_context> session;
    PendingRequest* req = nullptr;
    int client_fd = -1;
    uint32_t seqnum = 0;
    uint32_t devid = 0;
    uint32_t direction = 0;
    uint32_t ep = 0;
    uint8_t* payload_buffer = nullptr;
    int64_t start_time_ns = 0;
    int64_t submit_time_ns = 0; // set immediately before USBDEVFS_SUBMITURB; isolates pure USB wait time
    struct usbdevfs_urb urb; // Must be at the end

    async_urb_context() {
        memset(&urb, 0, sizeof(urb));
    }
};

// Forward declaration of send_all for use in Session TX Queueing
ssize_t send_all(int fd, const void *buf, size_t len);
bool send_iovec_all(int sockfd, const void* header, size_t header_len,
                     const void* payload, size_t payload_len, int timeout_ms);

void tcp_tx_thread(std::shared_ptr<session_context> ctx) {
    g_active_workers++;
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
            auto wait_start = std::chrono::steady_clock::now();
            bool ok;
            if (pkt->payload_len > 0 && pkt->payload) {
                // Send header + payload in one syscall (writev/sendmsg) instead
                // of allocating a temporary buffer and copying the payload
                // into it - removes a per-response heap allocation and a full
                // payload copy from the hot read-response path.
                ok = send_iovec_all(ctx->client_fd, &pkt->header, sizeof(pkt->header),
                                     pkt->payload, pkt->payload_len, 5000);
            } else {
                ok = send_all(ctx->client_fd, &pkt->header, sizeof(pkt->header)) == (ssize_t)sizeof(pkt->header);
            }
            auto wait_end = std::chrono::steady_clock::now();
            uint64_t wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wait_end - wait_start).count();
            g_tcp_write_wait_ns_total.fetch_add(wait_ns, std::memory_order_relaxed);
            g_tcp_write_wait_samples.fetch_add(1, std::memory_order_relaxed);

            if (!ok) {
                LOGE("TX Thread: Failed to send packet to fd %d", ctx->client_fd);
                ctx->is_connected->store(false);
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
    g_active_workers--;
}
// --- End TX Queueing ---

std::atomic<int> in_flight_urbs_count{0};
std::mutex in_flight_mutex;
std::unordered_map<uint32_t, async_urb_context*> active_urbs;

void cleanup_zombie_urbs(int device_fd, std::shared_ptr<session_context> session) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(session->request_mutex);
    int discarded_count = 0;

    for (auto const& item : session->pending_requests) {
        PendingRequest* req = item.second;
        if (req && req->ctx) {
            ioctl(device_fd, USBDEVFS_DISCARDURB, (void*)&req->ctx->urb);
            discarded_count++;
        }
    }
    LOGI("connection=%d Triggered hardware discard for %d zombie URBs.", session->client_fd, discarded_count);
}

/**
 * Guarantees the transmission of a full payload over a non-blocking TCP socket.
 *
 * @param sockfd The non-blocking file descriptor for the socket.
 * @param payload Pointer to the start of the URB payload buffer.
 * @param total_len The exact number of bytes to send (e.g., 65536).
 * @param timeout_ms Maximum time to wait for the socket buffer to clear (e.g., 5000).
 * @return true if all bytes were successfully sent, false if the connection dropped or timed out.
 */
bool send_full_urb_payload(int sockfd, const uint8_t* payload, size_t total_len, int timeout_ms = 5000) {
    if (!payload || total_len == 0) return true;
    size_t bytes_sent = 0;

    while (bytes_sent < total_len) {
        // Use MSG_NOSIGNAL to prevent the OS from killing the app with SIGPIPE if the client disconnects
        ssize_t result = send(sockfd, payload + bytes_sent, total_len - bytes_sent, MSG_NOSIGNAL);

        if (result > 0) {
            // Successfully wrote some (or all) bytes to the kernel buffer
            bytes_sent += (size_t)result;
        }
        else if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // The kernel TCP socket buffer is completely full.
                // We must wait for the Windows client to ACK packets to clear space.
                struct pollfd pfd;
                pfd.fd = sockfd;
                pfd.events = POLLOUT; // Wake up when we can write data again

                int poll_res = poll(&pfd, 1, timeout_ms);

                if (poll_res > 0) {
                    // Socket is writable again. Loop around and retry the send().
                    continue;
                }
                else if (poll_res == 0) {
                    // Windows client stopped responding and didn't clear the buffer in time.
                    LOGE("[ERROR] Timeout waiting for socket buffer to clear on fd %d.", sockfd);
                    g_socket_error_count.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                else {
                    if (errno == EINTR) continue; // Interrupted by an OS signal, retry.
                    LOGE("[ERROR] poll() failed on fd %d: %s (errno %d)", sockfd, strerror(errno), errno);
                    g_socket_error_count.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
            }
            else if (errno == EINTR) {
                // The send() call was interrupted by an OS signal before any data was written.
                continue;
            }
            else {
                // A hard error occurred (e.g., ECONNRESET meaning the Windows client disconnected).
                LOGE("[ERROR] Socket write failed on fd %d with errno: %d (%s)", sockfd, errno, strerror(errno));
                g_socket_error_count.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        else {
            // send() returned 0, which is highly abnormal for TCP. Treat as connection loss.
            g_socket_error_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    // Successfully looped until every single byte was handed to the kernel
    return true;
}

/**
 * Scatter-gather variant of send_full_urb_payload(): sends a usbip_ret_submit
 * header and its (optional) payload buffer in a single writev() syscall
 * without concatenating them into a temporary heap buffer first. This avoids
 * an allocation + full-payload memcpy on every USBIP_RET_SUBMIT response that
 * carries IN (device-to-client) data - the hot path for read-heavy transfers.
 */
bool send_iovec_all(int sockfd, const void* header, size_t header_len,
                     const void* payload, size_t payload_len, int timeout_ms = 5000) {
    struct iovec iov[2];
    int iov_count = 0;
    iov[iov_count].iov_base = const_cast<void*>(header);
    iov[iov_count].iov_len = header_len;
    iov_count++;
    if (payload && payload_len > 0) {
        iov[iov_count].iov_base = const_cast<void*>(payload);
        iov[iov_count].iov_len = payload_len;
        iov_count++;
    }

    size_t total_len = header_len + payload_len;
    size_t bytes_sent = 0;

    while (bytes_sent < total_len) {
        struct msghdr msg = {0};
        msg.msg_iov = iov;
        msg.msg_iovlen = iov_count;
        ssize_t result = sendmsg(sockfd, &msg, MSG_NOSIGNAL);

        if (result > 0) {
            bytes_sent += (size_t)result;
            // Advance the iovec array past the bytes already sent, so a
            // partial write resumes correctly instead of resending data.
            size_t remaining = (size_t)result;
            for (int i = 0; i < iov_count; ) {
                if (remaining >= iov[i].iov_len) {
                    remaining -= iov[i].iov_len;
                    iov[i].iov_len = 0;
                    iov[i].iov_base = nullptr;
                    i++;
                } else {
                    iov[i].iov_base = (uint8_t*)iov[i].iov_base + remaining;
                    iov[i].iov_len -= remaining;
                    remaining = 0;
                    break; // Fully consumed
                }
            }

            // Compact the iov array so the next sendmsg() call only sees
            // iovecs with data left to send. Using a fresh index/copy loop
            // (instead of shifting iov[0] = iov[1] in place) avoids reading
            // past the last populated slot when iov_count drops to 1.
            int new_iov_count = 0;
            for (int i = 0; i < iov_count; i++) {
                if (iov[i].iov_len > 0) {
                    iov[new_iov_count++] = iov[i];
                }
            }
            iov_count = new_iov_count;
        }
        else if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd;
                pfd.fd = sockfd;
                pfd.events = POLLOUT;
                int poll_res = poll(&pfd, 1, timeout_ms);
                if (poll_res > 0) continue;
                if (poll_res == 0) {
                    LOGE("[ERROR] Timeout waiting for socket buffer to clear on fd %d.", sockfd);
                    g_socket_error_count.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                if (errno == EINTR) continue;
                LOGE("[ERROR] poll() failed on fd %d: %s (errno %d)", sockfd, strerror(errno), errno);
                g_socket_error_count.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else if (errno == EINTR) {
                continue;
            } else {
                LOGE("[ERROR] Socket writev failed on fd %d with errno: %d (%s)", sockfd, errno, strerror(errno));
                g_socket_error_count.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        else {
            g_socket_error_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    return true;
}

ssize_t send_all(int fd, const void *buf, size_t len) {
    if (!buf || len == 0) return 0;
    return send_full_urb_payload(fd, static_cast<const uint8_t*>(buf), len) ? static_cast<ssize_t>(len) : -1;
}

void reap_thread(std::string busid, int device_fd, std::shared_ptr<session_context> session) {
    g_active_workers++;
    notify_performance_locks(true);
    LOGI("URB Reaper thread started for bus %s.", busid.c_str());
    auto is_connected = session->is_connected;
    while (is_connected->load() || in_flight_urbs_count.load() > 0) {
        struct usbdevfs_urb *urb = nullptr;
        int res = ioctl(device_fd, USBDEVFS_REAPURBNDELAY, &urb);
        if (res < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                // PERFORMANCE: previously this busy-polled with a fixed 1ms
                // sleep between USBDEVFS_REAPURBNDELAY attempts. USB Mass
                // Storage Bulk-Only Transport only ever has one outstanding
                // data phase at a time, so the Windows client cannot submit
                // its next request until it receives our RET_SUBMIT for this
                // one - meaning this reap latency was added directly to the
                // round-trip time of *every* USB transfer and capped
                // achievable throughput (chunk_size / round_trip_latency).
                // The usbfs char device supports poll(): it reports POLLOUT
                // as soon as a submitted URB completes and is ready to reap
                // (see drivers/usb/core/devio.c usbdev_poll(), the same
                // mechanism libusb's async I/O relies on). Waiting on that
                // event wakes us within the kernel's scheduling latency
                // (typically low microseconds) instead of up to 1ms late,
                // with no correctness change: REAPURBNDELAY is still called
                // to actually drain the completion, and the short poll
                // timeout still lets us re-check the exit condition.
                struct pollfd pfd;
                pfd.fd = device_fd;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                poll(&pfd, 1, 50);
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

        auto urb_session = ctx->session ? ctx->session : session;
        PendingRequest* req = ctx->req;
        RequestState state_before = RequestState::PENDING;
        bool emit_ret_submit = false;
        bool emit_ret_unlink = false;
        uint32_t response_seqnum = 0;

        if (urb_session && req) {
            std::lock_guard<std::mutex> lock(urb_session->request_mutex);
            state_before = req->state;
            if (req->state == RequestState::PENDING) {
                req->state = RequestState::COMPLETED;
                emit_ret_submit = true;
                response_seqnum = req->submit_seqnum;
                urb_session->pending_requests.erase(ntohl(req->submit_seqnum));
            } else if (req->state == RequestState::UNLINKING) {
                req->state = RequestState::COMPLETED;
                emit_ret_unlink = true;
                response_seqnum = req->unlink_seqnum;
                urb_session->pending_requests.erase(ntohl(req->submit_seqnum));
            } else {
                LOGI("connection=%d event=reap_duplicate reqSeq=%u ignored", urb_session->client_fd, ntohl(req->submit_seqnum));
            }
        }

        {
            std::lock_guard<std::mutex> lock(in_flight_mutex);
            in_flight_urbs_count--;
            // The submission path inserts every submitted URB into
            // active_urbs keyed by seqnum, but nothing previously erased the
            // entry on completion. Since ctx is deleted shortly after this
            // block, the map accumulated a dangling pointer per completed
            // URB - unbounded native memory growth plus a use-after-free
            // hazard for anything that looked the entry up.
            active_urbs.erase(ntohl(ctx->seqnum));
        }

        struct usbip_ret_submit ret = {0};
        if (urb->actual_length > 0) {
            record_transfer_bytes(busid, urb->actual_length);
        }

        if (emit_ret_submit) {
            int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t usb_wait_ns = (ctx->submit_time_ns > 0 && now_ns > ctx->submit_time_ns) ? (now_ns - ctx->submit_time_ns) : 0;
            record_urb_completion(busid, urb->status, ntohl(ctx->direction), urb->actual_length, ctx->start_time_ns,
                                  usb_wait_ns, (uint32_t)ctx->urb.buffer_length);

            LOGI("connection=%d event=completion submitSeq=%u stateBefore=PENDING stateAfter=COMPLETED response=RET_SUBMIT status=%d",
                 urb_session->client_fd, ntohl(response_seqnum), (int)urb->status);

            ret.command = htonl(USBIP_RET_SUBMIT);
            ret.seqnum = response_seqnum;
            ret.devid = ctx->devid;
            ret.direction = ctx->direction;
            ret.ep = ctx->ep;
            ret.status = htonl((uint32_t)urb->status);
            ret.actual_length = htonl(urb->actual_length);
            ret.number_of_packets = htonl(0xFFFFFFFF);

            if (urb->status != 0 && urb->status != -ENOENT) {
                if (urb->status == -EPIPE) {
                    uint32_t ep_num = ctx->urb.endpoint & 0x7F;
                    if (ep_num != 0) { // Do NOT call CLEAR_HALT on Control EP0!
                        LOGW("Endpoint stalled (EPIPE) on bus %s, ep=0x%02x, seq=%u. Clearing halt.",
                             busid.c_str(), ctx->urb.endpoint, ntohl(ctx->seqnum));

                        uint32_t ep_addr = ctx->urb.endpoint;
                        if (ioctl(device_fd, USBDEVFS_CLEAR_HALT, &ep_addr) < 0) {
                            LOGE("USBDEVFS_CLEAR_HALT failed for ep 0x%02x: %s", ep_addr, strerror(errno));
                        }
                    } else {
                        LOGI("Control Endpoint EP0 stalled (EPIPE) on bus %s, seq=%u (normal protocol stall).",
                             busid.c_str(), ntohl(ctx->seqnum));
                    }
                } else {
                    LOGE("URB failed with status %d (ep=%u, seq=%u) on bus %s", (int)urb->status, ntohl(ctx->ep), ntohl(ctx->seqnum), busid.c_str());
                }
            }

            if (ntohl(ctx->ep) == 0) {
                LOGI("<<< USBIP_RET_SUBMIT (EP0): seq=%u, status=%d, actual_len=%u, bus=%s",
                     ntohl(ctx->seqnum), (int32_t)ntohl(ret.status), (uint32_t)ntohl(ret.actual_length), busid.c_str());

                if (urb->actual_length > 0 && ntohl(ctx->direction) == 1 && ctx->urb.type == USBDEVFS_URB_TYPE_CONTROL) {
                    uint8_t* setup = ctx->payload_buffer;
                    uint8_t* data_ptr = ctx->payload_buffer + 8;

                    if (setup[0] == 0x80 && setup[1] == 0x06) { // GET_DESCRIPTOR
                        uint8_t desc_type = setup[3];

                        if (desc_type == 0x01 && urb->actual_length >= 18) { // DEVICE DESCRIPTOR
                            uint16_t bcdUSB = data_ptr[2] | (data_ptr[3] << 8);
                            LOGI("Device Descriptor: bcdUSB=0x%04x (USB %d.%d)",
                                 bcdUSB, (bcdUSB >> 8) & 0xFF, (bcdUSB >> 4) & 0x0F);

                            if (bcdUSB >= 0x0300) {
                                LOGI("SuperSpeed USB 3.0+ device detected (bcdUSB=0x%04x). Preserving SuperSpeed descriptors.", bcdUSB);
                            } else {
                                LOGI("Low/Full/High speed device detected (bcdUSB=0x%04x). Preserving standard descriptors unmodified.", bcdUSB);
                            }
                        } else if (desc_type == 0x02 && urb->actual_length >= 9) { // CONFIGURATION DESCRIPTOR
                            int pos = 0;
                            bool has_ss_companion = false;
                            while (pos + 1 < urb->actual_length) {
                                uint8_t d_len = data_ptr[pos];
                                if (d_len < 2 || pos + d_len > urb->actual_length) break;

                                uint8_t d_type = data_ptr[pos + 1];
                                if (d_type == 0x30) { // USB_DT_SS_ENDPOINT_COMP
                                    has_ss_companion = true;
                                }
                                pos += d_len;
                            }
                            if (has_ss_companion) {
                                LOGI("Configuration descriptor includes SuperSpeed Endpoint Companion Descriptors (0x30). Forwarding unmodified.");
                            } else {
                                LOGI("Configuration descriptor forwarded unmodified.");
                            }
                        }
                    }
                }
            }

            if (urb_session->is_connected->load()) {
                tx_packet* pkt = new tx_packet();
                pkt->header = ret;
                if (urb->actual_length > 0 && ntohl(ctx->direction) == 1) {
                    uint8_t* data_ptr = ctx->payload_buffer;
                    if (ctx->urb.type == USBDEVFS_URB_TYPE_CONTROL) data_ptr += 8;
                    pkt->payload = new uint8_t[urb->actual_length];
                    memcpy(pkt->payload, data_ptr, urb->actual_length);
                    pkt->payload_len = urb->actual_length;
                }
                urb_session->enqueue_response(pkt);
                LOGI("<<< USBIP_RET_SUBMIT (TCP-QUEUED): seq=%u, status=%d, len=%u, bus=%s",
                     ntohl(ctx->seqnum), (int32_t)ntohl(ret.status), (uint32_t)ntohl(ret.actual_length), busid.c_str());
            }

        } else if (emit_ret_unlink) {
            record_urb_completion(busid, urb->status, ntohl(ctx->direction), urb->actual_length, ctx->start_time_ns, 0, 0);

            LOGI("connection=%d event=completion_unlinked unlinkSeq=%u targetSubmitSeq=%u stateBefore=UNLINKING stateAfter=COMPLETED response=RET_UNLINK status=-2",
                 urb_session->client_fd, ntohl(response_seqnum), req ? ntohl(req->submit_seqnum) : 0);

            struct usbip_ret_submit ret = {0};
            ret.command = htonl(USBIP_RET_UNLINK);
            ret.seqnum = response_seqnum; // CMD_UNLINK.seqnum
            ret.status = htonl((uint32_t)-2); // -2 (cancellation status)

            if (urb_session->is_connected->load()) {
                tx_packet* pkt = new tx_packet();
                pkt->header = ret;
                urb_session->enqueue_response(pkt);
            }
        }

        delete req;
        delete[] ctx->payload_buffer;
        delete ctx;
    }
    LOGI("URB Reaper thread exiting.");
    notify_performance_locks(false);
    g_active_workers--;
}

ssize_t recv_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    auto *p = (char *)buf;
    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, MSG_WAITALL);
        if (n < 0) {
            g_socket_error_count.fetch_add(1, std::memory_order_relaxed);
            return n;
        }
        if (n == 0) return (ssize_t)total;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

// Same as recv_all(), but additionally records how long we were blocked
// waiting for the USBIP OUT-direction payload bytes to arrive over TCP -
// used for the "time spent waiting for TCP reads" throughput diagnostic.
ssize_t recv_all_timed(int fd, void *buf, size_t len) {
    auto t0 = std::chrono::steady_clock::now();
    ssize_t result = recv_all(fd, buf, len);
    auto t1 = std::chrono::steady_clock::now();
    g_tcp_read_wait_ns_total.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(),
        std::memory_order_relaxed);
    g_tcp_read_wait_samples.fetch_add(1, std::memory_order_relaxed);
    return result;
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

    // Fetch the actual speed dynamically from Android
    std::string s_busid(busid);
    int real_speed = get_int_for_busid(g_mid_getSpeed, s_busid);
    dev->speed = (real_speed > 0) ? htonl((uint32_t)real_speed) : htonl(3);

    if (device_fd == -1 || ioctl(device_fd, USBDEVFS_CONTROL, &ctrl) < 0) {
        std::string s_busid(busid);
        dev->idVendor = htons((uint16_t)get_int_for_busid(g_mid_getVid, s_busid));
        dev->idProduct = htons((uint16_t)get_int_for_busid(g_mid_getPid, s_busid));
        dev->bcdDevice = htons(0x0111);
        dev->bDeviceClass = 0;
        dev->bDeviceSubClass = 0;
        dev->bDeviceProtocol = 0;
        dev->bConfigurationValue = 1;
        dev->bNumConfigurations = 1;
        dev->bNumInterfaces = (uint8_t)get_int_for_busid(g_mid_getInterfaceCount, s_busid);
        if (dev->bNumInterfaces == 0) dev->bNumInterfaces = 1;

        intfs->clear();
        struct usbip_usb_interface i = {0};
        i.bInterfaceClass = 3;
        intfs->push_back(i);
        return;
    }

    uint16_t bcdUSB = desc.bcdUSB;
    if (bcdUSB >= 0x0300 && real_speed < 5) {
        real_speed = 5; // Promote to USBIP_SPEED_SUPER (5)
    }
    dev->speed = (real_speed > 0) ? htonl((uint32_t)real_speed) : htonl(3);

    dev->idVendor = htons(desc.idVendor);
    dev->idProduct = htons(desc.idProduct);
    dev->bcdDevice = htons(desc.bcdDevice);
    dev->bDeviceClass = desc.bDeviceClass;
    dev->bDeviceSubClass = desc.bDeviceSubClass;
    dev->bDeviceProtocol = desc.bDeviceProtocol;
    dev->bConfigurationValue = 1;
    dev->bNumConfigurations = (desc.bNumConfigurations > 0) ? desc.bNumConfigurations : 1;

    intfs->clear();
    eps->clear();
    std::vector<uint8_t> config_desc(4096);
    ctrl.wValue = 0x0200; // GET_DESCRIPTOR CONFIGURATION 0
    ctrl.wLength = 9; // Read header first to discover wTotalLength
    ctrl.data = config_desc.data();
    int len = ioctl(device_fd, USBDEVFS_CONTROL, &ctrl);
    if (len >= 9) {
        uint16_t wTotalLength = config_desc[2] | (config_desc[3] << 8);
        if (wTotalLength > 9) {
            ctrl.wLength = (wTotalLength <= (uint16_t)config_desc.size()) ? wTotalLength : (uint16_t)config_desc.size();
            len = ioctl(device_fd, USBDEVFS_CONTROL, &ctrl);
        }
    }

    if (len >= 9) {
        int pos = 0;
        while (pos + 1 < len) {
            uint8_t d_len = config_desc[(size_t)pos];
            if (d_len < 2 || pos + d_len > len) break;
            uint8_t d_type = config_desc[(size_t)pos + 1];
            if (d_type == 0x04 && d_len >= 9) { // Interface Descriptor
                struct usbip_usb_interface i = {0};
                i.bInterfaceClass = config_desc[(size_t)pos + 5];
                i.bInterfaceSubClass = config_desc[(size_t)pos + 6];
                i.bInterfaceProtocol = config_desc[(size_t)pos + 7];
                intfs->push_back(i);
            } else if (d_type == 0x05 && d_len >= 7) { // Endpoint Descriptor
                endpoint_info info_item = {0};
                info_item.addr = config_desc[(size_t)pos + 2];
                info_item.type = config_desc[(size_t)pos + 3] & 0x03;
                info_item.max_packet_size = (uint16_t)(config_desc[(size_t)pos + 4] | (config_desc[(size_t)pos + 5] << 8));
                eps->push_back(info_item);
            } else if (d_type == 0x30 && d_len >= 6) { // SuperSpeed Endpoint Companion Descriptor
                if (!eps->empty()) {
                    uint8_t bMaxBurst = config_desc[(size_t)pos + 2];
                    LOGI("SuperSpeed Endpoint Companion Descriptor (0x30): ep=0x%02x, bMaxBurst=%d",
                         eps->back().addr, bMaxBurst);
                }
            }
            pos += (int)d_len;
        }
    }
    dev->bNumInterfaces = (uint8_t)intfs->size();
    if (dev->bNumInterfaces == 0) dev->bNumInterfaces = 1;
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
        notify_performance_locks(false);
        return;
    }

    uint16_t code = ntohs(header.code);
    if (code == OP_REQ_DEVLIST) {
        LOGI("Handling OP_REQ_DEVLIST via Zero-Copy JNI Direct Buffer");
        JNIEnv* env = get_cached_env();
        if (!env) { is_connected->store(false); notify_performance_locks(false); return; }
        jobject jbuffer = nullptr;
        if (g_mid_getPayloadDirect) {
            jbuffer = env->CallObjectMethod(g_service_obj, g_mid_getPayloadDirect);
        } else if (g_mid_getPayload) {
            jbyteArray jpayload = (jbyteArray)env->CallObjectMethod(g_service_obj, g_mid_getPayload);
            struct op_common reply_header = {0};
            reply_header.version = htons(USBIP_VERSION);
            reply_header.code = htons(OP_REP_DEVLIST);
            reply_header.status = htonl(0);

            if (send_all(client_fd, &reply_header, sizeof(reply_header)) >= 0 && jpayload) {
                jsize len = env->GetArrayLength(jpayload);
                jbyte* body = env->GetByteArrayElements(jpayload, nullptr);
                send_all(client_fd, body, (size_t)len);
                env->ReleaseByteArrayElements(jpayload, body, JNI_ABORT);
                env->DeleteLocalRef(jpayload);
            }
            is_connected->store(false);
            notify_performance_locks(false);
            return;
        }

        struct op_common reply_header = {0};
        reply_header.version = htons(USBIP_VERSION);
        reply_header.code = htons(OP_REP_DEVLIST);
        reply_header.status = htonl(0);

        if (send_all(client_fd, &reply_header, sizeof(reply_header)) < 0) {
            LOGE("OP_REQ_DEVLIST: Failed to send header");
        } else if (jbuffer) {
            void* direct_addr = env->GetDirectBufferAddress(jbuffer);
            jlong capacity = env->GetDirectBufferCapacity(jbuffer);
            if (direct_addr && capacity > 0) {
                send_all(client_fd, direct_addr, (size_t)capacity);
                LOGI("OP_REQ_DEVLIST: Zero-copy sent %lld bytes from direct buffer", (long long)capacity);
            }
            env->DeleteLocalRef(jbuffer);
        }
        is_connected->store(false);
        notify_performance_locks(false);
        return;
    }

    if (code == OP_REQ_IMPORT) {
        char busid_buf[32];
        if (recv_all(client_fd, busid_buf, 32) < 32) {
            is_connected->store(false);
            notify_performance_locks(false);
            return;
        }
        std::string busid(busid_buf, strnlen(busid_buf, 32));

        // UDP flag polling removed. Direct TCP resolution.
        int resolved_fd = get_int_for_busid(g_mid_getFd, busid);
        if (resolved_fd == -1) {
            struct op_common err_header = {0};
            err_header.version = htons(USBIP_VERSION);
            err_header.code = htons(OP_REP_IMPORT);
            err_header.status = htonl(1);
            send_all(client_fd, &err_header, sizeof(err_header));
            is_connected->store(false);
            notify_performance_locks(false);
            return;
        }
        device_fd = resolved_fd;
        current_busid = busid;
        {
            std::lock_guard<std::mutex> lock(g_client_map_mutex);
            g_busid_to_client_fd[current_busid] = client_fd;
        }

        struct usbip_usb_device dev = {0};
        std::vector<struct usbip_usb_interface> intfs;
        std::vector<endpoint_info> eps;
        get_device_info(device_fd, &dev, &intfs, &eps, busid.c_str());

        struct op_common reply_header = {0};
        reply_header.version = htons(USBIP_VERSION);
        reply_header.code = htons(OP_REP_IMPORT);
        reply_header.status = htonl(0);

        send_all(client_fd, &reply_header, sizeof(reply_header));
        send_all(client_fd, &dev, sizeof(dev));

        std::thread(reap_thread, current_busid, device_fd, session).detach();

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

            uint32_t ep = ntohl(cmd_header.ep) & 0x7F;
            uint32_t dir = ntohl(cmd_header.direction);
            uint32_t transfer_len = ntohl(cmd_header.transfer_buffer_length);

            // OOM protection: reject absurd transfer_buffer_length values
            // before they reach an allocation. A malformed/malicious SUBMIT
            // header could otherwise request a huge or negative-as-unsigned
            // allocation and crash the process (std::bad_alloc is fatal here
            // since none of the call sites below expect it).
            if (transfer_len > 16777216) {
                LOGE("TCP: Oversized transfer_len rejected: %u", transfer_len);
                break; // Break the TCP loop instead of continue
            }

            if (command == USBIP_CMD_SUBMIT) {
                // Check for device FD update before submitting (Shared lock)
                {
                    std::shared_lock<std::shared_mutex> lock(g_devices_rw_mutex);
                    if (g_active_devices.count(current_busid) && g_active_devices[current_busid] != -1 && g_active_devices[current_busid] != device_fd) {
                        device_fd = g_active_devices[current_busid];
                    }
                }

                auto *ctx = new async_urb_context();
                ctx->session = session;
                ctx->start_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                record_urb_submission(current_busid, ntohs(dev.idVendor), ntohs(dev.idProduct), "");
                ctx->client_fd = client_fd;
                ctx->seqnum = cmd_header.seqnum;
                ctx->devid = cmd_header.devid;
                ctx->direction = cmd_header.direction;
                ctx->ep = cmd_header.ep;

                PendingRequest* req = new PendingRequest();
                req->submit_seqnum = cmd_header.seqnum;
                req->devid = cmd_header.devid;
                req->direction = cmd_header.direction;
                req->ep = cmd_header.ep;
                req->state = RequestState::PENDING;
                req->busid = current_busid;
                req->ctx = ctx;
                ctx->req = req;

                {
                    std::lock_guard<std::mutex> lock(session->request_mutex);
                    session->pending_requests[ntohl(cmd_header.seqnum)] = req;
                }

                if (ep == 0) {
                    ctx->payload_buffer = new (std::nothrow) uint8_t[8 + (size_t)transfer_len];
                    if (!ctx->payload_buffer) {
                        LOGE("TCP OOM: Failed to allocate %u bytes", transfer_len);
                        {
                            std::lock_guard<std::mutex> lock(session->request_mutex);
                            session->pending_requests.erase(ntohl(cmd_header.seqnum));
                        }
                        delete req; delete ctx;
                        break;
                    }
                    memcpy(ctx->payload_buffer, cmd_header.setup, 8);
                    if (dir == 0 && transfer_len > 0) {
                        recv_all_timed(client_fd, ctx->payload_buffer + 8, transfer_len);
                    }
                    ctx->urb.type = USBDEVFS_URB_TYPE_CONTROL;
                    ctx->urb.buffer = ctx->payload_buffer;
                    ctx->urb.buffer_length = (int)(8 + transfer_len);
                } else {
                    if (transfer_len > 0) {
                        ctx->payload_buffer = new (std::nothrow) uint8_t[transfer_len];
                        if (!ctx->payload_buffer) {
                            LOGE("TCP OOM: Failed to allocate %u bytes", transfer_len);
                            {
                                std::lock_guard<std::mutex> lock(session->request_mutex);
                                session->pending_requests.erase(ntohl(cmd_header.seqnum));
                            }
                            delete req; delete ctx;
                            break;
                        }
                        if (dir == 0) {
                            recv_all_timed(client_fd, ctx->payload_buffer, transfer_len);
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
                        int config = wValue & 0xFF;
                        LOGI("TCP: Executing SET_CONFIGURATION (config=%d)", config);

                        int res_sc = ioctl(device_fd, USBDEVFS_SETCONFIGURATION, &config);
                        if (res_sc < 0) {
                            LOGW("USBDEVFS_SETCONFIGURATION failed: %s (errno=%d)", strerror(errno), errno);
                        } else {
                            LOGI("USBDEVFS_SETCONFIGURATION succeeded for config %d", config);
                        }

                        // Re-detach active drivers and re-claim interfaces after setting configuration
                        for (int i = 0; i < 16; i++) {
                            struct usbdevfs_getdriver get_driver = {0};
                            get_driver.interface = i;
                            if (ioctl(device_fd, USBDEVFS_GETDRIVER, &get_driver) == 0) {
                                struct usbdevfs_ioctl disconnect = {0};
                                disconnect.ifno = i;
                                disconnect.ioctl_code = USBDEVFS_DISCONNECT;
                                ioctl(device_fd, USBDEVFS_IOCTL, &disconnect);
                            }
                            int intf = i;
                            if (ioctl(device_fd, USBDEVFS_CLAIMINTERFACE, &intf) == 0) {
                                LOGI("Re-claimed interface %d after SET_CONFIGURATION", i);
                            }
                        }

                        {
                            std::lock_guard<std::mutex> lock(session->request_mutex);
                            session->pending_requests.erase(ntohl(cmd_header.seqnum));
                            req->state = RequestState::COMPLETED;
                        }

                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum;
                        ret.devid = ctx->devid;
                        ret.direction = ctx->direction;
                        ret.ep = ctx->ep;
                        ret.status = (res_sc < 0 && errno != EBUSY) ? htonl((uint32_t)-errno) : 0;

                        tx_packet* pkt = new tx_packet();
                        pkt->header = ret;
                        session->enqueue_response(pkt);

                        delete req;
                        delete[] ctx->payload_buffer;
                        delete ctx;
                        continue;
                    } else if (cmd_header.setup[0] == 0x01 && bRequest == 0x0B) {
                        struct usbdevfs_setinterface setintf = {0};
                        setintf.interface = wIndex; setintf.altsetting = wValue;
                        int res_si = ioctl(device_fd, USBDEVFS_SETINTERFACE, &setintf);

                        {
                            std::lock_guard<std::mutex> lock(session->request_mutex);
                            session->pending_requests.erase(ntohl(cmd_header.seqnum));
                            req->state = RequestState::COMPLETED;
                        }

                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;
                        ret.status = (res_si < 0) ? htonl((uint32_t)-errno) : 0;

                        tx_packet* pkt = new tx_packet();
                        pkt->header = ret;
                        session->enqueue_response(pkt);

                        delete req;
                        delete[] ctx->payload_buffer;
                        delete ctx;
                        continue;
                    } else if (cmd_header.setup[0] == 0x00 && bRequest == 0x05) {
                        {
                            std::lock_guard<std::mutex> lock(session->request_mutex);
                            session->pending_requests.erase(ntohl(cmd_header.seqnum));
                            req->state = RequestState::COMPLETED;
                        }

                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum; ret.devid = ctx->devid; ret.direction = ctx->direction; ret.ep = ctx->ep;

                        tx_packet* pkt = new tx_packet();
                        pkt->header = ret;
                        session->enqueue_response(pkt);

                        delete req;
                        delete[] ctx->payload_buffer;
                        delete ctx;
                        continue;
                    } else if (cmd_header.setup[0] == 0x02 && bRequest == 0x01 && wValue == 0x0000) {
                        uint32_t target_endpoint = wIndex & 0xFF;
                        LOGI("TCP: Intercepted CLEAR_FEATURE (ENDPOINT_HALT) for ep 0x%02x", target_endpoint);

                        int res_cf = ioctl(device_fd, USBDEVFS_CLEAR_HALT, &target_endpoint);
                        if (res_cf < 0) {
                            LOGW("USBDEVFS_CLEAR_HALT failed: %s", strerror(errno));
                        }

                        {
                            std::lock_guard<std::mutex> lock(session->request_mutex);
                            session->pending_requests.erase(ntohl(cmd_header.seqnum));
                            req->state = RequestState::COMPLETED;
                        }

                        struct usbip_ret_submit ret = {0};
                        ret.command = htonl(USBIP_RET_SUBMIT);
                        ret.seqnum = ctx->seqnum;
                        ret.devid = ctx->devid;
                        ret.direction = ctx->direction;
                        ret.ep = ctx->ep;
                        ret.status = (res_cf < 0) ? htonl((uint32_t)-errno) : 0;

                        tx_packet* pkt = new tx_packet();
                        pkt->header = ret;
                        session->enqueue_response(pkt);

                        delete req;
                        delete[] ctx->payload_buffer;
                        delete ctx;
                        continue;
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(in_flight_mutex);
                    in_flight_urbs_count++;
                    update_peak(g_in_flight_peak, in_flight_urbs_count.load());
                }

                ctx->submit_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                if (ioctl(device_fd, USBDEVFS_SUBMITURB, (void*)&ctx->urb) < 0) {
                    {
                        std::lock_guard<std::mutex> lock(in_flight_mutex);
                        in_flight_urbs_count--;
                    }
                    {
                        std::lock_guard<std::mutex> lock(session->request_mutex);
                        session->pending_requests.erase(ntohl(cmd_header.seqnum));
                        req->state = RequestState::COMPLETED;
                    }

                    struct usbip_ret_submit ret_err = {0};
                    ret_err.command = htonl(USBIP_RET_SUBMIT);
                    ret_err.seqnum = ctx->seqnum; ret_err.devid = ctx->devid; ret_err.direction = ctx->direction; ret_err.ep = ctx->ep;
                    ret_err.status = htonl((uint32_t)-errno);

                    tx_packet* pkt = new tx_packet();
                    pkt->header = ret_err;
                    session->enqueue_response(pkt);

                    delete req;
                    delete[] ctx->payload_buffer;
                    delete ctx;
                }
            } else if (command == USBIP_CMD_UNLINK) {
                uint32_t cmd_unlink_seqnum = cmd_header.seqnum;
                uint32_t target_submit_seqnum = cmd_header.transfer_flags;

                PendingRequest* req_to_discard = nullptr;
                bool send_completed_unlink_response = false;

                {
                    std::lock_guard<std::mutex> lock(session->request_mutex);
                    auto it = session->pending_requests.find(ntohl(target_submit_seqnum));
                    if (it != session->pending_requests.end()) {
                        PendingRequest* req = it->second;
                        if (req->state == RequestState::PENDING) {
                            req->state = RequestState::UNLINKING;
                            req->unlink_seqnum = cmd_unlink_seqnum;
                            req_to_discard = req;
                            LOGI("connection=%d event=unlink unlinkSeq=%u targetSubmitSeq=%u stateBefore=PENDING stateAfter=UNLINKING response=pending_cancellation status=0",
                                 client_fd, ntohl(cmd_unlink_seqnum), ntohl(target_submit_seqnum));
                        } else {
                            LOGI("connection=%d event=unlink unlinkSeq=%u targetSubmitSeq=%u stateBefore=UNLINKING stateAfter=UNLINKING response=none status=duplicate",
                                 client_fd, ntohl(cmd_unlink_seqnum), ntohl(target_submit_seqnum));
                        }
                    } else {
                        send_completed_unlink_response = true;
                        LOGI("connection=%d event=unlink unlinkSeq=%u targetSubmitSeq=%u stateBefore=MISSING stateAfter=MISSING response=RET_UNLINK status=0",
                             client_fd, ntohl(cmd_unlink_seqnum), ntohl(target_submit_seqnum));
                    }
                }

                if (req_to_discard) {
                    if (ioctl(device_fd, USBDEVFS_DISCARDURB, (void*)&req_to_discard->ctx->urb) < 0) {
                        LOGW("USBDEVFS_DISCARDURB failed for seq %u: %s", ntohl(target_submit_seqnum), strerror(errno));
                    }
                } else if (send_completed_unlink_response) {
                    struct usbip_ret_submit ret = {0};
                    ret.command = htonl(USBIP_RET_UNLINK);
                    ret.seqnum = cmd_unlink_seqnum;
                    ret.status = htonl(0);

                    tx_packet* pkt = new tx_packet();
                    pkt->header = ret;
                    session->enqueue_response(pkt);
                }
            } else break;
        }
    }

    is_connected->store(false);
    session->tx_cv.notify_all(); // Wake up TX thread to exit

    { std::lock_guard<std::mutex> lock(g_client_map_mutex); if (g_busid_to_client_fd.count(current_busid) && g_busid_to_client_fd[current_busid] == client_fd) g_busid_to_client_fd.erase(current_busid); }
    cleanup_zombie_urbs(device_fd, session);
    notify_performance_locks(false);
}

// Creates, binds and starts listening on the USB/IP TCP socket synchronously.
// Returns the bound/listening fd on success, or -1 on failure. On failure the
// socket (if any) is fully closed here so no dangling fd is ever left behind.
static int prepare_server_socket() {
    int server_fd;
    struct sockaddr_in address = {0};
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        LOGE("Failed to create USB/IP server socket: %s", strerror(errno));
        return -1;
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_keepalive(server_fd);
    int nodelay = 1;
    setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Bind to INADDR_ANY for robust Android networking
    address.sin_port = htons(USBIP_PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        LOGE("Failed to bind USB/IP server socket on port %d: %s", USBIP_PORT, strerror(errno));
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, 3) < 0) {
        LOGE("Failed to listen on USB/IP server socket: %s", strerror(errno));
        close(server_fd);
        return -1;
    }
    return server_fd;
}

void run_server(int server_fd, int device_fd_raw, std::string server_ip) {
    int device_fd = (device_fd_raw != -1) ? dup(device_fd_raw) : -1;
    int client_fd;
    struct sockaddr_in address = {0};
    int addrlen = sizeof(address);
    int nodelay = 1;

    std::string expected_ip = "";
    if (!server_ip.empty() && server_ip != "127.0.0.1") {
        expected_ip = server_ip;
        LOGI("Server active on INADDR_ANY, strictly restricting connections to IP: %s", expected_ip.c_str());
    } else {
        LOGI("Server active on INADDR_ANY (all interfaces allowed)");
    }

    while (true) {
        client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (client_fd < 0) {
            std::lock_guard<std::mutex> lock(g_socket_mutex);
            if (g_server_socket == -1) break;
            continue;
        }

        // Enforce strict single-network connection restriction
        if (!expected_ip.empty()) {
            struct sockaddr_in local_addr;
            socklen_t local_len = sizeof(local_addr);
            if (getsockname(client_fd, (struct sockaddr*)&local_addr, &local_len) == 0) {
                char local_ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &local_addr.sin_addr, local_ip_str, sizeof(local_ip_str));
                if (expected_ip != local_ip_str) {
                    LOGW("Rejecting connection on interface %s (strictly restricted to broadcasted IP %s)", local_ip_str, expected_ip.c_str());
                    close(client_fd);
                    continue;
                }
            }
        }

        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        int buf_size = 2 * 1024 * 1024; // 2MB
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

        set_keepalive(client_fd);
        { std::lock_guard<std::mutex> lock(g_clients_mutex); g_client_sockets.push_back(client_fd); }
        // Detach rather than store the thread handle: this loop runs for the
        // lifetime of the server, and pushing every finished std::thread into
        // a vector (only ever drained when the server fully stops) leaks a
        // thread handle per connection - significant over Wi-Fi where
        // reconnects are frequent. g_client_sockets already tracks live
        // clients for shutdown/telemetry purposes, so no join is needed here.
        std::thread([client_fd]() {
            g_active_workers++;
            int current_fd = -1;
            { std::shared_lock<std::shared_mutex> lock(g_devices_rw_mutex); if (!g_active_devices.empty()) current_fd = g_active_devices.begin()->second; }
            handle_client(client_fd, current_fd);
            { std::lock_guard<std::mutex> lock(g_clients_mutex); g_client_sockets.erase(std::remove(g_client_sockets.begin(), g_client_sockets.end(), client_fd), g_client_sockets.end()); }
            close(client_fd);
            g_active_workers--;
        }).detach();
    }

    {
        // Only close/clear g_server_socket if it still refers to *this* thread's
        // socket. If a subsequent stop/start cycle already closed it (or replaced
        // it with a new socket), closing server_fd again here would either
        // double-close an fd or, worse, close an unrelated fd that the OS has
        // since reused for the newer listening socket - silently killing the
        // new server and leaving clients with a connection-refused error.
        std::lock_guard<std::mutex> lock(g_socket_mutex);
        if (g_server_socket == server_fd) {
            g_server_socket = -1;
            close(server_fd);
        }
    }
    if (device_fd >= 0) close(device_fd);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mizukos_usbip_UsbServerService_startNativeServer(JNIEnv *env, jobject thiz, jint device_fd, jstring jserver_ip) {
    signal(SIGPIPE, SIG_IGN);
    if (g_service_obj) env->DeleteGlobalRef(g_service_obj);
    g_service_obj = env->NewGlobalRef(thiz);

    // Resolve the busid-lookup jmethodIDs once here instead of doing a
    // reflective GetMethodID() name lookup on every get_int_for_busid() call.
    {
        jclass cls = env->GetObjectClass(g_service_obj);
        g_mid_getVid = env->GetMethodID(cls, "getVidForBusId", "(Ljava/lang/String;)I");
        g_mid_getPid = env->GetMethodID(cls, "getPidForBusId", "(Ljava/lang/String;)I");
        g_mid_getSpeed = env->GetMethodID(cls, "getSpeedForBusId", "(Ljava/lang/String;)I");
        g_mid_getInterfaceCount = env->GetMethodID(cls, "getInterfaceCountForBusId", "(Ljava/lang/String;)I");
        g_mid_getFd = env->GetMethodID(cls, "getFdForBusId", "(Ljava/lang/String;)I");
        g_mid_acquireLocks = env->GetMethodID(cls, "acquirePerformanceLocks", "()V");
        g_mid_releaseLocks = env->GetMethodID(cls, "releasePerformanceLocks", "()V");
        g_mid_getPayloadDirect = env->GetMethodID(cls, "getExportedDevicesPayloadDirect", "()Ljava/nio/ByteBuffer;");
        g_mid_getPayload = env->GetMethodID(cls, "getExportedDevicesPayload", "()[B");
        env->DeleteLocalRef(cls);
    }

    std::string server_ip = "";
    if (jserver_ip) {
        const char* ip_ptr = env->GetStringUTFChars(jserver_ip, nullptr);
        server_ip = ip_ptr;
        env->ReleaseStringUTFChars(jserver_ip, ip_ptr);
    }

    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_server_socket >= 0) { shutdown(g_server_socket, SHUT_RDWR); close(g_server_socket); g_server_socket = -1; }
    { std::unique_lock<std::shared_mutex> dev_lock(g_devices_rw_mutex); if (device_fd != -1) g_active_devices["1-1"] = device_fd; }
    if (g_server_thread.joinable()) g_server_thread.join();

    // Bind/listen synchronously so we can report success/failure back to the
    // caller. Previously this happened inside the background thread with no
    // way to signal failure, so the Kotlin side would mark the server as
    // "started" and advertise it via NSD even when bind()/listen() failed -
    // clients would then get a connection-refused error trying to reach a
    // port nothing was actually listening on.
    int server_fd = prepare_server_socket();
    if (server_fd < 0) {
        return JNI_FALSE;
    }
    g_server_socket = server_fd;
    g_server_thread = std::thread(run_server, server_fd, device_fd, server_ip);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_mizukos_usbip_UsbServerService_stopNativeServer(JNIEnv *env, jobject thiz) {
    { std::lock_guard<std::mutex> lock(g_socket_mutex); if (g_server_socket >= 0) { shutdown(g_server_socket, SHUT_RDWR); close(g_server_socket); g_server_socket = -1; } }
    std::vector<int> clients; { std::lock_guard<std::mutex> lock(g_clients_mutex); clients = g_client_sockets; }
    for (int fd : clients) shutdown(fd, SHUT_RDWR);
    if (g_server_thread.joinable()) g_server_thread.join();

    // Wait for all detached worker threads (per-connection handle_client
    // threads, reap_thread, tcp_tx_thread) to finish. They
    // are detached (not joined) to avoid leaking thread handles, but several
    // of them call JNI methods on g_service_obj as their very last act (e.g.
    // notify_performance_locks(false)). Deleting the global ref before they
    // finish would race with that call and crash with a
    // "JNI ERROR: accessed deleted global reference".
    while (g_active_workers.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // NOW it is safe to delete the JNI object
    if (g_service_obj) { env->DeleteGlobalRef(g_service_obj); g_service_obj = nullptr; }
    { std::unique_lock<std::shared_mutex> dev_lock(g_devices_rw_mutex); for (auto const& item : g_active_devices) { if (item.second != -1) close(item.second); } g_active_devices.clear(); }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_mizukos_usbip_UsbServerService_getTransferSpeedForBusId(JNIEnv *env, jobject thiz, jstring jbusid) {
    const char* busid_ptr = env->GetStringUTFChars(jbusid, nullptr);
    std::string busid(busid_ptr);
    env->ReleaseStringUTFChars(jbusid, busid_ptr);

    std::lock_guard<std::mutex> lock(g_stats_mutex);
    auto it = g_transfer_stats.find(busid);
    if (it == g_transfer_stats.end()) return 0;

    auto now = std::chrono::steady_clock::now();
    auto& stats = it->second;
    double elapsed = std::chrono::duration<double>(now - stats.last_tp).count();
    if (elapsed >= 0.5) {
        uint64_t current_bytes = stats.bytes_transferred.load();
        uint64_t diff = current_bytes - stats.last_bytes;
        stats.speed_mbps = ((double)diff * 8.0) / (elapsed * 1000.0 * 1000.0);
        stats.last_bytes = current_bytes;
        stats.last_tp = now;
    }
    return (jint)(stats.speed_mbps + 0.5);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mizukos_usbip_UsbServerService_getTelemetryJson(JNIEnv *env, jobject thiz) {
    uint64_t now_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    std::string json = "{";
    json += "\"schema_version\":1,";
    json += "\"service\":\"usbip-telemetry\",";
    json += "\"usbip_port\":3240,";
    json += "\"telemetry_port\":3241,";
    json += "\"timestamp_unix_ms\":" + std::to_string(now_wall_ms) + ",";

    // Pipeline/throughput diagnostics (process-wide; never includes payload bytes).
    {
        uint64_t usb_wait_total = g_usb_wait_ns_total.load(std::memory_order_relaxed);
        uint64_t usb_wait_samples = g_usb_wait_samples.load(std::memory_order_relaxed);
        uint64_t tcp_read_total = g_tcp_read_wait_ns_total.load(std::memory_order_relaxed);
        uint64_t tcp_read_samples = g_tcp_read_wait_samples.load(std::memory_order_relaxed);
        uint64_t tcp_write_total = g_tcp_write_wait_ns_total.load(std::memory_order_relaxed);
        uint64_t tcp_write_samples = g_tcp_write_wait_samples.load(std::memory_order_relaxed);

        json += "\"perf\":{";
        json += "\"in_flight_current\":" + std::to_string(in_flight_urbs_count.load()) + ",";
        json += "\"in_flight_peak\":" + std::to_string(g_in_flight_peak.load()) + ",";
        json += "\"tx_queue_depth_peak\":" + std::to_string(g_tx_queue_depth_peak.load()) + ",";
        json += "\"usb_wait_us_avg\":" + std::to_string(usb_wait_samples ? (usb_wait_total / usb_wait_samples / 1000) : 0) + ",";
        json += "\"usb_wait_us_max\":" + std::to_string(g_usb_wait_ns_max.load() / 1000) + ",";
        json += "\"tcp_read_wait_us_avg\":" + std::to_string(tcp_read_samples ? (tcp_read_total / tcp_read_samples / 1000) : 0) + ",";
        json += "\"tcp_write_wait_us_avg\":" + std::to_string(tcp_write_samples ? (tcp_write_total / tcp_write_samples / 1000) : 0) + ",";
        json += "\"short_transfer_count\":" + std::to_string(g_short_transfer_count.load()) + ",";
        json += "\"usb_error_count\":" + std::to_string(g_usb_error_count.load()) + ",";
        json += "\"socket_error_count\":" + std::to_string(g_socket_error_count.load()) + ",";
        json += "\"submit_retry_count\":" + std::to_string(g_submit_retry_count.load());
        json += "},";
    }

    json += "\"devices\":[";

    {
        std::lock_guard<std::mutex> lock(g_telemetry_mutex);
        bool first = true;
        for (const auto& pair : g_telemetry_map) {
            const auto& stats = pair.second;
            if (!first) json += ",";
            first = false;

            json += "{";
            json += "\"busid\":\"" + stats.busid + "\",";
            if (!stats.name.empty()) {
                json += "\"name\":\"" + stats.name + "\",";
            } else {
                json += "\"name\":null,";
            }
            if (stats.vendor_id > 0) {
                json += "\"vendor_id\":" + std::to_string(stats.vendor_id) + ",";
            } else {
                json += "\"vendor_id\":null,";
            }
            if (stats.product_id > 0) {
                json += "\"product_id\":" + std::to_string(stats.product_id) + ",";
            } else {
                json += "\"product_id\":null,";
            }
            if (!stats.usb_class.empty()) {
                json += "\"usb_class\":\"" + stats.usb_class + "\",";
            } else {
                json += "\"usb_class\":null,";
            }
            json += "\"active\":" + std::string(stats.active ? "true" : "false") + ",";
            json += "\"transfer_count\":" + std::to_string(stats.transfer_count) + ",";
            json += "\"completed_transfer_count\":" + std::to_string(stats.completed_transfer_count) + ",";
            json += "\"failed_transfer_count\":" + std::to_string(stats.failed_transfer_count) + ",";
            json += "\"bytes_to_client\":" + std::to_string(stats.bytes_to_client) + ",";
            json += "\"bytes_from_client\":" + std::to_string(stats.bytes_from_client) + ",";

            if (stats.completed_transfer_count > 0) {
                uint64_t avg_lat = stats.total_latency_us / stats.completed_transfer_count;
                json += "\"latency_us_average\":" + std::to_string(avg_lat) + ",";
                json += "\"latency_us_max\":" + std::to_string(stats.latency_us_max) + ",";
            } else {
                json += "\"latency_us_average\":null,";
                json += "\"latency_us_max\":null,";
            }

            if (stats.latency_samples.size() >= 2) {
                uint64_t jit = calculate_jitter(stats.latency_samples);
                json += "\"jitter_us\":" + std::to_string(jit) + ",";
            } else {
                json += "\"jitter_us\":null,";
            }

            if (stats.last_transfer_unix_ms > 0) {
                json += "\"last_transfer_unix_ms\":" + std::to_string(stats.last_transfer_unix_ms);
            } else {
                json += "\"last_transfer_unix_ms\":null";
            }
            json += "}";
        }
    }

    json += "]}";
    return env->NewStringUTF(json.c_str());
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
