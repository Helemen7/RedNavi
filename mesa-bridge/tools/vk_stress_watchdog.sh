#!/bin/bash
# usage: runwd.sh SECONDS args...   run vk_stress on the desktop with a watchdog; prints the tail
S=$1; shift
ssh helemen7@mac-desktop.lab "cd ~/rn; ./rn_test_ring reset | tail -1; cd vk; rm -f /tmp/st.out; (./run_tool.sh vk_stress $* > /tmp/st.out 2>&1; echo exit=\$? >> /tmp/st.out) & for i in \$(seq 1 $S); do sleep 1; grep -q '^exit=' /tmp/st.out && break; done; tail -25 /tmp/st.out; grep -q '^exit=' /tmp/st.out || { echo TIMEOUT; pkill vk_stress; }; true"
