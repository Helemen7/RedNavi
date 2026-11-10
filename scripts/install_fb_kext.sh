#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Installs RedNaviFB.kext into /Library/Extensions so macOS puts it in the auxiliary kernel collection.
# RedNaviFB needs IOGraphicsFamily, which lives in the system kernel collection, so OpenCore cannot inject it
# the way it does RedNavi.kext. Run on the machine with the RX 7800 XT, needs sudo.
# usage: sudo scripts/install_fb_kext.sh path/to/RedNaviFB.kext
set -e
SRC=${1:?usage: install_fb_kext.sh path/to/RedNaviFB.kext}
rm -rf /Library/Extensions/RedNaviFB.kext
cp -R "$SRC" /Library/Extensions/RedNaviFB.kext
chown -R root:wheel /Library/Extensions/RedNaviFB.kext
chmod -R 755 /Library/Extensions/RedNaviFB.kext
kmutil install --volume-root / --update-all
echo "Installed. If macOS asks to approve the extension, allow it in System Settings > Privacy & Security, then reboot."
