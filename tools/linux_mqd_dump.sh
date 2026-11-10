#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Collects what Linux uses for its compute queues, to compare with RedNavi's writeComputeMqd (docs/COMPAT.md). Run as root on Linux with amdgpu
# loaded on the RX 7800 XT and a GPU workload running (vkcube), then send the resulting file:
#
#     sudo sh tools/linux_mqd_dump.sh [output file]
#
# Read-only. The MQD files exist only when the kernel offers them in debugfs; the ring list and the dmesg lines are always collected.
OUT="${1:-linux_mqd_dump.txt}"
{
  echo "# kernel: $(uname -r)"
  echo "# ring names (VM invalidation engines):"
  dmesg | grep -E "uses VM inv eng|kiq ring mec|MES:" 
  for d in /sys/kernel/debug/dri/*/; do
    [ -e "$d/amdgpu_regs" ] || continue
    echo "# debugfs dir: $d"
    ls "$d" | grep -iE "mqd|ring|mes|kiq|hqd"
    for f in "$d"/amdgpu_mqd_* "$d"/amdgpu_ring_comp_1.0.0 "$d"/amdgpu_ring_mes_kiq_3.1.0 "$d"/amdgpu_ring_mes_3.0.0; do
      [ -e "$f" ] || continue
      echo "# file: $f"
      xxd -l 4096 "$f"
    done
  done
  echo "# umr:"
  if command -v umr >/dev/null 2>&1; then
    umr -O bits -r "*.*.CP_HQD_PQ_CONTROL" 2>&1 | head -40
    umr -O bits -r "*.*.CP_HQD_QUANTUM" 2>&1 | head -20
    umr -O bits -r "*.*.CP_HQD_PERSISTENT_STATE" 2>&1 | head -20
  else
    echo "umr not installed"
  fi
  echo "# done"
} > "$OUT" 2>&1
echo "wrote $OUT"
