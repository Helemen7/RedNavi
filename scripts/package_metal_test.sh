#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Assembles the Metal test package for a machine with any supported card (docs/M6_METAL_ON_RADV.md): the Metal plug-in bundle with RADV, the translator and their libraries
# inside it (no MacPorts needed on the target), RedNaviAccel2.kext, and an install script for the target.
#
# usage: scripts/package_metal_test.sh VKDIR OUTDIR
#   VKDIR   a folder with the libraries that go to RedNaviMTLDriver.bundle/Contents/Resources/vk: libvulkan.1.dylib, libLLVM.dylib, libSPIRV-Tools.dylib, librn_air2spirv.dylib,
#           libzstd.1.dylib and what they need (the folder of an installed plug-in has them all). It also takes libvulkan_radeon.dylib if the Mesa build is not given below.
#   OUTDIR  where the package folder is made (must not exist).
# Environment: RADV=path/to/libvulkan_radeon.dylib  a Mesa build to use instead of the one in VKDIR (its MacPorts references are rewritten to the files in VKDIR, or to /usr/lib)
#              CONFIG=Debug|Release                   which build of the kext and the plug-in to take (default Debug; run make first, `make mtl` builds the plug-in)
# Nothing in the package is signed for distribution: it is ad hoc signed, the target runs install.sh as root.
set -e
VKDIR=${1:?usage: package_metal_test.sh VKDIR OUTDIR}
OUT=${2:?usage: package_metal_test.sh VKDIR OUTDIR}
CONFIG=${CONFIG:-Debug}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -d "$VKDIR" ] || { echo "$VKDIR is not a folder"; exit 1; }
[ ! -e "$OUT" ] || { echo "$OUT exists"; exit 1; }
KEXT="$ROOT/build/$CONFIG/RedNaviAccel2.kext"
BUNDLE="$ROOT/build/$CONFIG/RedNaviMTLDriver.bundle"
[ -d "$KEXT" ] || { echo "$KEXT not built (make DEBUG=1)"; exit 1; }
[ -d "$BUNDLE" ] || { echo "$BUNDLE not built (make DEBUG=1 mtl)"; exit 1; }
for f in libvulkan.1.dylib libLLVM.dylib libSPIRV-Tools.dylib librn_air2spirv.dylib libzstd.1.dylib; do
  [ -f "$VKDIR/$f" ] || { echo "$VKDIR/$f missing"; exit 1; }
done

mkdir -p "$OUT"
cp -R "$KEXT" "$OUT/RedNaviAccel2.kext"
cp -R "$BUNDLE" "$OUT/RedNaviMTLDriver.bundle"
V="$OUT/RedNaviMTLDriver.bundle/Contents/Resources/vk"
rm -rf "$V"; mkdir -p "$V"
cp -P "$VKDIR"/*.dylib "$VKDIR"/rn_icd.json "$V"/ 2>/dev/null || true
[ -n "$RADV" ] && cp "$RADV" "$V/libvulkan_radeon.dylib"
[ -f "$V/libvulkan_radeon.dylib" ] || { echo "no libvulkan_radeon.dylib (give RADV=...)"; exit 1; }
[ -f "$V/rn_icd.json" ] || printf '%s\n' '{"file_format_version":"1.0.1","ICD":{"library_path":"./libvulkan_radeon.dylib","api_version":"1.4.0"}}' > "$V/rn_icd.json"

# Every reference to a MacPorts path becomes a reference next to the library (or to the system's copy); a reference that has neither is an error.
for f in "$V"/*.dylib; do
  [ -L "$f" ] && continue
  chmod u+w "$f"
  otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -E '^/opt/local|^/Users/' | while read -r dep; do
    base=$(basename "$dep")
    if [ -f "$V/$base" ]; then new="@loader_path/$base"
    elif [ -f "/usr/lib/$base" ] || [ "$base" = libz.1.dylib ]; then new="/usr/lib/$base"      # libz is in the dyld shared cache, not a file
    else echo "$f needs $dep, which is neither in $VKDIR nor in /usr/lib"; exit 1; fi
    install_name_tool -change "$dep" "$new" "$f"
  done
  id=$(otool -D "$f" | tail -n 1)
  case "$id" in /opt/local/*|/Users/*) install_name_tool -id "@rpath/$(basename "$f")" "$f" ;; esac
  # a library that carries an rpath into MacPorts loses it
  otool -l "$f" | awk '/LC_RPATH/{r=1} r&&/path /{print $2; r=0}' | grep -E '^/opt/local|^/Users/' | while read -r rp; do install_name_tool -delete_rpath "$rp" "$f"; done
done
bad=$(for f in "$V"/*.dylib; do [ -L "$f" ] && continue; otool -L "$f" | tail -n +2 | awk -v f="$(basename "$f")" '$1 ~ /^\/opt\/local|^\/Users\// {print f": "$1}'; done)
[ -z "$bad" ] || { echo "references to the build machine remain:"; echo "$bad"; exit 1; }
for f in "$V"/*.dylib; do [ -L "$f" ] || codesign -f -s - "$f" >/dev/null 2>&1; done
xattr -cr "$OUT/RedNaviMTLDriver.bundle"
codesign -f -s - "$OUT/RedNaviMTLDriver.bundle"
codesign --verify --strict "$OUT/RedNaviMTLDriver.bundle"

cat > "$OUT/install.sh" <<'EOF'
#!/bin/sh
# Installs the Metal test package on this Mac: the accelerator kext (auxiliary kernel collection, SIP must allow it as for RedNaviFB, docs/DP_BRINGUP.md) and the plug-in bundle in
# /Library/GPUBundles. Run as root from this folder: sudo sh install.sh. Then add the boot-arg rednavi_accel2=3 to the OpenCore config and reboot (README.txt).
set -e
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }
xattr -cr RedNaviAccel2.kext RedNaviMTLDriver.bundle
rm -rf /Library/Extensions/RedNaviAccel2.kext
cp -R RedNaviAccel2.kext /Library/Extensions/RedNaviAccel2.kext
chown -R root:wheel /Library/Extensions/RedNaviAccel2.kext
chmod -R 755 /Library/Extensions/RedNaviAccel2.kext
codesign -f -s - /Library/Extensions/RedNaviAccel2.kext
mkdir -p /Library/GPUBundles
rm -rf /Library/GPUBundles/RedNaviMTLDriver.bundle
cp -R RedNaviMTLDriver.bundle /Library/GPUBundles/RedNaviMTLDriver.bundle
chown -R root:wheel /Library/GPUBundles/RedNaviMTLDriver.bundle
codesign -f -s - /Library/GPUBundles/RedNaviMTLDriver.bundle
# A kext of the same name already in the auxiliary kernel collection is kept by `kmutil load` (the first Navi 33 test kept running the old build): rebuild the collection.
kmutil install --update-all || echo "kmutil install --update-all failed: run it again by hand"
echo "installed kext build (the UUID the boot must load, see kmutil showloaded --list-only | grep -i rednavi):"
dwarfdump -u /Library/Extensions/RedNaviAccel2.kext/Contents/MacOS/RedNaviAccel2
ls -la /Library/KernelCollections/
echo "installed. Approve the kext in System Settings > Privacy & Security if it asks, set the boot-arg and reboot."
EOF
chmod +x "$OUT/install.sh"
cat > "$OUT/README.txt" <<'EOF'
RedNavi Metal test: the Metal plug-in on RADV, for a card the RedNavi kext already drives (compute and graphics queues working, see docs/NAVI33_LADDER.md).

Needs: the RedNavi kext loaded with the stages of its rung 4c plus rednavi_gfx_queue=1; SIP set as for RedNaviFB (csr-active-config FF0F0000); an admin password.

1. In Terminal, in this folder:     sudo sh install.sh
   (approve the kext in System Settings > Privacy & Security if it asks)
2. Add  rednavi_accel2=3 rednavi_accel2_delay=90  to the boot-args of the OpenCore config.plist, reboot.
3. About two minutes after the boot, send me the output of:
     ioreg -rc IOAccelerator -l | grep -E "RedNaviAccel2|registered"
     ioreg -rc IOPCIDevice -n GFX0 -l | grep rednavi-accel2-log
   (if the card is not GFX0 in your ioreg, the node with the vendor id 1002 and the rednavi-accel2-log property)
To remove: sudo rm -rf /Library/Extensions/RedNaviAccel2.kext /Library/GPUBundles/RedNaviMTLDriver.bundle, remove the two boot-args, reboot.
EOF
echo "package in $OUT"
du -sh "$OUT"
