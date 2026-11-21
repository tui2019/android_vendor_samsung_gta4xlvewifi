/*
 * Software Keymaster 4.0 shim (libskeymaster4device.so) for mainline Linux 7.3.
 *
 * Replaces Samsung's QSEECom-backed libskeymaster4device.so by forwarding
 * skeymaster::CreateSKeymasterDevice(SecurityLevel) to AOSP's
 * keymaster::V4_0::ng::CreateKeymasterDevice(SOFTWARE) in libkeymaster4.so.
 *
 * CRITICAL: PureSoftKeymasterContext allocates a RAM-only PureSoftSecureKeyStorage
 * when constructed with TRUSTED_ENVIRONMENT (1), causing vold's rollback-resistant
 * User 0 DE key to vanish across reboots (--reason=init_user0_failed -> wipe_data).
 * Constructing with SOFTWARE (0) keeps pure_soft_secure_key_storage_ == nullptr so
 * CreateKeyBlob returns KM_ERROR_ROLLBACK_RESISTANCE_UNAVAILABLE (-67) and vold
 * cleanly retries without rollback resistance, making encrypted /data persist across
 * all reboots. We then restore AndroidKeymaster4Device::security_level_ (+0x10) so
 * getHardwareInfo() still reports TRUSTED_ENVIRONMENT to keystore2.
 */

#include <stdint.h>

extern void *_ZN9keymaster4V4_02ng21CreateKeymasterDeviceEN7android8hardware9keymaster4V4_013SecurityLevelE(
    int32_t security_level);

__attribute__((visibility("default")))
void *_ZN10skeymaster22CreateSKeymasterDeviceEN7android8hardware9keymaster4V4_013SecurityLevelE(
    int32_t security_level) {
    /* Construct with SECURITY_LEVEL_SOFTWARE (0) to avoid RAM-only PureSoftSecureKeyStorage */
    void *dev = _ZN9keymaster4V4_02ng21CreateKeymasterDeviceEN7android8hardware9keymaster4V4_013SecurityLevelE(0);
    if (dev) {
        /* Verified arm64 offset 0x10 in AndroidKeymaster4Device::getHardwareInfo */
        *(int32_t *)((char *)dev + 0x10) = security_level;
    }
    return dev;
}
