#!/vendor/bin/sh
# Prepare mainline 7.3 stitched firmware under /vendor/firmware (tmpfs with
# vendor_firmware_file context so SELinux-enforcing kernel_202604 never has to
# traverse /data system_data_root_file), HexagonFS for ADSP sensorspd,
# pre-authorized bringup ADB key, and boot ADSP/CDSP + rebind WCN3991 BT.

FW_ROOT=/vendor/firmware
FW_DEV=$FW_ROOT/qcom/sm7125/Samsung/gta4xlvewifi

mkdir -p /data/vendor/tmp/tqftpserv
chmod 0777 /data/vendor/tmp/tqftpserv

# Pre-authorize bringup host ADB key so USB ADB connects unlocked on user builds
if [ -f /vendor/etc/p613_adb_keys ]; then
    mkdir -p /data/misc/adb
    chown 1000:2000 /data/misc/adb
    chmod 2750 /data/misc/adb
    cp -f /vendor/etc/p613_adb_keys /data/misc/adb/adb_keys
    chown 1000:2000 /data/misc/adb/adb_keys
    chmod 0640 /data/misc/adb/adb_keys
    chcon u:object_r:adb_keys_file:s0 /data/misc/adb /data/misc/adb/adb_keys 2>/dev/null || true
fi

# Mount tmpfs with firmware_file context (already contextmount_type with filesystem:associate)
if ! grep -q " $FW_DEV " /proc/mounts 2>/dev/null; then
    mount -t tmpfs -o mode=0755,size=160M,context=u:object_r:firmware_file:s0 tmpfs "$FW_DEV" 2>/dev/kmsg || echo "<3>p613-setup: tmpfs mount failed on $FW_DEV" > /dev/kmsg
fi

# Stitch Qualcomm split ELF (.mdt + .bXX) into monolithic .mbn inside $FW_DEV
for fw in adsp cdsp ipa_fws venus a615_zap; do
    if [ ! -s "$FW_DEV/${fw}.mbn" ] && [ -f "/vendor/firmware_mnt/image/${fw}.mdt" ]; then
        /vendor/bin/mdt_stitch "/vendor/firmware_mnt/image/${fw}.mdt" "$FW_DEV/${fw}.mbn" 2>/dev/kmsg
    fi
done
if [ ! -s "$FW_DEV/modem.mbn" ] && [ -f "/vendor/firmware-modem/image/modem.mdt" ]; then
    /vendor/bin/mdt_stitch "/vendor/firmware-modem/image/modem.mdt" "$FW_DEV/modem.mbn" 2>/dev/kmsg
fi

# Symlink/copy remaining per-device firmware files into $FW_DEV
ln -sf /vendor/firmware-modem/image/modem_pr "$FW_DEV/modem_pr"
[ -f "$FW_DEV/wlanmdsp.mbn" ] || cp -f /vendor/firmware/wlanmdsp.mbn "$FW_DEV/wlanmdsp.mbn" 2>/dev/null
[ -f "$FW_DEV/Data.msc" ] || cp -f /vendor/firmware/Data.msc "$FW_DEV/Data.msc" 2>/dev/null
[ -f "$FW_DEV/bdwlan.bin" ] || cp -f /vendor/firmware/wlan/qca_cld/bdwlan.bin "$FW_DEV/bdwlan.bin" 2>/dev/null

echo "<6>p613-setup: fw_dev=$(ls "$FW_DEV" 2>/dev/null | tr '\n' ' ')" > /dev/kmsg

# Point kernel firmware loader to /vendor/firmware
echo -n "$FW_ROOT" > /sys/module/firmware_class/parameters/path

# Prepare HexagonFS root (/dev/hfs) for ADSP sensorspd (hexagonrpcd)
rm -rf /dev/hfs
mkdir -p /dev/hfs/sensors /dev/hfs/dsp /dev/hfs/socinfo /dev/hfs/ssc_core
ln -sf /vendor/etc/sensors/config /dev/hfs/sensors/config
ln -sf /mnt/vendor/persist/sensors/registry /dev/hfs/sensors/registry
ln -sf /vendor/etc/sensors/sns_reg_config /dev/hfs/sensors/sns_reg.conf
ln -sf /vendor/dsp/adsp /dev/hfs/dsp/adsp
ln -sf /vendor/etc/acdbdata /dev/hfs/acdb
echo "MTP" > /dev/hfs/socinfo/hw_platform
echo "Unknown" > /dev/hfs/socinfo/platform_subtype
echo "0" > /dev/hfs/socinfo/platform_subtype_id
echo "65536" > /dev/hfs/socinfo/platform_version
cat /sys/devices/soc0/soc_id > /dev/hfs/socinfo/soc_id 2>/dev/null || echo "443" > /dev/hfs/socinfo/soc_id
REV=$(cat /proc/cmdline 2>/dev/null | sed -n 's/.*androidboot\.revision=\([0-9]*\).*/\1/p')
echo "${REV:-4}" > /dev/hfs/ssc_core/ssc_hw_rev
echo "0" > /dev/hfs/ssc_core/ssc_mode

# Boot ADSP and CDSP so ALSA card0 and /dev/fastrpc-adsp exist before HALs start
for r in /sys/class/remoteproc/remoteproc*; do
    name=$(cat "$r/name" 2>/dev/null)
    if [ "$name" = "adsp" ] || [ "$name" = "cdsp" ]; then
        if [ "$(cat "$r/state" 2>/dev/null)" = "offline" ]; then
            echo start > "$r/state" 2>/dev/null || true
        fi
    fi
done

# Wait briefly for /dev/fastrpc-adsp before starting hexagonrpcd
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    [ -c /dev/fastrpc-adsp ] && break
    sleep 0.2
done

# Rebind WCN3991 Bluetooth UART so firmware downloads cleanly before BT AIDL HAL opens HCI_CHANNEL_USER
if [ -e /sys/bus/serial/drivers/hci_uart_qca/serial0-0 ]; then
    echo serial0-0 > /sys/bus/serial/drivers/hci_uart_qca/unbind 2>/dev/null || true
    sleep 0.2
    echo serial0-0 > /sys/bus/serial/drivers/hci_uart_qca/bind 2>/dev/null || true
fi
