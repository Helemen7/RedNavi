#!/bin/sh
# Installs the Metal plug-in stub (RedNaviMTL/) where the accelerator kext's MetalPluginName points: /Library/GPUBundles/RedNaviMTLDriver.bundle.
# Run on the machine with the RX 7800 XT, needs sudo. It only logs (/tmp/rednavi-metal.log) and answers nil.
# usage: sudo scripts/install_mtl_stub.sh path/to/RedNaviMTLDriver.bundle
# Remove: sudo rm -rf /Library/GPUBundles/RedNaviMTLDriver.bundle
set -e
SRC=${1:?usage: install_mtl_stub.sh path/to/RedNaviMTLDriver.bundle}
mkdir -p /Library/GPUBundles
rm -rf /Library/GPUBundles/RedNaviMTLDriver.bundle
cp -R "$SRC" /Library/GPUBundles/RedNaviMTLDriver.bundle
chown -R root:wheel /Library/GPUBundles/RedNaviMTLDriver.bundle
chmod -R 755 /Library/GPUBundles/RedNaviMTLDriver.bundle
codesign -f -s - /Library/GPUBundles/RedNaviMTLDriver.bundle
rm -f /tmp/rednavi-metal.log
echo "Installed. Metal processes started from now on load it; log: /tmp/rednavi-metal.log"
