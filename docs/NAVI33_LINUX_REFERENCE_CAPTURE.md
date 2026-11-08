# Navi 33 native Linux reference capture (RX 7600)

This capture is a prerequisite for moving P5 from *synthetic host tests*
toward PSP/MES/Compute bring-up on macOS.

The validated Navi33 P5 Kext can check IP Discovery and read selected
MMHUB/PSP state, but no hardware test has yet confirmed that the actual RX
7600 returns the IP base counts or memory layout assumed in our fixtures.

Linux amdgpu exposes real device information, firmware status, fences and
some kernel-managed MQDs. This script **only reads documented observational
interfaces**. It deliberately avoids `amdgpu_test_ib`, `amdgpu_gpu_recover`,
`amdgpu_evict_vram`, `amdgpu_evict_gtt`, GPU benchmark and register access
endpoints because some debugfs reads/writes trigger GPU work or a reset.

## Requirements

- **Native Linux booted on the desktop with RX 7600**, e.g. an Ubuntu
  Live USB with "Try Ubuntu". WSL2 is unsupported (its kernel cannot
  expose the PCI device and real GPU PSP/MES registers in the same way).
- The AMD `amdgpu` kernel driver must bind to PCI `1002:7480`.
- Privileges to read `/sys/kernel/debug/dri`; typically run as root.
- `git`, `tar`, `timeout`, `head`, `find`, `sort`, `lspci` (optional).

## Run from native Linux

```bash
sudo apt update
sudo apt install -y git pciutils
git clone --branch feature/navi33-p5-compute --single-branch \
  https://github.com/darrenintr/RedNavi.git
cd RedNavi
mountpoint -q /sys/kernel/debug || sudo mount -t debugfs debugfs /sys/kernel/debug
sudo bash tools/capture_navi33_linux_reference.sh "$PWD/rx7600-linux-reference"
```

This creates `rx7600-linux-reference.tar.gz` next to the directory. Save
this archive **before ending a Live USB session**, then share it for a
side-by-side comparison with the P5 Runtime and Cobalt64 captures.

The archive contains PCI resource/identity details, IP Discovery
base-address text from sysfs, debugfs firmware and fence state,
raw (bounded) IP Discovery image, and up to 16 KiB of each
kernel-managed `amdgpu_mqd_*` endpoint exposed by the kernel.
Files not available on that kernel are safely omitted.

## How the evidence will be used

1. Compare Linux's actual IP base counts and versions with
   `navi33_bootstrap.h`, including GC/MMHUB/OSSSYS/NBIO.
2. Compare actual VRAM and BAR0 sizing and Linux GPU address translation
   with RedNavi's P5 `navi33-memory-preflight` logic.
3. Verify the versions of PSP, SMU, MES, MEC, RLC, SDMA firmware and
   determine whether the P1 pinned manifest matches the actual Linux build.
4. Check MQD and GPU ring/fence shape before attempting `MAP_QUEUES`
   and debugging the earlier HQD mapping failure.

A Linux MQD dump is a snapshot of the Linux driver's queue state, **not**
a ready-made macOS queue configuration. It can show expected fields,
structure and the type of hardware state to reproduce, but copying it
directly is not safe.

**The archive does not prove that RedNavi has executed GPU instructions.**
After PSP/GMC/MES are ported and verified on a recoverable native macOS
testbed, the acceptance test will be a GPU-generated result and fence.
