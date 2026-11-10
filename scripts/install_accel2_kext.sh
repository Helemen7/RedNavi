#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Installs RedNaviAccel2.kext (docs/M6_ACCELERATOR_STUDY.md, "RedNaviAccel2") into /Library/Extensions so that macOS puts it in the auxiliary kernel collection.
# Same reasons and prerequisites as install_accel_kext.sh. The kext does nothing unless boot-arg rednavi_accel2=<stage> is set (1: log only, 2: let the family's start()
# run with the hooks answered); with the boot-arg set it replaces RedNaviAccel for the card (probe score 200 against 100), which stays installed as the baseline.
# usage: sudo scripts/install_accel2_kext.sh path/to/RedNaviAccel2.kext
# To remove it again: sudo rm -rf /Library/Extensions/RedNaviAccel2.kext, then reboot (if the desktop does not come up, remove the boot-arg from the internal EFI instead).
set -e
SRC=${1:?usage: install_accel2_kext.sh path/to/RedNaviAccel2.kext}
rm -rf /Library/Extensions/RedNaviAccel2.kext
cp -R "$SRC" /Library/Extensions/RedNaviAccel2.kext
chown -R root:wheel /Library/Extensions/RedNaviAccel2.kext
chmod -R 755 /Library/Extensions/RedNaviAccel2.kext
codesign -f -s - /Library/Extensions/RedNaviAccel2.kext
kmutil load -p /Library/Extensions/RedNaviAccel2.kext
echo "Loaded into the AuxKC. After the boot-arg is set and the machine rebooted, read: ioreg -rc IOPCIDevice -n GFX0 -l | grep rednavi-accel2-log  (the log is a property of the PCI device)"
