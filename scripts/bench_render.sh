#!/bin/bash
# flutter_inappwebview Linux 渲染管线基准（WebKitGTK 后端）
#
# 用法:
#   ./bench_render.sh <label>                    # 单管线基准（默认 $DISPLAY）
#   BENCH_DISPLAY=:0 ./bench_render.sh gpu       # 指定显示
#   BENCH_AB=1 ./bench_render.sh ab              # :0 同屏 A/B（GPU 直通 vs snapshot 回退）
#
# 环境变量:
#   BENCH_DISPLAY   目标 X display（默认继承环境）
#   BENCH_X/BENCH_Y webview 内滚动点击点（屏幕绝对坐标；默认需按窗口位置调整）
#   BENCH_IDLE_SECS 默认 5
#   BENCH_SCROLL_SECS 默认 12
#
# 指标（追加到 /tmp/bench_results.log）:
#   - flutter_cpu / webkit_cpu: 进程级 CPU%（/proc stat utime+stime）
#   - fps: app stdout 中的 debug 打点
#     GPU 直通: "WebKitGpuCapture: present fps=N"（damage 驱动，页面变才出帧）
#     snapshot:  "InAppWebView(gtk): snapshot fps=N"（50ms 节拍器上限 ~20fps）
#   实测参考（i915 / 60Hz / flutter.dev / 1280x204）:
#     GPU 直通 scroll 60fps / CPU 0.4%；snapshot scroll 19fps / CPU 0.2-0.4%
#
# 注意:
#   - Xvfb（无 DRI3）下 EGL_KHR_image_pixmap 不可用，GPU 直通必回退 snapshot，
#     因此 fps A/B 必须在真实 X 显示（:0）上跑；Xvfb 仅用于 snapshot 回归
#   - A/B 会短暂弹出 app 窗口抢占前台（每轮约 idle+scroll+8s），跑完自动退出
set -u

APP_CANDIDATES=(
    "$PWD/build/linux/x64/debug/bundle/flutter_inappwebview_example"
    "$PWD/flutter_inappwebview/example/build/linux/x64/debug/bundle/flutter_inappwebview_example"
)
APP=""
for c in "${APP_CANDIDATES[@]}"; do
    [ -x "$c" ] && APP="$c" && break
done
[ -z "$APP" ] && {
    echo "ERROR: 未找到 example 二进制，先 flutter build linux --debug"
    exit 1
}

LABEL="${1:-bench}"
IDLE_SECS="${BENCH_IDLE_SECS:-5}"
SCROLL_SECS="${BENCH_SCROLL_SECS:-12}"
RESULTS=/tmp/bench_results.log

# 滚动点击点：默认假设 app 窗口在主屏左上区域，webview 条带在窗口相对 (400,300)
WX="${BENCH_X:-0}"
WY="${BENCH_Y:-0}"
if [ "${BENCH_X:-}" = "" ] || [ "${BENCH_Y:-}" = "" ]; then
    WID=$(DISPLAY="${BENCH_DISPLAY:-$DISPLAY}" xdotool search --name '^flutter_inappwebview_example$' 2>/dev/null | head -1)
    if [ -n "${WID:-}" ]; then
        read -r wx wy <<<"$(DISPLAY="${BENCH_DISPLAY:-$DISPLAY}" xdotool getwindowgeometry --shell "$WID" |
            grep -E '^(X|Y)=' | tr -d 'X=Y' | tr '\n' ' ')"
        WX=$((wx + 400))
        WY=$((wy + 300))
    fi
fi

group_ticks() { # $1=进程名 pattern -> utime+stime 总 tick
    local sum=0 v p
    for p in $(pgrep -f "$1"); do
        v=$(awk '{print $14+$15}' "/proc/$p/stat" 2>/dev/null) && sum=$((sum + v))
    done
    echo "$sum"
}

phase() { # $1=idle|scroll
    local f0 w0 f1 w1 t0 t1 dir=5
    f0=$(group_ticks flutter_inappwebview_example)
    w0=$(group_ticks WebKitWebProcess)
    t0=$(date +%s.%N)
    if [ "$1" = scroll ]; then
        local end=$((SECONDS + SCROLL_SECS))
        while [ $SECONDS -lt $end ]; do
            DISPLAY="${BENCH_DISPLAY:-$DISPLAY}" xdotool mousemove "$WX" "$WY" click "$dir"
            sleep 0.03
            [ $((SECONDS % 2)) -eq 0 ] && dir=5 || dir=4
        done
    else
        sleep "$IDLE_SECS"
    fi
    t1=$(date +%s.%N)
    f1=$(group_ticks flutter_inappwebview_example)
    w1=$(group_ticks WebKitWebProcess)
    local dt fc wc
    dt=$(echo "$t1 - $t0" | bc)
    fc=$(echo "scale=1;($f1 - $f0)/100/$dt" | bc)
    wc=$(echo "scale=1;($w1 - $w0)/100/$dt" | bc)
    echo "RESULT label=$1 dt=${dt}s flutter_cpu=${fc}% webkit_cpu=${wc}%" | tee -a "$RESULTS"
}

run_one() { # $1=label $2=env 前缀
    pkill -f flutter_inappwebview_example 2>/dev/null
    sleep 1
    LOG="/tmp/bench_app_$1.log"
    env $2 DISPLAY="${BENCH_DISPLAY:-$DISPLAY}" "$APP" >"$LOG" 2>&1 &
    sleep 8
    echo "=== RUN $1 click=($WX,$WY) $(date +%T)" | tee -a "$RESULTS"
    phase idle
    phase scroll
    grep -hE "fps=|GPU direct|pixel buffer" "$LOG" | tail -8
    pkill -f flutter_inappwebview_example 2>/dev/null
    sleep 1
}

echo "=== BENCH $LABEL $(date) win_click=($WX,$WY)" | tee -a "$RESULTS"
if [ "${BENCH_AB:-0}" = "1" ]; then
    # :0 同屏 A/B：GPU 直通（默认） vs 强制 snapshot 回退
    run_one "$LABEL-gpu" "GDK_SCALE=${BENCH_GDK_SCALE:-1}"
    run_one "$LABEL-snapshot" "GDK_SCALE=${BENCH_GDK_SCALE:-1} FLUTTER_INAPPWEBVIEW_LINUX_GPU_CAPTURE=0"
else
    run_one "$LABEL" ""
fi
