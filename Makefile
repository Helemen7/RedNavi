# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Builds build/<Config>/RedNavi.kext. Needs macOS + Xcode command-line tools and the
# MacKernelSDK (scripts/bootstrap.sh fetches it).
ifeq ($(DEBUG),1)
CONFIG := Debug
OPT := -O0 -g -DDEBUG
else
CONFIG := Release
OPT := -Os
endif

SDK := MacKernelSDK
OUT := build/$(CONFIG)
# Every kext is signed at build time (ad hoc by default, no Team ID: an ad hoc signature is all the test machines need with SIP and AMFI relaxed), so nothing has to be signed
# again where it is installed. RN_SIGN_ID=<identity> signs with a certificate instead (an Apple Development one gives a Team ID, but changes nothing for loading here).
RN_SIGN_ID ?= -
# iCloud Drive puts Finder attributes back on files of this folder at any moment, and codesign refuses them: strip and sign again until it takes (a copy made with rsync -a has none).
SIGN = for i in 1 2 3 4 5; do xattr -cr $(1); codesign -f -s "$(RN_SIGN_ID)" --timestamp=none $(1) 2>/dev/null && break; sleep 1; done; codesign --verify $(1) || { echo "cannot sign $(1)"; exit 1; }
KEXT := $(OUT)/RedNavi.kext
FBKEXT := $(OUT)/RedNaviFB.kext
ACCELKEXT := $(OUT)/RedNaviAccel.kext
ACCEL2KEXT := $(OUT)/RedNaviAccel2.kext
CXX := clang++
ARCH := -arch x86_64
CXXFLAGS := $(ARCH) $(OPT) -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -mkernel \
	-nostdinc -isystem $(SDK)/Headers -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE \
	-DAPPLE -DNeXT -mmacosx-version-min=12.0 -Wall -Icompat/include
# Optional local firmware component (python3 tools/gen_psp_fw_header.py ... RedNavi/psp_fw.h).
ifneq ($(wildcard RedNavi/psp_fw.h RedNavi/dmub_fw.h),)
CXXFLAGS += -DRN_HAVE_PSP_FW
endif
ifneq ($(wildcard RedNavi/smu_fw.h),)
CXXFLAGS += -DRN_HAVE_SMU_FW
endif
ifneq ($(wildcard RedNavi/gc_fw.h),)
CXXFLAGS += -DRN_HAVE_GC_FW
endif
ifneq ($(wildcard RedNavi/dmub_fw.h),)
CXXFLAGS += -DRN_HAVE_DMUB_FW
endif
LDFLAGS := $(ARCH) -Xlinker -kext -nostdlib -lkmod -L$(SDK)/Library -lkmodc++ -lcc_kext \
	-Xlinker -object_path_lto -Xlinker /dev/null -mmacosx-version-min=12.0

all: $(KEXT) $(FBKEXT) $(ACCELKEXT) $(ACCEL2KEXT)

# Every header of the kext: gfx11.h, rn_uapi.h, the generated splash and register headers changed behind a stale object once (a new splash that never reached the build).
$(OUT)/kern_probe.o: RedNavi/kern_probe.cpp $(wildcard RedNavi/*.h compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/edid.o: RedNavi/edid.cpp RedNavi/edid.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/mode_regs.o: RedNavi/mode_regs.cpp RedNavi/mode_regs.h RedNavi/edid.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/gfx11.o: RedNavi/gfx11.cpp $(wildcard RedNavi/*.h compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/smu13.o: RedNavi/smu13.cpp RedNavi/smu13.h $(wildcard compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# LinuxKPI-style compat layer (see docs/COMPAT.md). Plain objects linked into RedNavi.kext.
COMPAT_SRC := $(wildcard compat/src/*.cpp)
COMPAT_OBJ := $(patsubst compat/src/%.cpp,$(OUT)/compat_%.o,$(COMPAT_SRC))

$(OUT)/compat_%.o: compat/src/%.cpp $(wildcard compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/kern_info.o: RedNavi/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(KEXT): $(OUT)/kern_probe.o $(OUT)/smu13.o $(OUT)/gfx11.o $(OUT)/edid.o $(OUT)/mode_regs.o $(COMPAT_OBJ) $(OUT)/kern_info.o RedNavi/Info.plist.in
	@mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/kern_probe.o $(OUT)/smu13.o $(OUT)/gfx11.o $(OUT)/edid.o $(OUT)/mode_regs.o $(COMPAT_OBJ) $(OUT)/kern_info.o -o $(KEXT)/Contents/MacOS/RedNavi
	@if nm -u $(KEXT)/Contents/MacOS/RedNavi | grep -E "RedNavi|rn_|kRN|^ *_g[A-Z]" | grep -vE "_g(IO|OS)"; then echo "unresolved RedNavi symbols in the kext"; exit 1; fi
	cp RedNavi/Info.plist.in $(KEXT)/Contents/Info.plist
	$(call SIGN,$(KEXT))
	@echo "built $(KEXT)"

# The framebuffer is its own kext: IOGraphicsFamily is not in the boot kernel collection OpenCore injects into.
$(OUT)/fb_kern_fb.o: RedNaviFB/kern_fb.cpp
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/fb_kern_info.o: RedNaviFB/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(FBKEXT): $(OUT)/fb_kern_fb.o $(OUT)/fb_kern_info.o RedNaviFB/Info.plist.in
	@mkdir -p $(FBKEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/fb_kern_fb.o $(OUT)/fb_kern_info.o -o $(FBKEXT)/Contents/MacOS/RedNaviFB
	cp RedNaviFB/Info.plist.in $(FBKEXT)/Contents/Info.plist
	$(call SIGN,$(FBKEXT))
	@echo "built $(FBKEXT)"

# The accelerator skeleton (M6): also an AuxKC kext, it depends on IOAcceleratorFamily2 (docs/M6_ACCELERATOR_STUDY.md).
$(OUT)/accel_kern_accel.o: RedNaviAccel/kern_accel.cpp
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/accel_kern_info.o: RedNaviAccel/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(ACCELKEXT): $(OUT)/accel_kern_accel.o $(OUT)/accel_kern_info.o RedNaviAccel/Info.plist.in
	@mkdir -p $(ACCELKEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/accel_kern_accel.o $(OUT)/accel_kern_info.o -o $(ACCELKEXT)/Contents/MacOS/RedNaviAccel
	cp RedNaviAccel/Info.plist.in $(ACCELKEXT)/Contents/Info.plist
	$(call SIGN,$(ACCELKEXT))
	@echo "built $(ACCELKEXT)"

# RedNaviAccel2 (M6): the same family dependency, but the family's own start() runs (docs/M6_ACCELERATOR_STUDY.md).
$(OUT)/accel2_kern_accel2.o: RedNaviAccel2/kern_accel2.cpp RedNaviAccel/IOGraphicsAccelerator2.h RedNaviAccel2/stubs.inc $(wildcard RedNaviAccel2/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/accel2_kern_info.o: RedNaviAccel2/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(ACCEL2KEXT): $(OUT)/accel2_kern_accel2.o $(OUT)/accel2_kern_info.o RedNaviAccel2/Info.plist.in
	@mkdir -p $(ACCEL2KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/accel2_kern_accel2.o $(OUT)/accel2_kern_info.o -o $(ACCEL2KEXT)/Contents/MacOS/RedNaviAccel2
	cp RedNaviAccel2/Info.plist.in $(ACCEL2KEXT)/Contents/Info.plist
	$(call SIGN,$(ACCEL2KEXT))
	@echo "built $(ACCEL2KEXT)"

# The Metal plug-in stub (user space, docs/M6_ACCELERATOR_STUDY.md) and the program that makes Metal load it.
MTLBUNDLE := $(OUT)/RedNaviMTLDriver.bundle
$(MTLBUNDLE): RedNaviMTL/plugin.m RedNaviMTL/compute.m RedNaviMTL/metallib.c RedNaviMTL/rn_air.c RedNaviMTL/rn_vk.c RedNaviMTL/rn_vk_selftest.c $(wildcard RedNaviMTL/*.h) RedNaviMTL/Info.plist
	@mkdir -p $(MTLBUNDLE)/Contents/MacOS
	clang -bundle -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -I/opt/local/include -framework Foundation -framework Metal -framework IOKit RedNaviMTL/plugin.m RedNaviMTL/compute.m RedNaviMTL/metallib.c RedNaviMTL/rn_air.c RedNaviMTL/rn_vk.c RedNaviMTL/rn_vk_selftest.c -framework IOSurface -framework ImageIO -framework CoreGraphics -o $(MTLBUNDLE)/Contents/MacOS/RedNaviMTLDriver
	cp RedNaviMTL/Info.plist $(MTLBUNDLE)/Contents/Info.plist
	xattr -cr $(MTLBUNDLE)
	$(call SIGN,$(MTLBUNDLE))
	@echo "built $(MTLBUNDLE)"

$(OUT)/mtl_probe: RedNaviMTL/mtl_probe.m
	@mkdir -p $(OUT)
	clang -arch x86_64 -mmacosx-version-min=12.0 -framework Metal -framework Foundation $< -o $@

mtl: $(MTLBUNDLE) $(OUT)/mtl_probe

clean:
	rm -rf build

# Host tests for the pure parts of the compat layer (they run on the Mac, not in the kernel).
test:
	@mkdir -p build
	clang++ -std=c++17 -Wall -DRN_HOST_TEST -Icompat/include tests/host/compat_test.cpp tests/host/host_hooks.cpp \
		compat/src/rn_slab.cpp compat/src/rn_firmware.cpp compat/src/rn_reg.cpp -o build/compat_test
	./build/compat_test
	clang++ -std=c++17 -Wall tests/host/edid_test.cpp RedNavi/edid.cpp -o build/edid_test
	./build/edid_test
	clang++ -std=c++17 -Wall tests/host/mode_regs_test.cpp RedNavi/mode_regs.cpp RedNavi/edid.cpp -o build/mode_regs_test
	./build/mode_regs_test

.PHONY: all clean test mtl
