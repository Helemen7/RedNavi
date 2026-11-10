#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# 8 s call-stack sample of WindowServer plus a top snapshot of the busiest processes, to see where the CPU goes while an animation runs (minimize a window right after starting).
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_sample.sh >/dev/null 2>&1 &'   output: /Users/Shared/ws_sample.txt, /Users/Shared/ws_top.txt
PID=$(pgrep -x WindowServer | head -1)
sleep 3
top -l 2 -n 8 -o cpu -stats pid,cpu,command > /Users/Shared/ws_top.txt 2>&1 &
sample $PID 8 -file /Users/Shared/ws_sample.txt >/dev/null 2>&1
chmod 644 /Users/Shared/ws_sample.txt /Users/Shared/ws_top.txt
