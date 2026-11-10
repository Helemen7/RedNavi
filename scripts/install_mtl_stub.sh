#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Installs the Metal plug-in stub (RedNaviMTL/) where the accelerator kext's MetalPluginName points: /Library/GPUBundles/RedNaviMTLDriver.bundle.
# Run on the machine with the RX 7800 XT, needs sudo. It logs (/tmp/rednavi-metal-<uid>.log): the methods Metal expects, the init methods of the device with the IOKit calls made inside them, and answers nil to what nobody implements.
# usage: sudo scripts/install_mtl_stub.sh path/to/RedNaviMTLDriver.bundle [directory with libvulkan_radeon.dylib and libzstd.1.dylib]
# With the second argument the RADV libraries of the compute path (RedNaviMTL/rn_vk.c) are copied to /Library/Application Support/RedNavi/vk, where the plug-in looks for them.
# Remove: sudo rm -rf /Library/GPUBundles/RedNaviMTLDriver.bundle
set -e
SRC=${1:?usage: install_mtl_stub.sh path/to/RedNaviMTLDriver.bundle}
mkdir -p /Library/GPUBundles
rm -rf /Library/GPUBundles/RedNaviMTLDriver.bundle
cp -R "$SRC" /Library/GPUBundles/RedNaviMTLDriver.bundle
chown -R root:wheel /Library/GPUBundles/RedNaviMTLDriver.bundle
chmod -R 755 /Library/GPUBundles/RedNaviMTLDriver.bundle
xattr -cr /Library/GPUBundles/RedNaviMTLDriver.bundle
codesign -f -s - /Library/GPUBundles/RedNaviMTLDriver.bundle
if [ -n "$2" ]; then
    mkdir -p "/Library/Application Support/RedNavi/vk"
    cp "$2/libvulkan_radeon.dylib" "$2/libzstd.1.dylib" "/Library/Application Support/RedNavi/vk/"
    chown -R root:wheel "/Library/Application Support/RedNavi"
    chmod -R 755 "/Library/Application Support/RedNavi"
    echo "RADV libraries installed in /Library/Application Support/RedNavi/vk"
fi
rm -f /tmp/rednavi-metal.log /tmp/rednavi-metal-*.log
echo "Installed. Metal processes started from now on load it; log: /tmp/rednavi-metal-<uid>.log (one per user)"
