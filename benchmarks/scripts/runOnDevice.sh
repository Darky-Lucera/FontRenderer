#!/system/bin/sh
# Runs benchmarkDraw on the device, pinned to one core.
# Usage: runOnDevice.sh <core|fastest> [benchmarkDraw arguments...]
#
# Phones mix big and small cores. If Android moves the process from one to another in the middle of a
# measure, the time is wrong, so the benchmark always runs on the same core.

cd "$(dirname "$0")" || exit 1
chmod 755 ./benchmarkDraw

core=$1
shift

# With the screen off, Android lowers the maximum frequency of the big cores and parks some of them.
# Some phones do not let adb change the setting that keeps the screen on, so a loop wakes it again and
# again, more often than the lock screen turns it off.
WakeScreenLoop() {
    while true; do
        input keyevent KEYCODE_WAKEUP
        sleep 5
    done
}

WakeScreenLoop &
waker=$!
trap 'kill "$waker" 2>/dev/null' EXIT
trap 'exit 1' HUP INT TERM
sleep 2

MaxFreq() {
    cat "/sys/devices/system/cpu/cpu$1/cpufreq/cpuinfo_max_freq" 2>/dev/null
}

if ! command -v taskset >/dev/null 2>&1; then
    echo "No taskset: the benchmark is not pinned to one core" >&2
    ./benchmarkDraw "$@"
    exit $?
fi

# A parked core rejects the affinity, so "fastest" takes the fastest core that accepts it.
if [ "$core" = "fastest" ]; then
    core=
    for freq in $(cat /sys/devices/system/cpu/cpu[0-9]*/cpufreq/cpuinfo_max_freq | sort -rnu); do
        for path in /sys/devices/system/cpu/cpu[0-9]*; do
            candidate=${path##*cpu}
            if [ "$(MaxFreq "$candidate")" = "$freq" ] && taskset "$(printf '%x' $((1 << candidate)))" true 2>/dev/null; then
                core=$candidate
                break 2
            fi
        done
    done
fi

if [ -z "$core" ]; then
    echo "No core accepts the affinity" >&2
    exit 1
fi

fastest=$(cat /sys/devices/system/cpu/cpu[0-9]*/cpufreq/cpuinfo_max_freq | sort -rn | head -n 1)
echo "Core $core, $(MaxFreq "$core") kHz max" >&2
if [ "$(MaxFreq "$core")" != "$fastest" ]; then
    echo "A core of $fastest kHz exists, but Android does not let the benchmark use it now" >&2
fi

# Each "input" starts a Java process, which must not take time from the benchmark core.
others=0
for path in /sys/devices/system/cpu/cpu[0-9]*; do
    candidate=${path##*cpu}
    if [ "$candidate" != "$core" ]; then
        others=$((others | (1 << candidate)))
    fi
done
taskset -p "$(printf '%x' $others)" "$waker" >/dev/null 2>&1

taskset "$(printf '%x' $((1 << core)))" ./benchmarkDraw "$@"
exit $?
