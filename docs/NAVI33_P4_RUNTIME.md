# Navi 33 P4 — real-device discovery gate for compute bring-up

Status: implemented in `feature/navi33-p4-runtime`; **does not yet submit
GPU commands**.

## Why this is on the path to GPU compute

On gfx1102 (RX 7600) the correct firmware, memory mappings, queue
descriptors and hardware IP register bases must be chosen from the actual
device before the macOS kernel driver can execute compute commands. Passing
a host-only planner does not prove the GPU accepted a single command.

The P4 Kext now recognizes PCI `1002:7480` and validates the full copied
IP Discovery table with `rn_navi33::inspectRuntimeDiscovery`:
- binary signature, size, binary checksum, table and die bounds;
- instance-0 GC 11.0.2, SDMA 6.0.2, PSP/SMU 13.0.7,
  DCN 3.2.1, MMHUB 3.0.2;
- a separate code path returning **before** all existing Navi 32 firmware
  loading, GART/SDMA, framebuffer and Metal/WindowServer initialization;
- `navi33-identity-verified`, `navi33-discovery-error`,
  `navi33-blocks-found`, `navi33-binary-header-major` and
  `navi33-ip-table-version` IORegistry properties.

The probe maps BAR5 and performs GPU VRAM-index reads to discover IP
information, so it is **not** a zero-side-effect probe. It does not attempt
MES / VM / PSP initialization on Navi 33 yet, and it refuses to assume 16 GiB
of VRAM if the Navi 33 memory-size register is invalid.

## Build/test

```sh
make test
clang++ -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi \
  tests/host/navi33_runtime_test.cpp -o build/navi33_runtime_test
./build/navi33_runtime_test
```

GitHub Actions builds Kexts on macOS and runs the host tests on Linux.

## What must be implemented to achieve physical RX 7600 compute

1. **PSP/SMU**: verified Navi 33-specific firmware acquisition, component
   order and PSP command responses; no copying Navi 32 signed firmware.
2. **GMC11 / GART**: real VRAM/FB address discovery, protected memory
   allocations, page tables and invalidation fences, not the Navi 32
   `RN_VRAM_MC_BASE` constant.
3. **MES / KIQ**: correctly allocated MQD and queues, verified
   `MAP_QUEUES` completion, doorbell and GFX HQD `MAPPED` read-back.
4. **Compute acceptance**: one GPU-written 32-bit marker and a fence,
   then a 65,536-value kernel with correct GPU read-back. CPU writes into
   scanout/VRAM do not count as physical GPU compute.
5. **RADV/Metal**: bind the gfx1102 backend and new UAPI after these lower
   hardware steps are verified.

With a sole display GPU, experiments that reset or reprogram its PSP/MES/DCN
can black-screen the host. Test writes on a separately recoverable
macOS+passthrough or native test system. WSL2 host tests cannot attest to
these operations.

**Do not claim Navi 33 Metal or compute acceleration from P4 alone.**
