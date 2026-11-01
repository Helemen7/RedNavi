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
KEXT := $(OUT)/RedNavi.kext
FBKEXT := $(OUT)/RedNaviFB.kext
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

all: $(KEXT) $(FBKEXT)

$(OUT)/kern_probe.o: RedNavi/kern_probe.cpp $(wildcard RedNavi/psp_fw.h RedNavi/dmub_fw.h RedNavi/smu_fw.h RedNavi/gc_fw.h RedNavi/smu13.h compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/edid.o: RedNavi/edid.cpp RedNavi/edid.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/mode_regs.o: RedNavi/mode_regs.cpp RedNavi/mode_regs.h RedNavi/edid.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/gfx11.o: RedNavi/gfx11.cpp RedNavi/gfx11.h RedNavi/gc_regs.h $(wildcard compat/include/*/*.h)
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
	@echo "built $(FBKEXT)"

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

.PHONY: all clean test
