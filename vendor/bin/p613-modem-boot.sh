#!/vendor/bin/sh
# Boot MPSS modem (WLAN PD -> ath10k_snoc wlan0) after tqftpserv and rmtfs are up,
# then apply Samsung factory Wi-Fi MAC address from EFS.

sleep 1

for r in /sys/class/remoteproc/remoteproc*; do
    name=$(cat "$r/name" 2>/dev/null)
    if [ "$name" = "modem" ]; then
        if [ "$(cat "$r/state" 2>/dev/null)" = "offline" ]; then
            echo start > "$r/state" 2>/dev/null || true
        fi
    fi
done

MAC=$(cat /mnt/vendor/efs/wifi/.mac.info 2>/dev/null | tr -d '\r\n')
for i in $(seq 1 60); do
    if [ -d /sys/class/net/wlan0 ]; then
        if [ -n "$MAC" ]; then
            ip link set wlan0 down 2>/dev/null || true
            ip link set wlan0 address "$MAC" 2>/dev/null || true
        fi
        break
    fi
    sleep 1
done
