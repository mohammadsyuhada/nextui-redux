#!/bin/sh
cd "$(dirname "$0")"

# Set CPU frequency for music player (power saving: 408-1000 MHz)
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo 408000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo 1000000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq

./musicplayer.elf &> "$LOGS_PATH/music-player.txt"

# Restore default CPU settings on exit
echo 408000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo 2000000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
