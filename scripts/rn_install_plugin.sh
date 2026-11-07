#!/bin/sh
# Replaces the executable of the installed Metal plug-in (/Library/GPUBundles/RedNaviMTLDriver.bundle) with a new build staged on the desktop and seals the bundle again (ad hoc). The RADV and
# translator libraries inside the bundle stay as they are. WindowServer must be restarted afterwards to load it (sudo killall -9 WindowServer).
# usage (on the desktop): sudo sh /Users/Shared/rn_install_plugin.sh [/Users/Shared/rnstage/RedNaviMTLDriver.bin]
set -e
NEW=${1:-/Users/Shared/rnstage/RedNaviMTLDriver.bin}
B=/Library/GPUBundles/RedNaviMTLDriver.bundle
[ -f "$NEW" ] || { echo "$NEW not found"; exit 1; }
[ -d "$B" ] || { echo "$B is not installed (scripts/install_mtl_stub.sh installs the whole bundle)"; exit 1; }
cp "$NEW" "$B/Contents/MacOS/RedNaviMTLDriver"
chown root:wheel "$B/Contents/MacOS/RedNaviMTLDriver"
chmod 755 "$B/Contents/MacOS/RedNaviMTLDriver"
xattr -cr "$B"
codesign -f -s - "$B"
codesign --verify --strict "$B" && echo "plug-in replaced and sealed; now: sudo killall -9 WindowServer"
