#!/bin/bash
# Run on Linux (the live USB) on the machine with the RX 7800 XT, as root, to collect what the kext's INFO queries have to answer the way the Linux kernel does
# (docs/M5_PLAN.md, "Not answered yet"): the numbers RADV gets from DRM_AMDGPU_INFO DEV_INFO and FW_VERSION, and what it derives from them.
#
#     sudo tools/linux_radv_info.sh > radv_info.txt 2>&1        (needs vulkan-tools and Mesa's RADV for the first part)
#
# Raw output goes in docs/logs/linux-reference/, minus the serial numbers (there are none in these files, but look before committing).
echo "== kernel"; uname -r
echo "== card"; for c in /sys/class/drm/card[0-9]; do [ -r $c/device/vendor ] && echo "$c vendor $(cat $c/device/vendor) device $(cat $c/device/device) revision $(cat $c/device/revision) subsys $(cat $c/device/subsystem_vendor)/$(cat $c/device/subsystem_device)"; done
echo "== RADV's own dump of what the kernel told it (ac_print_gpu_info)"
if command -v vulkaninfo >/dev/null; then RADV_DEBUG=info vulkaninfo --summary 2>&1 | head -400; else echo "vulkaninfo not installed: sudo apt install vulkan-tools mesa-vulkan-drivers"; fi
echo "== firmware versions as the kernel reports them (the FW_VERSION query)"
for d in /sys/kernel/debug/dri/[0-9]*; do [ -r $d/amdgpu_firmware_info ] && { echo "$d"; cat $d/amdgpu_firmware_info; }; done
echo "== gfx configuration the kernel read from the hardware (amdgpu_gca_config: struct of u32, version first)"
for d in /sys/kernel/debug/dri/[0-9]*; do [ -r $d/amdgpu_gca_config ] && { echo "$d"; od -An -tx4 -v $d/amdgpu_gca_config; }; done
echo "== vm and memory lines of the kernel log"
dmesg | grep -iE "vm size|fragment size|VRAM:|GART:|visible vram|gtt|amdgpu.*(ip block|gfx|mes)" | head -60
echo "== clocks"
for c in /sys/class/drm/card[0-9]/device; do [ -r $c/pp_dpm_sclk ] && { echo "$c sclk"; cat $c/pp_dpm_sclk; echo "mclk"; cat $c/pp_dpm_mclk; }; done
