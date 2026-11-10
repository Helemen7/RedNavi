#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Safety net for tests on the desktop: reads and changes the boot-args in the OpenCore config of the INTERNAL EFI over SSH, always with a backup, and can put it back.
# Runs on the machine you work from. It needs no sudo on the desktop (the EFI is writable by the user), so it also needs no password; what needs root (restarting WindowServer, rebooting,
# removing an installed bundle) is only printed by `root-help`.
#
#   scripts/rn_rollback.sh [-n] status                  boot-args that matter, the EFI that is used, running test programs, installed bundle, latest backups
#   scripts/rn_rollback.sh [-n] backup [TAG]            copy config.plist to config.plist.pre-TAG (default pre-rollback-<date>), never overwriting
#   scripts/rn_rollback.sh [-n] set ARG=VALUE ...       backup, then set (or add) those boot-args; the plist is checked with plutil -lint and read back
#   scripts/rn_rollback.sh [-n] unset NAME ...          backup, then remove boot-args (a name ending in * is a prefix: rednavi_accel2*)
#   scripts/rn_rollback.sh [-n] safe                    = set rednavi_accel2=2 (the accelerator answers but is not registered: Metal sees no device, WindowServer is untouched)
#   scripts/rn_rollback.sh [-n] off                     = unset rednavi_accel2* (the accelerator kext does nothing)
#   scripts/rn_rollback.sh [-n] restore [FILE]          put a backup back (default: the newest pre-rollback-*), after a backup of the current file
#   scripts/rn_rollback.sh processes                    stop the test programs (test_window, ca_renderer_test, mtl_init_harness) on the desktop
#   scripts/rn_rollback.sh root-help                    the commands that need the desktop's password
# -n: show what would change and change nothing. A boot-arg change takes effect at the next boot (OpenCore applies the config then); this script does not reboot.
# Environment: RN_HOST (default helemen7@mac-desktop.lab). The EFI is accepted only if it is internal and its config.plist has the rednavi boot-args (CLAUDE.md: never write to another EFI).
set -e
HOST=${RN_HOST:-helemen7@mac-desktop.lab}
DRY=0
if [ "$1" = "-n" ]; then DRY=1; shift; fi
MODE=${1:-status}
[ $# -gt 0 ] && shift

if [ "$MODE" = "root-help" ]; then
cat <<'EOF'
Needs the password of the desktop's user (run in its Terminal, or `ssh -t`):
  sudo killall -HUP WindowServer              restarts WindowServer (the session ends, the login window comes back; programs running in it are lost)
  sudo shutdown -r now                        reboots (the new boot-args apply)
  sudo rm -rf /Library/GPUBundles/RedNaviMTLDriver.bundle     removes the Metal plug-in (Metal then sees no RedNavi device; reboot or log out)
  sudo rm -rf /Library/Extensions/RedNaviAccel2.kext && sudo kmutil load -p /Library/Extensions   removes the accelerator kext from the next boot (check the path of the installed one first)
If the desktop does not answer at all: power-cycle it, boot the USB stick with the OpenCore EFI without the kexts, then run this script (restore or off) over SSH.
EOF
exit 0
fi

# The remote part. It receives MODE, DRY and the arguments; the data stay on the desktop.
ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST" sh -s -- "$DRY" "$MODE" "$@" <<'REMOTE'
DRY=$1; MODE=$2; shift 2
UUID=7C436110-AB2A-4BBB-A880-FE41995C9F82
KEY=":NVRAM:Add:$UUID:boot-args"
PB=/usr/libexec/PlistBuddy
fail() { echo "rn_rollback: $*" >&2; exit 2; }

# the internal EFI that holds the rednavi config
find_efi() {
    for v in /Volumes/EFI*; do
        [ -f "$v/EFI/OC/config.plist" ] || continue
        diskutil info "$v" 2>/dev/null | grep -q "Device Location: *Internal" || continue
        grep -q "rednavi_psp_load" "$v/EFI/OC/config.plist" || continue
        echo "$v"; return 0
    done
    return 1
}
EFI=$(find_efi) || fail "no mounted internal EFI with the rednavi boot-args under /Volumes (mount it: diskutil mount <internal EFI partition>)"
P=$EFI/EFI/OC/config.plist
args() { $PB -c "Print $KEY" "$P"; }
newest_backup() { ls -t "$EFI"/EFI/OC/config.plist.pre-rollback-* 2>/dev/null | head -1; }
stamp() { date +%Y%m%d-%H%M%S; }

do_backup() {   # $1 tag
    dst="$P.pre-$1"
    [ -e "$dst" ] && fail "$dst exists, not overwriting"
    if [ "$DRY" = 1 ]; then echo "would copy config.plist to $dst"; return 0; fi
    cp -p "$P" "$dst" && cmp -s "$P" "$dst" || fail "backup to $dst failed"
    echo "backup: $dst"
}

apply_args() {   # $1 new boot-args string
    if [ "$DRY" = 1 ]; then echo "would set boot-args to:"; echo "  $1"; return 0; fi
    $PB -c "Set $KEY '$1'" "$P" || fail "PlistBuddy could not set the boot-args"
    plutil -lint "$P" >/dev/null || fail "config.plist does not lint after the change: run restore"
    [ "$(args)" = "$1" ] || fail "read back differs: run restore"
    echo "boot-args now:"; echo "  $(args)"
    echo "takes effect at the next boot"
}

# remove the tokens that match $1 (name, or prefix when it ends in *)
without() {
    cur=$2
    for t in $cur; do
        case "$1" in
            *\*) pre=${1%\*}; case "$t" in "$pre"*) continue;; esac;;
            *) case "$t" in "$1"|"$1="*) continue;; esac;;
        esac
        printf '%s ' "$t"
    done
}

case "$MODE" in
status)
    echo "uptime:   $(uptime | sed 's/^ *//')"
    echo "EFI:      $EFI ($(diskutil info "$EFI" | awk -F: '/Device Node/ {gsub(/ /,"",$2); print $2}'), internal)"
    echo "config:   $(ls -l "$P" | awk '{print $5, $6, $7, $8}')"
    echo "boot-args in config.plist (accelerator, framebuffer and display ones):"
    args | tr ' ' '\n' | grep -E "accel2|rednavi_fb|boot_fb|noigpu|disable_ws|rednavi_mode" | sed 's/^/  /'
    echo "boot-args of the running boot (nvram): $(nvram boot-args 2>/dev/null | tr ' ' '\n' | grep -c rednavi_) rednavi args; accel2: $(nvram boot-args 2>/dev/null | tr ' ' '\n' | grep '^rednavi_accel2=' | head -1)"
    echo "test programs running: $(pgrep -l -f 'test_window|ca_renderer_test|mtl_init_harness' 2>/dev/null | tr '\n' ' ')"
    echo "installed bundle: $(md5 -q /Library/GPUBundles/RedNaviMTLDriver.bundle/Contents/MacOS/RedNaviMTLDriver 2>/dev/null || echo none)"
    echo "sudo without a password: $(sudo -n true 2>/dev/null && echo yes || echo no)"
    echo "latest backups:"; ls -t "$EFI"/EFI/OC/config.plist.pre-* 2>/dev/null | head -4 | sed 's/^/  /'
    ;;
backup)
    do_backup "${1:-rollback-$(stamp)}"
    ;;
set)
    [ $# -ge 1 ] || fail "set needs ARG=VALUE"
    cur=$(args); new=$cur
    for kv in "$@"; do
        case "$kv" in *=*) ;; *) fail "set wants NAME=VALUE, got $kv";; esac
        new="$(without "${kv%%=*}" "$new")$kv"
        new=$(echo "$new" | sed 's/ *$//')
    done
    do_backup "rollback-$(stamp)"
    apply_args "$(echo "$new" | sed 's/ *$//')"
    ;;
unset)
    [ $# -ge 1 ] || fail "unset needs a name"
    cur=$(args); new=$cur
    for n in "$@"; do new=$(without "$n" "$new"); done
    do_backup "rollback-$(stamp)"
    apply_args "$(echo "$new" | sed 's/ *$//')"
    ;;
safe)
    cur=$(args); new="$(without 'rednavi_accel2' "$cur")rednavi_accel2=2"
    do_backup "rollback-$(stamp)"
    apply_args "$new"
    ;;
off)
    cur=$(args); new=$(without 'rednavi_accel2*' "$cur")
    do_backup "rollback-$(stamp)"
    apply_args "$(echo "$new" | sed 's/ *$//')"
    ;;
restore)
    src=${1:-$(newest_backup)}
    [ -n "$src" ] || fail "no backup named config.plist.pre-rollback-* and none given"
    case "$src" in /*) ;; *) src="$EFI/EFI/OC/$src";; esac
    [ -f "$src" ] || fail "$src is not a file"
    plutil -lint "$src" >/dev/null || fail "$src does not lint, not restoring it"
    grep -q rednavi_psp_load "$src" || fail "$src has no rednavi boot-args: not the right file"
    do_backup "restore-$(stamp)" 2>&1 | sed 's/^/  /'
    if [ "$DRY" = 1 ]; then echo "would restore $src"; exit 0; fi
    cp -p "$src" "$P" && cmp -s "$src" "$P" || fail "restore failed: compare $src and $P"
    echo "restored from $src"
    echo "boot-args now:"; echo "  $(args)"
    ;;
processes)
    for n in test_window ca_renderer_test mtl_init_harness; do
        if pgrep -x "$n" >/dev/null 2>&1; then pkill -x "$n" && echo "stopped $n"; fi
    done
    echo "running now: $(pgrep -l -f 'test_window|ca_renderer_test|mtl_init_harness' 2>/dev/null | tr '\n' ' ')"
    ;;
*)
    fail "unknown command $MODE (status, backup, set, unset, safe, off, restore, processes, root-help)"
    ;;
esac
REMOTE
