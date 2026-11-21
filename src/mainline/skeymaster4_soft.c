/*
 * Software Keymaster 4.0 shim (libskeymaster4device.so) for mainline Linux 7.3.
 *
 * Replaces Samsung's QSEECom-backed libskeymaster4device.so by forwarding
 * skeymaster::CreateSKeymasterDevice(SecurityLevel) directly to AOSP's
 * keymaster::V4_0::ng::CreateKeymasterDevice(SecurityLevel) in libkeymaster4.so,
 * which is already present on /vendor/lib64 and backed by PureSoftKeymasterContext
 * (libpuresoftkeymasterdevice.so, 100% software BoringSSL, zero /dev/qseecom).
 */

#include <stdint.h>

extern void *_ZN9keymaster4V4_02ng21CreateKeymasterDeviceEN7android8hardware9keymaster4V4_013SecurityLevelE(
    int32_t security_level);

__attribute__((visibility("default")))
void *_ZN10skeymaster22CreateSKeymasterDeviceEN7android8hardware9keymaster4V4_013SecurityLevelE(
    int32_t security_level) {
    return _ZN9keymaster4V4_02ng21CreateKeymasterDeviceEN7android8hardware9keymaster4V4_013SecurityLevelE(
        security_level);
}
