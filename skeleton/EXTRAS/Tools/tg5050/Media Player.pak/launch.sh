#!/bin/sh
cd "$(dirname "$0")"

# Bring big cores online for video decoding (default: only cpu4)
echo 1 > /sys/devices/system/cpu/cpu5/online 2>/dev/null
echo 1 > /sys/devices/system/cpu/cpu6/online 2>/dev/null
echo 1 > /sys/devices/system/cpu/cpu7/online 2>/dev/null

# Video decoding needs dynamic scaling on big cluster
echo schedutil > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor 2>/dev/null
echo 408000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq 2>/dev/null
echo 2160000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq 2>/dev/null

# GPU: lock to performance for hardware-accelerated video rendering
echo performance > /sys/devices/platform/soc@3000000/1800000.gpu/devfreq/1800000.gpu/governor 2>/dev/null

export SDL_NOMOUSE=1
export HOME=/mnt/SDCARD
mkdir -p /mnt/SDCARD/Videos

./mediaplayer.elf &> "$LOGS_PATH/media-player.txt"

# Restore default core state (only cpu0,1,4 online)
echo 0 > /sys/devices/system/cpu/cpu5/online 2>/dev/null
echo 0 > /sys/devices/system/cpu/cpu6/online 2>/dev/null
echo 0 > /sys/devices/system/cpu/cpu7/online 2>/dev/null
