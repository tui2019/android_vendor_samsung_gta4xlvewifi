#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define __INTRODUCED_IN(x)
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_parcel.h>
#include <android/binder_process.h>
#include <android/binder_stability.h>
#include <android/binder_status.h>

#ifdef __cplusplus
extern "C" {
#endif
extern int *__errno(void);
extern int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#ifdef __cplusplus
}
#endif

#define B_ERRNO (*__errno())
#define LOGI(...) __android_log_print(4, "p613_bt_hal", __VA_ARGS__)
#define LOGE(...) __android_log_print(6, "p613_bt_hal", __VA_ARGS__)

#define BT_HCI_IFACE "android.hardware.bluetooth.IBluetoothHci"
#define BT_CB_IFACE  "android.hardware.bluetooth.IBluetoothHciCallbacks"
#define BT_IFACE_HASH "f0eb2eb6d3b970cef8fb1b6e22c4ec39a4da5119"

#define TXN_GET_VER   0x00ffffffU
#define TXN_GET_HASH  0x00fffffeU

enum {
    CMD_CLOSE = 1,
    CMD_INIT  = 2,
    CMD_ACL   = 3,
    CMD_HCI   = 4,
    CMD_ISO   = 5,
    CMD_SCO   = 6,
};

enum {
    EV_ACL  = 1,
    EV_HCI  = 2,
    EV_INIT = 3,
    EV_ISO  = 4,
    EV_SCO  = 5,
};

#define BTPROTO_HCI 1
#define HCI_CHANNEL_USER 1
#define HCIDEVDOWN _IOW('H', 202, int)

struct hci_sockaddr {
    sa_family_t family;
    unsigned short dev;
    unsigned short channel;
};

struct pkt_buf {
    uint8_t *raw;
    int32_t count;
};

static AIBinder_Class *s_hci_class;
static AIBinder_Class *s_cb_class;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_fd = -1;
static int s_pipe[2] = {-1, -1};
static AIBinder *s_cb = NULL;
static pthread_t s_tid;
static bool s_running = false;
static uint8_t s_mac[6];
static bool s_has_mac = false;

static void read_samsung_bt_mac(void) {
    int fd = open("/mnt/vendor/efs/bluetooth/bt_addr", O_RDONLY);
    if (fd < 0) return;
    char txt[32];
    ssize_t n = read(fd, txt, sizeof(txt) - 1);
    close(fd);
    if (n < 17) return;
    txt[n] = '\0';
    unsigned v[6];
    if (sscanf(txt, "%x:%x:%x:%x:%x:%x", &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) == 6) {
        for (int i = 0; i < 6; i++) s_mac[i] = (uint8_t)v[i];
        s_has_mac = true;
    }
}

static void write_file_str(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, s, strlen(s));
        close(fd);
    }
}

static int open_user_channel(void) {
    for (int i = 0; i < 2; i++) {
        int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
        if (ctl >= 0) {
            ioctl(ctl, HCIDEVDOWN, 0);
            close(ctl);
        }
        int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
        if (fd < 0) {
            LOGE("socket err=%d", B_ERRNO);
            return -1;
        }
        struct hci_sockaddr sa = { AF_BLUETOOTH, 0, HCI_CHANNEL_USER };
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            LOGI("hci0 user channel ready (fd=%d)", fd);
            return fd;
        }
        LOGE("bind try %d failed err=%d", i, B_ERRNO);
        close(fd);
        if (i == 0) {
            write_file_str("/sys/bus/serial/drivers/hci_uart_qca/unbind", "serial0-0");
            usleep(300000);
            write_file_str("/sys/bus/serial/drivers/hci_uart_qca/bind", "serial0-0");
            sleep(2);
        }
    }
    return -1;
}

static void notify_init(AIBinder *cb, int32_t st) {
    AParcel *p = NULL, *reply = NULL;
    if (AIBinder_prepareTransaction(cb, &p) != STATUS_OK) return;
    AParcel_writeInt32(p, st);
    AIBinder_transact(cb, EV_INIT, &p, &reply, 0);
    if (reply) AParcel_delete(reply);
}

static void notify_pkt(AIBinder *cb, transaction_code_t code, const uint8_t *buf, size_t len) {
    AParcel *p = NULL, *reply = NULL;
    if (AIBinder_prepareTransaction(cb, &p) != STATUS_OK) return;
    AParcel_writeByteArray(p, (const int8_t *)buf, (int32_t)len);
    AIBinder_transact(cb, code, &p, &reply, 0);
    if (reply) AParcel_delete(reply);
}

static void *rx_loop(void *unused) {
    (void)unused;
    uint8_t pkt[2048];
    while (1) {
        struct pollfd fds[2] = {
            { .fd = s_fd, .events = POLLIN },
            { .fd = s_pipe[0], .events = POLLIN },
        };
        int rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (B_ERRNO == 4) continue;
            break;
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) break;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(fds[0].revents & POLLIN)) continue;

        ssize_t n = read(s_fd, pkt, sizeof(pkt));
        if (n <= 1) continue;

        if (s_has_mac && pkt[0] == 0x04 && n == 13 &&
            pkt[1] == 0x0e && pkt[2] == 0x0a && pkt[4] == 0x09 && pkt[5] == 0x10 && pkt[6] == 0x00) {
            memcpy(pkt + 7, s_mac, 6);
        }

        pthread_mutex_lock(&s_mtx);
        AIBinder *cb = s_cb;
        if (cb) AIBinder_incStrong(cb);
        pthread_mutex_unlock(&s_mtx);
        if (!cb) continue;

        if (pkt[0] == 0x04) notify_pkt(cb, EV_HCI, pkt + 1, (size_t)(n - 1));
        else if (pkt[0] == 0x02) notify_pkt(cb, EV_ACL, pkt + 1, (size_t)(n - 1));
        else if (pkt[0] == 0x03) notify_pkt(cb, EV_SCO, pkt + 1, (size_t)(n - 1));
        else if (pkt[0] == 0x05) notify_pkt(cb, EV_ISO, pkt + 1, (size_t)(n - 1));
        AIBinder_decStrong(cb);
    }
    return NULL;
}

static void shutdown_locked(void) {
    if (s_running) {
        if (s_pipe[1] >= 0) {
            char x = 1;
            (void)write(s_pipe[1], &x, 1);
        }
        pthread_mutex_unlock(&s_mtx);
        pthread_join(s_tid, NULL);
        pthread_mutex_lock(&s_mtx);
        s_running = false;
    }
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
    if (s_pipe[0] >= 0) { close(s_pipe[0]); s_pipe[0] = -1; }
    if (s_pipe[1] >= 0) { close(s_pipe[1]); s_pipe[1] = -1; }
    if (s_cb) { AIBinder_decStrong(s_cb); s_cb = NULL; }
}

static bool pkt_alloc(void *ctx, int32_t len, int8_t **out) {
    struct pkt_buf *pb = (struct pkt_buf *)ctx;
    if (len < 0 || len > 65536) return false;
    pb->count = len;
    if (len == 0) { pb->raw = NULL; *out = NULL; return true; }
    pb->raw = (uint8_t *)malloc((size_t)len + 1);
    if (!pb->raw) return false;
    *out = (int8_t *)(pb->raw + 1);
    return true;
}

static void write_ok(AParcel *out) {
    AStatus *st = AStatus_newOk();
    AParcel_writeStatusHeader(out, st);
    AStatus_delete(st);
}

static void *on_create(void *a) { return a; }
static void on_destroy(void *a) { (void)a; }
static binder_status_t on_cb_txn(AIBinder *b, transaction_code_t c, const AParcel *i, AParcel *o) {
    (void)b; (void)c; (void)i; (void)o;
    return STATUS_UNKNOWN_TRANSACTION;
}

static binder_status_t on_hci_txn(AIBinder *b, transaction_code_t code, const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TXN_GET_VER) {
        write_ok(out);
        AParcel_writeInt32(out, 1);
        return STATUS_OK;
    }
    if (code == TXN_GET_HASH) {
        write_ok(out);
        AParcel_writeString(out, BT_IFACE_HASH, (int32_t)strlen(BT_IFACE_HASH));
        return STATUS_OK;
    }
    if (code == CMD_INIT) {
        AIBinder *cb = NULL;
        if (AParcel_readStrongBinder(in, &cb) != STATUS_OK || !cb) return STATUS_BAD_VALUE;
        AIBinder_associateClass(cb, s_cb_class);
        pthread_mutex_lock(&s_mtx);
        shutdown_locked();
        int fd = open_user_channel();
        if (fd < 0) {
            pthread_mutex_unlock(&s_mtx);
            notify_init(cb, 2);
            AIBinder_decStrong(cb);
            write_ok(out);
            return STATUS_OK;
        }
        pipe(s_pipe);
        s_fd = fd;
        s_cb = cb;
        s_running = (pthread_create(&s_tid, NULL, rx_loop, NULL) == 0);
        pthread_mutex_unlock(&s_mtx);
        notify_init(cb, 0);
        write_ok(out);
        return STATUS_OK;
    }
    if (code == CMD_CLOSE) {
        pthread_mutex_lock(&s_mtx);
        shutdown_locked();
        pthread_mutex_unlock(&s_mtx);
        write_ok(out);
        return STATUS_OK;
    }

    uint8_t tag = 0;
    if (code == CMD_HCI) tag = 0x01;
    else if (code == CMD_ACL) tag = 0x02;
    else if (code == CMD_SCO) tag = 0x03;
    else if (code == CMD_ISO) tag = 0x05;
    else return STATUS_UNKNOWN_TRANSACTION;

    struct pkt_buf pb = {NULL, 0};
    if (AParcel_readByteArray(in, &pb, pkt_alloc) != STATUS_OK) {
        free(pb.raw);
        return STATUS_BAD_VALUE;
    }
    if (pb.raw && pb.count > 0) {
        pb.raw[0] = tag;
        pthread_mutex_lock(&s_mtx);
        if (s_fd >= 0) (void)write(s_fd, pb.raw, (size_t)pb.count + 1);
        pthread_mutex_unlock(&s_mtx);
    }
    free(pb.raw);
    write_ok(out);
    return STATUS_OK;
}

#ifdef __cplusplus
extern "C"
#endif
int main(void) {
    LOGI("starting p613 mainline Bluetooth AIDL HAL");
    read_samsung_bt_mac();
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    s_hci_class = AIBinder_Class_define(BT_HCI_IFACE, on_create, on_destroy, on_hci_txn);
    s_cb_class = AIBinder_Class_define(BT_CB_IFACE, on_create, on_destroy, on_cb_txn);
    AIBinder *svc = AIBinder_new(s_hci_class, NULL);
    AIBinder_markVintfStability(svc);
    binder_exception_t ex = AServiceManager_addService(svc, BT_HCI_IFACE "/default");
    if (ex != 0) {
        LOGE("addService failed: %d", ex);
        return 1;
    }
    LOGI("registered %s/default", BT_HCI_IFACE);
    ABinderProcess_joinThreadPool();
    return 0;
}
