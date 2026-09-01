#!/bin/sh
# GPU / SoC survey for hardware-decode bundle selection. POSIX sh (busybox-safe:
# muOS/RockNIX ship no bash). Run on any target device; prints everything we key
# off, then a one-line VERDICT usable by scripts:
#   gpu=<adreno|mali-blob|panfrost|unknown> soc=<qcom|rockchip|allwinner|...>
#
# Signals, strongest first:
#   /proc/device-tree/compatible  SoC vendor+model (present on all ARM handhelds)
#   /dev/kgsl-3d0                 Adreno downstream (Qualcomm kgsl) driver
#   /dev/mali0                    Mali blob driver
#   DRM DRIVER= in uevent         msm (Adreno mainline) / panfrost (Mali mainline)
#                                 / rockchip / lima / sun4i-drm

echo "== /proc/device-tree/compatible =="
tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null || echo "(missing)"

echo "== drm cards =="
for c in /sys/class/drm/card*/device/uevent; do
    [ -f "$c" ] && { echo "$c:"; head -3 "$c"; }
done

echo "== gpu devnodes =="
ls /dev/kgsl-3d0 /dev/mali0 /dev/dri 2>/dev/null

echo "== kernel modules =="
lsmod 2>/dev/null | grep -iE 'mali|adreno|msm|panfrost|lima|kgsl' || echo "(none matched)"

echo "== gl libs on device =="
for d in /usr/lib /usr/lib64 /usr/lib/aarch64-linux-gnu /lib; do
    ls "$d"/libmali* "$d"/libEGL* "$d"/libGLES*2* 2>/dev/null
done | sort -u | head -8

echo "== video decode devnodes =="
ls /dev/video* /dev/mpp_service 2>/dev/null | head -8
for v in /sys/class/video4linux/video*; do
    [ -f "$v/name" ] && echo "$v: $(cat "$v/name")"
done

# ---- verdict ----
soc=unknown; gpu=unknown
compat=$(tr '\0' ',' < /proc/device-tree/compatible 2>/dev/null)
case "$compat" in
    *qcom,*)      soc=qcom ;;
    *rockchip,*)  soc=rockchip ;;
    *allwinner,*|*sun50i*) soc=allwinner ;;
    *amlogic,*)   soc=amlogic ;;
esac
if [ -e /dev/kgsl-3d0 ]; then gpu=adreno
elif [ -e /dev/mali0 ]; then gpu=mali-blob
else
    for c in /sys/class/drm/card*/device/uevent; do
        [ -f "$c" ] || continue
        case "$(head -1 "$c")" in
            DRIVER=msm*)     gpu=adreno ;;   # msm / msm_dpu (RP5 reports msm_dpu)
            DRIVER=panfrost) gpu=panfrost ;;
            DRIVER=lima)     gpu=lima ;;
        esac
    done
    # qcom SoC with a msm DRM node but no kgsl = mainline freedreno
    [ "$gpu" = unknown ] && [ "$soc" = qcom ] && gpu=adreno
fi
echo "VERDICT: gpu=$gpu soc=$soc"
