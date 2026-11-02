#!/bin/sh
# Installs RedNaviAccel.kext (the accelerator skeleton, docs/M6_ACCELERATOR_STUDY.md) into /Library/Extensions so that macOS puts it in the auxiliary kernel collection.
# Like RedNaviFB it depends on kexts of the system kernel collection (IOAcceleratorFamily2, IOGraphicsFamily), so OpenCore cannot inject it. Run on the machine with the
# RX 7800 XT, needs sudo, SIP configured as for RedNaviFB (docs/DP_BRINGUP.md).
# usage: sudo scripts/install_accel_kext.sh path/to/RedNaviAccel.kext
# To remove it again: sudo rm -rf /Library/Extensions/RedNaviAccel.kext, then reboot (boot-arg -rednavi... is not needed; nothing else depends on it).
set -e
SRC=${1:?usage: install_accel_kext.sh path/to/RedNaviAccel.kext}
rm -rf /Library/Extensions/RedNaviAccel.kext
cp -R "$SRC" /Library/Extensions/RedNaviAccel.kext
chown -R root:wheel /Library/Extensions/RedNaviAccel.kext
chmod -R 755 /Library/Extensions/RedNaviAccel.kext
codesign -f -s - /Library/Extensions/RedNaviAccel.kext
kmutil load -p /Library/Extensions/RedNaviAccel.kext
echo "Loaded. Read the result with: ioreg -rn RedNaviAccel -l   (properties accel-class-info, accel-class-missing, call-log, open-count, call-count, probe-count)"
