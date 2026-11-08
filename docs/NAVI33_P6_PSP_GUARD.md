# RX 7600 / Navi33 P6.1 — PSP 13.0.7 readiness and firmware audit

Branch: `feature/navi33-p6-hw-bringup`.

## Scope and limitations

P6.1 introduces a **read-only decision gate** on the existing Navi33
P5 path in `RedNavi/kern_probe.cpp`. It does not enable a PSP loader,
change firmware, allocate GPU VRAM, create a PSP ring, or issue PSP
commands. `kHardwareWritesEnabled` remains `false`.

The underlying P5 probe itself already performs MM_INDEX discovery
reads and enables the PCI memory decode; **do not describe the entire
kext as side-effect-free**.

The gate consumes the real P5 discovery/FB preflight and two readable
MP0 mailbox registers; it records the result in the
`navi33-p6-psp-preflight` IORegistry dictionary.

### `status` values

| Value | Meaning |
|---:|---|
| 1 | IP Discovery was not verified |
| 2 | Required IP register bases are absent |
| 3 | MMHUB FB memory window is not verified |
| 4 | PSP mailbox could not be read via BAR5 |
| 5 | PSP mailbox returned an invalid all-ones value |
| 6 | IFWI ready bit 31 is clear |
| 7 | Secure OS version is nonzero; sOS appears to be running |
| 8 | Secure OS version is zero; signed, verified firmware would be needed |

`sos_alive=1` is **not proof** of a working PSP command ring.
`may_issue_psp_commands` is **always zero** in P6.1.
No stage advances automatically.

## Verify firmware *bytes*, not filenames

The P1 reference file `RedNavi/navi33_psp_plan.h` names nine gfx1102 /
SMU13 firmware files and pins their expected uncompressed sizes and
SHA-256 digests. Those historical hashes **have not been independently
validated from the 2026-11-08 Linux capture**. The capture listed file
names/sizes but did not include firmware bytes.

```sh
sudo apt update
sudo apt install -y zstd
python3 tools/navi33_p6_firmware_verify.py --firmware-dir /lib/firmware/amdgpu
```

If the WSL image does not contain AMD firmware, supply another local
directory containing genuine `*.bin`, `*.bin.zst` or `*.bin.xz`
images, or install Ubuntu's `linux-firmware` package if desired.
A nonzero exit means missing, mismatched, or unreadable files; **do not
bypass the hash check to load firmware**. A zero exit means only that
the files match the P1 reference; firmware revision compatibility,
allocation ownership and PSP command safety remain unproven.

The tool does not download or include firmware in Git commits.

## Tests

```sh
git switch feature/navi33-p6-hw-bringup
git pull --ff-only
make test
gh run list -R darrenintr/RedNavi --branch feature/navi33-p6-hw-bringup --limit 10
```

The `navi33-p6.yml` workflow compiles the gate with GCC and Clang under
ASan/UBSan and runs the Python verifier tests using synthetic fake
files. It does **not** validate physical RX 7600 hardware.

For a recoverable test macOS system only:

```sh
ioreg -rn RedNaviProbe -l -x | grep -E 'navi33-(p6|memory|firmware|identity|ip-bases)'
```

Never enable experimental firmware writes on a machine whose sole
working display is this GPU without a tested out-of-band recovery route.

## P6.2 follow-up

Before a PSP command can be added, implement an owned/reserved VRAM
staging allocator, verify all nine actual firmware bytes, handle
already-alive firmware without reloading, port PSP13.0.7 command/ring
handshakes and timeouts from Linux with Navi33-specific addresses,
and prove a physical acknowledge/fence on the test macOS machine.
Only then move on to GMC11/GART, MES/KIQ and a real shader-written
marker. Host-only green tests are not GPU compute acceleration.
