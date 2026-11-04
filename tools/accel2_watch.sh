#!/bin/sh
# Follows the log of RedNaviAccel2 (property rednavi-accel2-log of the PCI device) on the test desktop and prints every line once, as it appears. A kernel panic loses the
# property, so the last lines printed are what the kext did right before it (docs/M6_ACCELERATOR_STUDY.md). Stops when ssh loses the machine.
# usage: tools/accel2_watch.sh [user@host]
HOST=${1:-helemen7@mac-desktop.lab}
ssh -o ConnectTimeout=5 -o ServerAliveInterval=2 "$HOST" '
last=0
while :; do
  ioreg -l -w0 | grep "rednavi-accel2-log" | sed "s/.*= (//; s/)\$//; s/\",\"/\n/g; s/^\"//; s/\"\$//" > /tmp/rnaccel2.cur
  n=$(wc -l < /tmp/rnaccel2.cur)
  if [ "$n" -gt "$last" ]; then tail -n +$((last+1)) /tmp/rnaccel2.cur; last=$n; fi
  sleep 0.2
done'
