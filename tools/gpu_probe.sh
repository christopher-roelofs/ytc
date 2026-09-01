#!/bin/sh
# YTC hardware report — GPU / SoC / video-decoder survey for hardware-decode
# support. POSIX sh (busybox-safe: CFW handhelds ship no bash), no dependencies.
#
# Users: run this on your device and share the printed report (it is also
# saved as ytc_hw_report.txt next to wherever you ran it):
#   curl -L https://raw.githubusercontent.com/christopher-roelofs/ytc/main/tools/gpu_probe.sh | sh
#
# Signals, strongest first:
#   /proc/device-tree/compatible  SoC vendor+model (present on all ARM handhelds)
#   /dev/kgsl-3d0                 Adreno downstream (Qualcomm kgsl) driver
#   /dev/mali0                    Mali blob driver
#   DRIVER= in drm uevent         msm* (Adreno mainline) / panfrost / lima
#   /sys/class/video4linux        the actual video decode/encode hardware
# Known-device results live in docs/HWDEC_SURVEY.md.

REPORT_VERSION=2

report() {
    echo "== ytc hardware report v$REPORT_VERSION =="
    echo "date: $(date 2>/dev/null)"

    echo "== os =="
    grep -E "^(NAME|OS_NAME|VERSION|OS_VERSION|PRETTY_NAME)=" /etc/os-release 2>/dev/null
    uname -a 2>/dev/null

    echo "== cpu/mem =="
    grep -m1 -iE "model name|Hardware" /proc/cpuinfo 2>/dev/null
    echo "cores: $(grep -c ^processor /proc/cpuinfo 2>/dev/null)"
    grep MemTotal /proc/meminfo 2>/dev/null

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
    soc=unknown; gpu=unknown; decoder=none
    compat=$(tr '\0' ',' < /proc/device-tree/compatible 2>/dev/null)
    case "$compat" in
        *qcom,*)      soc=qcom ;;
        *rockchip,*)  soc=rockchip ;;
        *allwinner,*|*sun50i*) soc=allwinner ;;
        *amlogic,*)   soc=amlogic ;;
        *mediatek,*)  soc=mediatek ;;
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
    for v in /sys/class/video4linux/video*; do
        [ -f "$v/name" ] || continue
        n=$(cat "$v/name")
        case "$n" in
            *enc*) ;;                      # encoders don't count
            *dec*) decoder=$n; break ;;
        esac
    done
    echo "VERDICT: gpu=$gpu soc=$soc decoder=$decoder"
}

report 2>/dev/null | tee ytc_hw_report.txt 2>/dev/null || report
echo ""
echo "Saved as ytc_hw_report.txt - please share the report above."
