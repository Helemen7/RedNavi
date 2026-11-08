#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Capture RX7600 / Navi33 amdgpu reference state from *native* Linux.
# This script never writes GPU registers, runs a ring test, resets the GPU,
# or reads debugfs controls whose read handler has side effects.
set -Eeuo pipefail
export LC_ALL=C

if grep -qiE 'microsoft|wsl' /proc/sys/kernel/osrelease 2>/dev/null; then
  echo "ERROR: WSL2 does not expose native amdgpu PCI/PSP/MES state. Boot native Linux." >&2
  exit 2
fi

out=${1:-"./navi33-linux-reference-$(date -u +%Y%m%d-%H%M%S)"}
mkdir -p "$out"
out=$(cd "$out" && pwd -P)
printf 'capture-utc=%s\nkernel=%s\n' "$(date -u -Is)" "$(uname -r)" > "$out/capture.txt"

bdf=""
for dev in /sys/bus/pci/devices/*; do
  [[ -f "$dev/vendor" && -f "$dev/device" ]] || continue
  if [[ $(cat "$dev/vendor") == 0x1002 && $(cat "$dev/device") == 0x7480 ]]; then
    bdf=${dev##*/}
    break
  fi
done
if [[ -z $bdf ]]; then
  echo "ERROR: native PCI device 1002:7480 was not found." >&2
  exit 3
fi
dev=/sys/bus/pci/devices/$bdf
printf 'pci-bdf=%s\ndriver=%s\n' "$bdf" "$(basename "$(readlink -f "$dev/driver" 2>/dev/null || printf '/none')")" >> "$out/capture.txt"

if [[ $(basename "$(readlink -f "$dev/driver" 2>/dev/null || printf '/none')") != amdgpu ]]; then
  echo "ERROR: RX 7600 exists, but the native amdgpu kernel driver is not bound." >&2
  exit 4
fi

{
  printf 'PCI identity and resources\n'
  for entry in vendor device subsystem_vendor subsystem_device revision resource; do
    [[ -r "$dev/$entry" ]] || continue
    echo "$entry:"
    cat "$dev/$entry"
  done
} > "$out/pci.txt"
if command -v lspci >/dev/null 2>&1; then
  lspci -nnk -s "$bdf" > "$out/lspci.txt" 2>&1 || true
fi

# Capture the Linux driver's parsed IP versions and per-instance base offsets.
# All sysfs files here are observational; do not copy writable PCI config.
if [[ -d "$dev/ip_discovery" ]]; then
  while IFS= read -r -d '' f; do
    rel=${f#"$dev/ip_discovery/"}
    case ${f##*/} in
      hw_id|major|minor|revision|base_addr|num_base_addresses|num_instance|harvest)
        printf '%s: ' "$rel" >> "$out/ip_discovery.txt"
        head -c 4096 "$f" >> "$out/ip_discovery.txt" 2>/dev/null || true
        printf '\n' >> "$out/ip_discovery.txt"
        ;;
    esac
  done < <(find -L "$dev/ip_discovery" -type f -print0 | sort -z)
fi

# Detect the DRM primary node representing this exact PCI device.
card=""
for path in /sys/class/drm/card[0-9]*; do
  [[ -e "$path/device" ]] || continue
  if [[ $(readlink -f "$path/device") == "$(readlink -f "$dev")" ]]; then
    card=${path##*/}
    break
  fi
done
if [[ -n $card ]]; then
  printf 'drm-card=%s\n' "$card" >> "$out/capture.txt"
  dbg="/sys/kernel/debug/dri/${card#card}"
  if [[ -d $dbg ]]; then
    # Whitelist ONLY documented observation endpoints. Avoid amdgpu_test_ib,
    # amdgpu_gpu_recover, amdgpu_evict_{vram,gtt}, benchmarks, registers,
    # VRAM and GPU-reset controls: some have side effects even on a read.
    for name in amdgpu_firmware_info amdgpu_fence_info amdgpu_pm_info; do
      if [[ -r "$dbg/$name" ]]; then
        timeout 5 head -c 262144 "$dbg/$name" > "$out/$name.txt" 2>"$out/$name.err" || true
      fi
    done
    # Raw IP Discovery (chip specific; upper bound avoids giant dumps).
    if [[ -r "$dbg/amdgpu_discovery" ]]; then
      timeout 5 head -c 65536 "$dbg/amdgpu_discovery" > "$out/amdgpu_discovery.bin" 2>/dev/null || true
    fi
    # 16 KiB cap per MQD. No ring commands, writes or GPU reset.
    shopt -s nullglob
    for file in "$dbg"/amdgpu_mqd_*; do
      [[ -f "$file" && -r "$file" ]] || continue
      timeout 5 head -c 16384 "$file" > "$out/${file##*/}.bin" 2>/dev/null || true
    done
    shopt -u nullglob
  else
    echo "debugfs unavailable: mount -t debugfs debugfs /sys/kernel/debug (as root)" > "$out/debugfs-note.txt"
  fi
fi

# Curated kernel messages only. Does not modify kernel state.
if command -v dmesg >/dev/null 2>&1; then
  dmesg --color=never 2>/dev/null |
    grep -iE 'amdgpu|psp|mes|sdma|gfxhub|mmhub|firmware|ring .*test'       > "$out/amdgpu-dmesg.txt" || true
fi

# Firmware inventory: hashes are from files on this host, not proof of which
# firmware actually loaded. The firmware-info debugfs file provides versions.
if [[ -d /lib/firmware/amdgpu ]]; then
  find /lib/firmware/amdgpu -maxdepth 1 -type f \
    \( -name '*11_0_2*' -o -name '*6_0_2*' -o -name '*13_0_7*' \) \
    -printf '%f %s bytes\n' | sort > "$out/firmware-files.txt"
fi

# The archive will be sent back for comparison against RedNavi P5.
tar -czf "$out.tar.gz" -C "$(dirname "$out")" "$(basename "$out")"
printf 'Navi33 evidence written to:\n  %s.tar.gz\n' "$out"
printf 'No GPU command submission or reset was requested by this script.\n'
