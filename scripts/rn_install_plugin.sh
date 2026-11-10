#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Replaces the executable of the installed Metal plug-in (/Library/GPUBundles/RedNaviMTLDriver.bundle) with a new build staged on the desktop and seals the bundle again (ad hoc). The RADV and
# translator libraries inside the bundle stay as they are. WindowServer must be restarted afterwards to load it (sudo killall -9 WindowServer).
# usage (on the desktop): sudo sh /Users/Shared/rn_install_plugin.sh [/Users/Shared/rnstage/RedNaviMTLDriver.bin [/Users/Shared/rnstage/libvulkan_radeon.dylib]]
set -e
NEW=${1:-/Users/Shared/rnstage/RedNaviMTLDriver.bin}
B=/Library/GPUBundles/RedNaviMTLDriver.bundle
[ -f "$NEW" ] || { echo "$NEW not found"; exit 1; }
[ -d "$B" ] || { echo "$B is not installed (scripts/install_mtl_stub.sh installs the whole bundle)"; exit 1; }
cp "$NEW" "$B/Contents/MacOS/RedNaviMTLDriver"
chown root:wheel "$B/Contents/MacOS/RedNaviMTLDriver"
chmod 755 "$B/Contents/MacOS/RedNaviMTLDriver"
# A new RADV (zero-copy IOSurface textures need the bridge's RN_UC_BO_USERPTR import): staged next to the executable as /Users/Shared/rnstage/libvulkan_radeon.dylib, put in the bundle when it differs.
VKLIB=${2:-/Users/Shared/rnstage/libvulkan_radeon.dylib}
if [ -f "$VKLIB" ] && ! cmp -s "$VKLIB" "$B/Contents/Resources/vk/libvulkan_radeon.dylib"; then
  cp "$VKLIB" "$B/Contents/Resources/vk/libvulkan_radeon.dylib"
  chown root:wheel "$B/Contents/Resources/vk/libvulkan_radeon.dylib"
  chmod 755 "$B/Contents/Resources/vk/libvulkan_radeon.dylib"
  xattr -c "$B/Contents/Resources/vk/libvulkan_radeon.dylib"
  codesign -f -s - "$B/Contents/Resources/vk/libvulkan_radeon.dylib"
  echo "libvulkan_radeon.dylib replaced"
fi
xattr -cr "$B"
codesign -f -s - "$B"
codesign --verify --strict "$B" && echo "plug-in replaced and sealed; now: sudo killall -9 WindowServer"
