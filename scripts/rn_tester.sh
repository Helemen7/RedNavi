#!/bin/sh
# One script for testers of a Metal test package (scripts/package_metal_test.sh): it installs the package's Metal plug-in, runs the Metal test suite and the
# Geekbench GPU benchmark (Metal) from the command line, and packs what came out into one zip in the home folder, which is the only thing to send back.
#
#   sudo sh rn_tester.sh                  install the plug-in, run the tests and Geekbench, collect
#   sudo sh rn_tester.sh --no-geekbench   the same without Geekbench
#   sh rn_tester.sh --no-install          tests and Geekbench with the plug-in that is installed already (no sudo needed)
#   sudo sh rn_tester.sh --restore        put back the plug-in that was installed before the first run of this script
#
# Only the plug-in (/Library/GPUBundles/RedNaviMTLDriver.bundle) is replaced; the kext is not touched. The plug-in that was there first is kept in
# /Users/Shared/rednavi-plugin-backup. Each process loads the plug-in when it first opens the card, so no reboot is needed; a process that is running already
# keeps the plug-in it loaded. Geekbench uploads its result to the Geekbench Browser (the free version always does); the link is in geekbench.txt.
set -u
cd "$(dirname "$0")" || exit 1
PKG="$(pwd)"
DEST=/Library/GPUBundles/RedNaviMTLDriver.bundle
BACKUP=/Users/Shared/rednavi-plugin-backup

do_install=1; do_gb=1
for opt in "$@"; do case "$opt" in
    --restore)
        [ "$(id -u)" = 0 ] || { echo "run it with sudo: sudo sh $0 --restore"; exit 1; }
        [ -d "$BACKUP/RedNaviMTLDriver.bundle" ] || { echo "no backup in $BACKUP"; exit 1; }
        rm -rf "$DEST" && cp -R "$BACKUP/RedNaviMTLDriver.bundle" "$DEST" && chown -R root:wheel "$DEST" && echo "restored the plug-in from $BACKUP"
        exit $? ;;
    --no-install) do_install=0 ;;
    --no-geekbench) do_gb=0 ;;
    *) echo "unknown option $opt"; exit 1 ;;
esac; done

# The account the tests and Geekbench run as: the one that ran sudo, or this one.
if [ "$(id -u)" = 0 ]; then
    U="${SUDO_USER:-}"
    [ -n "$U" ] && [ "$U" != root ] || { echo "run it with sudo from your own account (SUDO_USER is not set)"; exit 1; }
    as_user() { sudo -u "$U" "$@"; }
else
    [ $do_install = 0 ] || { echo "installing needs sudo: sudo sh $0 $*   (or sh $0 --no-install to use the plug-in that is installed)"; exit 1; }
    U="$(id -un)"
    as_user() { "$@"; }
fi
UID_U="$(id -u "$U")"
HOME_U="$(dscl . -read "/Users/$U" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
[ -d "$HOME_U" ] || HOME_U="/Users/$U"
STAMP="$(date +%Y%m%d-%H%M%S)"
WORK="/tmp/rednavi-report-$STAMP"
OUT="$HOME_U/rednavi-report-$STAMP.zip"
mkdir -p "$WORK"
MARK="$WORK/.start"; touch "$MARK"
say() { echo "== $*"; echo "== $*" >> "$WORK/steps.txt"; }

if [ $do_install = 1 ]; then
    [ -d "$PKG/RedNaviMTLDriver.bundle" ] || { echo "RedNaviMTLDriver.bundle is not next to this script"; exit 1; }
    if [ -d "$DEST" ] && [ ! -d "$BACKUP/RedNaviMTLDriver.bundle" ]; then
        mkdir -p "$BACKUP" && cp -R "$DEST" "$BACKUP/" && say "kept the plug-in that was installed in $BACKUP"
    fi
    xattr -cr "$PKG/RedNaviMTLDriver.bundle"
    rm -rf "$DEST" && cp -R "$PKG/RedNaviMTLDriver.bundle" "$DEST" && chown -R root:wheel "$DEST" && chmod -R 755 "$DEST" &&
        codesign -f -s - "$DEST" >/dev/null 2>&1 || { echo "installing the plug-in failed"; exit 1; }
    say "installed the plug-in from $PKG"
fi

# The plug-in's log of this account (in /private/var/tmp when /Users/Shared/rednavi-log exists); only what this run adds is collected.
LOG="/tmp/rednavi-metal-$UID_U.log"
[ -f /Users/Shared/rednavi-log ] && LOG="/private/var/tmp/rednavi-metal-$UID_U.log"
LOG_AT=0; [ -f "$LOG" ] && LOG_AT=$(wc -c < "$LOG" | tr -d ' ')

if [ -f "$PKG/run_all_tests.sh" ]; then
    say "running the Metal test suite (about a minute)"
    as_user sh "$PKG/run_all_tests.sh" > "$WORK/tests.txt" 2>&1
    echo "   $(grep -c 'all checks passed\|rn_vk_selftest -> 0\|0 of 256 values wrong' "$WORK/tests.txt") checks passed; failures:"
    grep -iE "fail|wrong|TIMEOUT|exit [1-9]" "$WORK/tests.txt" | grep -vE " 0 (of [0-9]+ )?(values|bytes|texels)? ?wrong|0 of 256 values wrong|error: none|error none" | head -20
else
    say "run_all_tests.sh is not in the package, tests skipped"
fi

if [ $do_gb = 1 ]; then
    GB_APP="$(ls -d /Applications/Geekbench*.app 2>/dev/null | sort | tail -1)"
    GB="$GB_APP/Contents/Resources/geekbench_avx2"
    [ -x "$GB" ] || GB="$(ls "$GB_APP"/Contents/Resources/geekbench_* 2>/dev/null | head -1)"
    if [ -n "$GB_APP" ] && [ -x "$GB" ]; then
        # RN_METAL_SRCLIB: shaders compiled from Metal source (Geekbench's) go through the plug-in. Newer plug-ins do this by default (except in WindowServer);
        # the variable is kept for older ones.
        DEV="$(as_user env RN_METAL_COMPUTE=1 "$GB" --gpu-list 2>/dev/null | awk '/^Metal/{m=1} /^OpenCL/{m=0} m && /RADV/ {print $4; exit}')"
        if [ -n "$DEV" ]; then
            say "running the Geekbench GPU benchmark (Metal, device $DEV); it takes a few minutes"
            as_user env RN_METAL_COMPUTE=1 RN_METAL_SRCLIB=1 "$GB" --gpu Metal --gpu-device-id "$DEV" > "$WORK/geekbench.txt" 2>&1
            rc=$?; echo "   geekbench exit status $rc"
            # a crash report is written some seconds after the process died
            if [ $rc -ge 128 ]; then for _ in 1 2 3 4 5 6 7 8 9 10; do
                find "$HOME_U/Library/Logs/DiagnosticReports" -newer "$MARK" -name "geekbench*" 2>/dev/null | grep -q . && break; sleep 2; done; fi
            grep -iE "Metal Score|browser.geekbench.com|error|fail" "$WORK/geekbench.txt" | grep -v "^objc\[" | head -10
        else
            say "Geekbench does not list a RADV Metal device; its list is in geekbench.txt"
            as_user env RN_METAL_COMPUTE=1 "$GB" --gpu-list > "$WORK/geekbench.txt" 2>&1
        fi
    else
        say "Geekbench is not in /Applications, skipped"
    fi
fi

say "collecting"
{
    echo "date: $(date)"; sw_vers; echo "model: $(sysctl -n hw.model)"
    echo "boot-args: $(sysctl -n kern.bootargs)"
    echo "plug-in: $(md5 -q "$DEST/Contents/MacOS/RedNaviMTLDriver" 2>/dev/null)"
    echo "kext: $(kmutil showloaded --list-only 2>/dev/null | grep -i rednavi)"
    ls -l /Users/Shared | grep rednavi
} > "$WORK/system.txt" 2>&1
ioreg -rn RedNaviProbe -l 2>/dev/null | grep -E '"(gfx-queue|gfx-test|ladder|mes-)' > "$WORK/ioreg_probe.txt"
ioreg -rc IOPCIDevice -l 2>/dev/null | grep rednavi-accel2-log | cut -c1-2000 > "$WORK/ioreg_accel2.txt"
if [ -f "$LOG" ]; then
    tail -c +$((LOG_AT + 1)) "$LOG" > "$WORK/plugin_log.txt"
    # how often each message came: the first thing to read
    sed -E 's/^[0-9.]+ pid [0-9]+ //' "$WORK/plugin_log.txt" | sort | uniq -c | sort -rn | head -150 > "$WORK/plugin_log_summary.txt"
    [ "$(wc -c < "$WORK/plugin_log.txt")" -gt 30000000 ] && { tail -c 30000000 "$WORK/plugin_log.txt" > "$WORK/l" && mv "$WORK/l" "$WORK/plugin_log.txt"; }
fi
find "$HOME_U/Library/Logs/DiagnosticReports" /Library/Logs/DiagnosticReports -newer "$MARK" -type f \( -name "*.ips" -o -name "*.crash" -o -name "*.panic" \) 2>/dev/null |
    while read -r f; do cp -X "$f" "$WORK/" 2>/dev/null; done
rm -f "$MARK"
(cd /tmp && ditto -c -k --keepParent "$(basename "$WORK")" "$OUT") && rm -rf "$WORK"
[ "$(id -u)" = 0 ] && chown "$U" "$OUT"
echo
echo "Done. Send this file: $OUT"
[ $do_install = 1 ] && echo "(to go back to the plug-in you had before: sudo sh $0 --restore)"
