#!/bin/sh
# Installs the kexts the desktop is tested with from a staging folder into /Library/Extensions and loads them into the auxiliary kernel collection, in one command. The kexts are signed
# when they are built (Makefile, ad hoc), so there is no signing step here; one that is not validly signed is signed ad hoc as a fallback. A kext whose executable did not change is
# left alone (no reinstall, no need for the reboot that goes with it).
# usage (on the desktop, from its Terminal): sudo sh /Users/Shared/rn_install.sh [-r] RedNaviFB RedNaviAccel2 ...      (-r: reboot afterwards when something was installed)
# The kexts are taken from /Users/Shared/rnfb/<name>.kext (staged with rsync by the machine the work is done on). Remove one with: sudo rm -rf /Library/Extensions/<name>.kext, then reboot.
set -e
STAGE=${RN_STAGE:-/Users/Shared/rnfb}
REBOOT=0
if [ "$1" = "-r" ]; then REBOOT=1; shift; fi
[ $# -gt 0 ] || { echo "usage: sudo sh $0 [-r] RedNaviFB RedNaviAccel2 ..."; exit 2; }
CHANGED=0
for N in "$@"; do
  SRC="$STAGE/$N.kext"; DST="/Library/Extensions/$N.kext"
  [ -d "$SRC" ] || { echo "$N: $SRC not found"; exit 1; }
  if [ -f "$DST/Contents/MacOS/$N" ] && cmp -s "$SRC/Contents/MacOS/$N" "$DST/Contents/MacOS/$N" && cmp -s "$SRC/Contents/Info.plist" "$DST/Contents/Info.plist"; then
    echo "$N: already installed and identical, left alone"; continue
  fi
  rm -rf "$DST"
  cp -R "$SRC" "$DST"
  chown -R root:wheel "$DST"
  chmod -R 755 "$DST"
  if ! codesign --verify --strict "$DST" 2>/dev/null; then echo "$N: not validly signed, signing ad hoc"; codesign -f -s - "$DST"; fi
  kmutil load -p "$DST"
  echo "$N: installed and loaded"
  CHANGED=1
done
if [ "$CHANGED" = 1 ] && [ "$REBOOT" = 1 ]; then echo "rebooting"; shutdown -r now; fi
