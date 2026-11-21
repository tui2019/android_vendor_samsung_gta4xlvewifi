/*
 * Pure-software Gatekeeper 1.0 HAL module (gatekeeper.mdfpp.so) for mainline Linux 7.3.
 *
 * Replaces Qualcomm/Samsung QSEECom gatekeeper.mdfpp.so with standard AOSP
 * software password-handle + hw_auth_token HMAC-SHA256 verification using
 * /vendor/lib64/libcrypto.so (BoringSSL). Zero /dev/qseecom dependency.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Android arm64 Bionic libc prototypes (compiled with -nostdlib against Bionic libc.so) */
#define EINVAL 22
#define ENOMEM 12
#define CLOCK_BOOTTIME 7

struct timespec {
    long tv_sec;
    long tv_nsec;
};

extern void *malloc(size_t size);
extern void *calloc(size_t nmemb, size_t size);
extern void free(void *ptr);
extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern int clock_gettime(int clk_id, struct timespec *tp);

/* BoringSSL symbols from /vendor/lib64/libcrypto.so */
extern const void *EVP_sha256(void);
extern unsigned char *HMAC(const void *evp_md, const void *key, int key_len,
                           const unsigned char *d, size_t n,
                           unsigned char *md, unsigned int *md_len);
extern int RAND_bytes(unsigned char *buf, size_t num);

/* Standard AOSP hardware/hardware.h ABI (arm64) */
#define HARDWARE_MODULE_TAG 0x48574d54U /* 'HWMT' */
#define HARDWARE_DEVICE_TAG 0x48574454U /* 'HWDT' */

struct hw_module_t;
struct hw_device_t;

struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id,
                struct hw_device_t **device);
};

struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};

struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *device);
};

/* Standard AOSP hardware/gatekeeper.h ABI (arm64: close@0x70, enroll@0x78, verify@0x80) */
struct gatekeeper_device {
    struct hw_device_t common;
    int (*enroll)(const struct gatekeeper_device *dev, uint32_t uid,
                  const uint8_t *current_password_handle, uint32_t current_password_handle_length,
                  const uint8_t *current_password, uint32_t current_password_length,
                  const uint8_t *desired_password, uint32_t desired_password_length,
                  uint8_t **enrolled_password_handle, uint32_t *enrolled_password_handle_length);
    int (*verify)(const struct gatekeeper_device *dev, uint32_t uid, uint64_t challenge,
                  const uint8_t *enrolled_password_handle, uint32_t enrolled_password_handle_length,
                  const uint8_t *provided_password, uint32_t provided_password_length,
                  uint8_t **auth_token, uint32_t *auth_token_length, bool *request_reenroll);
    int (*delete_user)(const struct gatekeeper_device *dev, uint32_t uid);
    int (*delete_all_users)(const struct gatekeeper_device *dev);
};

_Static_assert(offsetof(struct hw_device_t, close) == 0x70, "hw_device_t.close offset mismatch");
_Static_assert(offsetof(struct gatekeeper_device, enroll) == 0x78, "gatekeeper_device.enroll offset mismatch");
_Static_assert(offsetof(struct gatekeeper_device, verify) == 0x80, "gatekeeper_device.verify offset mismatch");

/* AOSP password_handle_t (packed, 69 bytes) */
struct __attribute__((packed)) password_handle_t {
    uint8_t version;
    uint64_t user_id;
    uint64_t flags;
    uint64_t salt;
    uint8_t signature[32];
    uint8_t hardware_backed;
};

/* AOSP hw_auth_token_t (packed, 69 bytes) */
struct __attribute__((packed)) hw_auth_token_t {
    uint8_t version;
    uint64_t challenge;
    uint64_t user_id;
    uint64_t authenticator_id;
    uint32_t authenticator_type; /* big-endian */
    uint64_t timestamp;          /* big-endian */
    uint8_t hmac[32];
};

static const uint8_t kPasswordKey[32] = {
    0x70, 0x36, 0x31, 0x33, 0x5f, 0x73, 0x6f, 0x66,
    0x74, 0x5f, 0x67, 0x6b, 0x5f, 0x70, 0x77, 0x5f,
    0x6b, 0x65, 0x79, 0x5f, 0x76, 0x31, 0x5f, 0x6d,
    0x61, 0x69, 0x6e, 0x6c, 0x69, 0x6e, 0x65, 0x21
};

/* Standard AOSP SoftGateKeeper / PureSoftKeymaster shared HMAC key (32 zero bytes) */
static const uint8_t kAuthTokenKey[32] = { 0 };

static uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

static uint64_t get_millis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void compute_pw_sig(uint8_t out_sig[32], uint64_t salt,
                           const uint8_t *pw, uint32_t pw_len) {
    size_t total = sizeof(salt) + pw_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        memset(out_sig, 0, 32);
        return;
    }
    memcpy(buf, &salt, sizeof(salt));
    if (pw && pw_len)
        memcpy(buf + sizeof(salt), pw, pw_len);
    unsigned int md_len = 32;
    HMAC(EVP_sha256(), kPasswordKey, sizeof(kPasswordKey), buf, total, out_sig, &md_len);
    memset(buf, 0, total);
    free(buf);
}

static int ct_memcmp(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= a[i] ^ b[i];
    return diff;
}

static int gk_enroll(const struct gatekeeper_device *dev, uint32_t uid,
                     const uint8_t *cur_handle, uint32_t cur_handle_len,
                     const uint8_t *cur_pw, uint32_t cur_pw_len,
                     const uint8_t *desired_pw, uint32_t desired_pw_len,
                     uint8_t **out_handle, uint32_t *out_handle_len) {
    (void)dev;
    (void)uid;
    if (!desired_pw || desired_pw_len == 0 || !out_handle || !out_handle_len)
        return -EINVAL;

    uint64_t user_id = 0;
    if (cur_handle && cur_handle_len == sizeof(struct password_handle_t)) {
        const struct password_handle_t *ch = (const struct password_handle_t *)cur_handle;
        uint8_t expected[32];
        compute_pw_sig(expected, ch->salt, cur_pw, cur_pw_len);
        if (ct_memcmp(expected, ch->signature, 32) != 0)
            return -EINVAL;
        user_id = ch->user_id;
    } else {
        if (RAND_bytes((unsigned char *)&user_id, sizeof(user_id)) != 1)
            user_id = 0x123456789abcdef0ULL ^ get_millis();
    }

    struct password_handle_t *nh = (struct password_handle_t *)calloc(1, sizeof(*nh));
    if (!nh)
        return -ENOMEM;

    nh->version = 2;
    nh->user_id = user_id;
    nh->flags = 0;
    if (RAND_bytes((unsigned char *)&nh->salt, sizeof(nh->salt)) != 1)
        nh->salt = 0xfedcba9876543210ULL ^ get_millis();
    nh->hardware_backed = 0;
    compute_pw_sig(nh->signature, nh->salt, desired_pw, desired_pw_len);

    *out_handle = (uint8_t *)nh;
    *out_handle_len = sizeof(*nh);
    return 0;
}

static int gk_verify(const struct gatekeeper_device *dev, uint32_t uid, uint64_t challenge,
                     const uint8_t *enrolled_handle, uint32_t enrolled_handle_len,
                     const uint8_t *provided_pw, uint32_t provided_pw_len,
                     uint8_t **out_token, uint32_t *out_token_len, bool *request_reenroll) {
    (void)dev;
    (void)uid;
    if (request_reenroll)
        *request_reenroll = 0;
    if (!enrolled_handle || enrolled_handle_len != sizeof(struct password_handle_t) ||
        !provided_pw || !out_token || !out_token_len)
        return -EINVAL;

    const struct password_handle_t *eh = (const struct password_handle_t *)enrolled_handle;
    uint8_t expected[32];
    compute_pw_sig(expected, eh->salt, provided_pw, provided_pw_len);
    if (ct_memcmp(expected, eh->signature, 32) != 0)
        return -EINVAL;

    struct hw_auth_token_t *tok = (struct hw_auth_token_t *)calloc(1, sizeof(*tok));
    if (!tok)
        return -ENOMEM;

    tok->version = 0;
    tok->challenge = challenge;
    tok->user_id = eh->user_id;
    tok->authenticator_id = 0;
    tok->authenticator_type = bswap32(1U); /* HW_AUTH_PASSWORD */
    tok->timestamp = bswap64(get_millis());
    unsigned int md_len = 32;
    HMAC(EVP_sha256(), kAuthTokenKey, sizeof(kAuthTokenKey),
         (const unsigned char *)tok, offsetof(struct hw_auth_token_t, hmac),
         tok->hmac, &md_len);

    *out_token = (uint8_t *)tok;
    *out_token_len = sizeof(*tok);
    return 0;
}

static int gk_delete_user(const struct gatekeeper_device *dev, uint32_t uid) {
    (void)dev;
    (void)uid;
    return 0;
}

static int gk_delete_all_users(const struct gatekeeper_device *dev) {
    (void)dev;
    return 0;
}

static int gk_close(struct hw_device_t *device) {
    free(device);
    return 0;
}

static int gk_open(const struct hw_module_t *module, const char *id,
                   struct hw_device_t **device) {
    (void)id;
    struct gatekeeper_device *dev = (struct gatekeeper_device *)calloc(1, sizeof(*dev));
    if (!dev)
        return -ENOMEM;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 1;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = gk_close;
    dev->enroll = gk_enroll;
    dev->verify = gk_verify;
    dev->delete_user = gk_delete_user;
    dev->delete_all_users = gk_delete_all_users;

    *device = &dev->common;
    return 0;
}

static struct hw_module_methods_t gk_module_methods = {
    .open = gk_open,
};

__attribute__((visibility("default")))
struct hw_module_t HMI = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = 0x100,
    .id = "gatekeeper",
    .name = "Software Gatekeeper HAL (Mainline 7.3)",
    .author = "SM-P613 Mainline Port",
    .methods = &gk_module_methods,
    .dso = NULL,
};
