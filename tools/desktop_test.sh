#!/bin/bash
# Deploy the built kext and rn_test to the development desktop, reboot it, and run rn_test modes over ssh once the kext has published the MES result.
#
#     tools/desktop_test.sh [mode ...]          modes: plain ring seq info syncobj bo vm vmhole high vmfault (default: all of them, vmfault last because it leaves compute stuck until a reset)
#
# Needs `make DEBUG=1` done, rn_test built for x86_64 (clang -O1 -arch x86_64 -mmacosx-version-min=12 -framework IOKit -framework CoreFoundation tools/rn_test.c -o rn_test_x86),
# ssh keys for helemen7@mac-desktop.lab and a sudoers rule that lets that user run `reboot`. Private to this setup (docs/TEST_MACHINES.md), kept here so that it is not lost.
W="$(cd "$(dirname "$0")/.." && pwd)"
D=helemen7@mac-desktop.lab
T="${RN_TEST_BIN:-$W/rn_test_x86}"
modes=("$@"); [ ${#modes[@]} -eq 0 ] && modes=(plain ring seq info syncobj bo vm vmhole high plain vmfault plain)
# The disk numbers move when USB sticks are in (the internal EFI was disk0s1, with sticks plugged in it is disk3s1): identify the EFI by what it is, an internal disk whose
# OpenCore config carries the RedNavi boot-args, not by its number.
loc=$(ssh -o BatchMode=yes $D "diskutil info /Volumes/EFI | grep 'Device Location' | awk '{print \$3}'")
has=$(ssh -o BatchMode=yes $D "grep -c rednavi_psp_load /Volumes/EFI/EFI/OC/config.plist")
[ "$loc" = "Internal" ] && [ "${has:-0}" -ge 1 ] || { echo "ABORT: /Volumes/EFI is not the internal OpenCore EFI (location '$loc', rednavi args '$has')"; exit 1; }
nm -u "$W/build/Debug/RedNavi.kext/Contents/MacOS/RedNavi" | grep -E "^ *_g[A-Z]" | grep -vE "_g(IO|OS)" && { echo "ABORT: unresolved own symbols"; exit 1; }
rsync -a --delete "$W/build/Debug/RedNavi.kext/" $D:/Volumes/EFI/EFI/OC/Kexts/RedNavi.kext/ || exit 1
scp -q "$T" $D:rn/rn_test_ring || exit 1
echo "deployed kext $(shasum -a 256 "$W/build/Debug/RedNavi.kext/Contents/MacOS/RedNavi" | cut -c1-16)"
ssh -o BatchMode=yes $D 'sync; sudo -n reboot' >/dev/null 2>&1
sleep 60
until ssh -o ConnectTimeout=4 -o BatchMode=yes $D true 2>/dev/null; do sleep 5; done
t0=$(date +%s)
until ssh -o ConnectTimeout=4 -o BatchMode=yes $D 'ioreg -rn RedNaviProbe -l -x | grep -q "mes-start"' 2>/dev/null; do sleep 3; [ $(( $(date +%s) - t0 )) -gt 420 ] && { echo TIMEOUT; break; }; done
ssh -o BatchMode=yes $D 'chmod +x ~/rn/rn_test_ring'
for m in "${modes[@]}"; do
    arg=$m; [ "$m" = plain ] && arg=
    echo "---$m"
    ssh -o BatchMode=yes -o ConnectTimeout=60 $D "~/rn/rn_test_ring $arg 2>&1 | tail -40"
done
echo DONE
