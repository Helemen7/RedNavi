# Navi33 P5: physical Compute preflight

Development branch: `feature/navi33-p5-compute`.

**Status:** Linux/macOS compile and host-level tests are the first gates. A
physical RX 7600 Compute Dispatch is **not yet implemented**. Do not equate
successful PCI/IP Discovery or memory-register reads with GPU execution.

## What this stage now does on real hardware

On a PCI `1002:7480`, after validating the *actual* Navi33 discovery image:

1. Copy the actual **GC**, **SDMA0**, **MP0/MP1**, **MMHUB**, **OSSSYS**
   and **NBIF** segment offsets into the in-kernel `rn_device.reg_offset`.
   Reject missing/malformed segments rather than falling back to Navi32
   hard-coded addresses.
2. Read **MMHUB 3.0.2** FB_LOCATION_BASE, TOP and OFFSET through *checked
   BAR5 MMIO*. Combine base/top as 16 MiB location units, cross-check against
   the reported VRAM capacity and the PCI BAR0 CPU aperture size.
3. Read **MP0 PSP IFWI**, **PSP secure OS alive** and **MP1 SMU response**
   registers without sending firmware commands.
4. Report `navi33-ip-bases-ready`, `navi33-memory-preflight`,
   `navi33-memory-window-verified`, `navi33-firmware-preflight`,
   `navi33-fb-start` and `navi33-fb-end` in IORegistry.
5. Preserve existing Navi32 M6 initialization. For Navi32, resolve the
   GC/MP1 segment bases *after* copying IP Discovery, fixing their prior
   initialization order.

The MM_INDEX register pair is written while **reading** the Discovery
image, and `setMemoryEnable(true)` still modifies PCI configuration on
probe. P5 is not a side-effect-free diagnostic. It does **not** yet load
firmware, enable GART, reset GPU blocks or map a MES queue on Navi33.

## Verify

```sh
git fetch origin
git switch feature/navi33-p4-runtime
git switch -c feature/navi33-p5-compute  # only if not already created locally
git merge --ff-only origin/feature/navi33-p5-compute
make test
gh run list -R darrenintr/RedNavi --branch feature/navi33-p5-compute --limit 10
```

On a test macOS installation with a recoverable GPU, capture diagnostics:

```sh
ioreg -rn RedNaviProbe -l -x | grep -E 'navi33-|ip-discovery'
```

**Do not install the experimental Kext on a sole unrecoverable display
GPU.** Need a recovery path or PCIe passthrough for firmware/VM writes.
On Windows WSL2 these Mac IOKit properties cannot be collected.

## Physical Compute acceptance criteria still pending

1. PSP13.0.7 signed firmware and command acknowledgements; correct component
   order and reset ownership.
2. GMC11 page tables and GART with verified FB base, protected allocations,
   fences and VMID. In particular, do not use Navi32's fixed
   `RN_VRAM_MC_BASE` or its 16 GiB TMR/work-area assumptions.
3. MES Scheduler/KIQ firmware start, kernel MQD and doorbells, a confirmed
   `MAP_QUEUES` completion and physical HQD `MAPPED=1`.
4. One GPU shader writes a marker to VRAM, fence completes and CPU reads
   the value. Later validate a 65,536-element compute output.

The only pass condition for real Compute is the **GPU's own output and
completed fence**, not a synthetic register plan or a Kext build.
