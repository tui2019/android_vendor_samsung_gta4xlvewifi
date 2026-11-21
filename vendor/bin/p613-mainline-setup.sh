#!/vendor/bin/sh
# Prepare mainline 7.3 stitched firmware, HexagonFS for ADSP sensorspd,
# and boot ADSP/CDSP + rebind WCN3991 Bluetooth early before HALs start.

FW_ROOT=/data/vendor/firmware
FW_DEV=$FW_ROOT/qcom/sm7125/Samsung/gta4xlvewifi

mkdir -p "$FW_DEV" "$FW_ROOT/ath10k/WCN3990/hw1.0" "$FW_ROOT/qca" "$FW_ROOT/qcom" /data/vendor/tmp/tqftpserv
chmod 0755 "$FW_ROOT" "$FW_ROOT/qcom" "$FW_ROOT/qcom/sm7125" "$FW_ROOT/qcom/sm7125/Samsung" "$FW_DEV" \
           "$FW_ROOT/ath10k" "$FW_ROOT/ath10k/WCN3990" "$FW_ROOT/ath10k/WCN3990/hw1.0" "$FW_ROOT/qca"
chmod 0777 /data/vendor/tmp/tqftpserv

# Stitch Qualcomm split ELF (.mdt + .bXX) into monolithic .mbn once (survives factory reset)
for fw in adsp cdsp ipa_fws venus a615_zap; do
    if [ ! -s "$FW_DEV/${fw}.mbn" ] && [ -f "/vendor/firmware_mnt/image/${fw}.mdt" ]; then
        /vendor/bin/mdt_stitch "/vendor/firmware_mnt/image/${fw}.mdt" "$FW_DEV/${fw}.mbn"
    fi
done
if [ ! -s "$FW_DEV/modem.mbn" ] && [ -f "/vendor/firmware-modem/image/modem.mdt" ]; then
    /vendor/bin/mdt_stitch "/vendor/firmware-modem/image/modem.mdt" "$FW_DEV/modem.mbn"
fi

# Symlink/copy remaining mainline firmware files
ln -sf /vendor/firmware-modem/image/modem_pr "$FW_DEV/modem_pr"
[ -f "$FW_DEV/wlanmdsp.mbn" ] || cp -f /vendor/firmware/wlanmdsp.mbn "$FW_DEV/wlanmdsp.mbn" 2>/dev/null
[ -f "$FW_DEV/Data.msc" ] || cp -f /vendor/firmware/Data.msc "$FW_DEV/Data.msc" 2>/dev/null
[ -f "$FW_DEV/bdwlan.bin" ] || cp -f /vendor/firmware/wlan/qca_cld/bdwlan.bin "$FW_DEV/bdwlan.bin" 2>/dev/null
[ -f "$FW_ROOT/ath10k/WCN3990/hw1.0/firmware-5.bin" ] || cp -f /vendor/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin "$FW_ROOT/ath10k/WCN3990/hw1.0/firmware-5.bin" 2>/dev/null
[ -f "$FW_ROOT/ath10k/WCN3990/hw1.0/board.bin" ] || cp -f /vendor/firmware/wlan/qca_cld/bdwlan.bin "$FW_ROOT/ath10k/WCN3990/hw1.0/board.bin" 2>/dev/null
[ -f "$FW_ROOT/qca/crbtfw32.tlv" ] || cp -f /vendor/firmware/crbtfw32.tlv /vendor/firmware/crnv32.bin /vendor/firmware/crnv32u.bin "$FW_ROOT/qca/" 2>/dev/null
[ -f "$FW_ROOT/qcom/a630_sqe.fw" ] || cp -f /vendor/firmware/a630_sqe.fw /vendor/firmware/a630_gmu.bin "$FW_ROOT/qcom/" 2>/dev/null
[ -f "$FW_ROOT/regulatory.db" ] || cp -f /vendor/firmware/regulatory.db /vendor/firmware/regulatory.db.p7s "$FW_ROOT/" 2>/dev/null

# Point kernel firmware loader to stitched directory
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
