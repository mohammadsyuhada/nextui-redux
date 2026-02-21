#!/bin/sh

cd $(dirname "$0")

# Set CPU frequency for input app (power saving: 600 MHz)
echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo 600000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed 2>/dev/null

./input.elf &> "$LOGS_PATH/.txt"

# Restore default CPU governor on exit
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
