# Navi33 P6.2 — PSP VRAM staging / ring address preflight (host-only)

Branch: `feature/navi33-p6-hw-bringup`.

P6.1 has confirmed the exact **nine GC/SMU/MES/SDMA firmware files**
from Ubuntu `linux-firmware-amd-graphics`
`20260915.git1522c78a-0ubuntu1` (see
`docs/NAVI33_P6_FIRMWARE_PROVENANCE.md`).
That audit does not cover PSP secure OS/TA firmware images or any
actual macOS PSP ring handshake.

## Implemented in P6.2

`RedNavi/navi33_p6_psp_staging.h` validates an *illustrative* set of
four page-aligned buffer regions:

1. Firmware staging (large enough for the existing P1 **transfer plan**,
   at most 4 MiB).
2. A proposed PSP ring buffer (one 4 KiB page).
3. A command/response buffer (one 4 KiB page).
4. A fence buffer (one 4 KiB page).

It **rejects** a plan until the caller explicitly supplies all of:

- A successful prior P6 preflight.
- Externally verified firmware bytes.
- An explicit exclusive VRAM ownership assertion from a real allocator.
- A verified map of protected/reserved VRAM regions (at least one).
- The actual MMHUB FB base/usable FB length/CPU-visible intersection.

It rejects inconsistent/overflowing FB windows, non-page-aligned or
undersized buffers, VRAM outside the CPU aperture, reserved-region
overlap, and aliasing between firmware/ring/command/fence.

Offsets are relative to **validated GPU MC FB start** (from P5).
A synthesized GPU MC address is only an arithmetic proposal; the PSP
firmware may require additional address translation and other ring
structures that are not proved here.

The captured Linux RX 7600 example has 8192 MiB physical VRAM, 8176 MiB
usable FB and a 19 MiB reserved TMR. The tests use those numbers as
**non-production fixtures**, without allocating any physical memory.
The addresses in `tests/host/navi33_p6_staging_test.cpp` are not safe
buffer locations for real macOS use.

No caller in the macOS RedNavi kext uses this module yet. In particular,
`mayIssuePspCommands` and `kHardwareWritesEnabled` are always false.
A passing plan must **never** trigger PSP load/ring writes by itself.

## Run tests

```sh
cd ~/RedNavi
git switch feature/navi33-p6-hw-bringup
git pull --ff-only
make test
```

The dedicated GitHub Actions workflow runs P6.2 with GCC and Clang
AddressSanitizer/UndefinedBehaviorSanitizer, in addition to `make test`.

## Actual next physical bring-up gates (not implemented)

1. Verify PSP13.0.7 secure-OS/TA firmware images independently and
   determine whether the sOS is already alive at macOS start.
2. Implement a **real VRAM allocator/ownership protocol**. It must
   avoid PSP TMR, VBIOS, GART, scanout, preexisting queues, and every
   live allocation; a boolean assertion alone is not sufficient.
3. Port/validate the actual Navi33 PSP ring layout, mailbox
   handshake, command buffer, fence sequence, acknowledgements, and
   timeouts against upstream amdgpu.
4. Only attempt signed-firmware loading and command submission with
   an out-of-band recovery path, not on a sole unprotected display GPU.
5. Then validate GMC11/GART and MES queue on hardware before claiming
   successful GPU compute, and finally integrate RADV/Metal.

**Status:** P6.2 proves software range checking only, not functioning
GPU buffers, PSP execution, GART, MES, Metal or macOS graphics output.
