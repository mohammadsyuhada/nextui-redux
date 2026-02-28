#!/bin/sh

EMU_EXE=pcsx_rearmed

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
HOME="$USERDATA_PATH"
cd "$HOME"

# BIG cluster: bring cpu5 online for dual-core emulation
echo 1 >/sys/devices/system/cpu/cpu5/online 2>/dev/null

# GPU: lock to performance for PS1 hardware-accelerated OpenGL rendering
echo performance >/sys/devices/platform/soc@3000000/1800000.gpu/devfreq/1800000.gpu/governor 2>/dev/null

# Launch on big cores, then pin threads after startup
taskset -c 4,5 minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt" &
EMU_PID=$!
sleep 2

# Thread pinning helper
pin_threads() {
    for TID_PATH in /proc/$EMU_PID/task/*; do
        TID=$(basename "$TID_PATH")
        TNAME=$(cat "$TID_PATH/comm" 2>/dev/null)
        case "$TNAME" in
            pcsxr-drc)
                taskset -p -c 4 "$TID" 2>/dev/null ;;
            pcsxr-gpu|mali-*|PrepareFrameThr)
                taskset -p -c 5 "$TID" 2>/dev/null ;;
            SDLAudioP2|SDLHotplug*|pcsxr-cdrom)
                taskset -p -c 0,1 "$TID" 2>/dev/null ;;
        esac
    done
    taskset -p -c 4 "$EMU_PID" 2>/dev/null
}

# Thread pinning:
#   pcsxr-drc (CPU emu + recompiler) → BIG cpu4 (dedicated)
#   pcsxr-gpu + mali + PrepareFrame  → BIG cpu5 (dedicated)
#   audio/cdrom/hotplug              → LITTLE cpu0-1
pin_threads

# Second pass: catch late-spawning threads (e.g. pcsxr-gpu with threaded rendering)
sleep 3
pin_threads

wait $EMU_PID
