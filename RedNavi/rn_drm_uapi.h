/*
 * The AMDGPU DRM structures the kext's INFO query and its command-submission references speak, copied unchanged from Mesa's include/drm-uapi/amdgpu_drm.h
 * (Linux kernel uapi, Copyright 2012 Advanced Micro Devices, Inc., MIT licence, see NOTICE). Only what RADV asks for is here. The fixed-width types are
 * the C99 ones instead of the kernel's __u32 and __u64.
 */
#ifndef REDNAVI_RN_DRM_UAPI_H
#define REDNAVI_RN_DRM_UAPI_H

#include <stdint.h>

#ifndef __u8
#define __u8 uint8_t
#define __u16 uint16_t
#define __u32 uint32_t
#define __u64 uint64_t
#define __s32 int32_t
#define __s64 int64_t
#endif

#define AMDGPU_HW_IP_GFX          0
#define AMDGPU_HW_IP_COMPUTE      1
#define AMDGPU_HW_IP_DMA          2
#define AMDGPU_HW_IP_UVD          3
#define AMDGPU_HW_IP_VCE          4
#define AMDGPU_HW_IP_UVD_ENC      5
#define AMDGPU_HW_IP_VCN_DEC      6
#define AMDGPU_HW_IP_VCN_ENC      7
#define AMDGPU_HW_IP_VCN_JPEG     8
#define AMDGPU_HW_IP_VPE          9
#define AMDGPU_HW_IP_NUM          10
#define AMDGPU_HW_IP_INSTANCE_MAX_COUNT 1
#define AMDGPU_IDS_FLAGS_FUSION         0x1
#define AMDGPU_IDS_FLAGS_PREEMPTION     0x2
#define AMDGPU_IDS_FLAGS_TMZ            0x4
#define AMDGPU_IDS_FLAGS_CONFORMANT_TRUNC_COORD 0x8
#define AMDGPU_IDS_FLAGS_MODE_MASK      0x300
#define AMDGPU_IDS_FLAGS_MODE_SHIFT     0x8
#define AMDGPU_IDS_FLAGS_MODE_PF        0x0
#define AMDGPU_IDS_FLAGS_MODE_VF        0x1
#define AMDGPU_IDS_FLAGS_MODE_PT        0x2
#define AMDGPU_INFO_ACCEL_WORKING		0x00
#define AMDGPU_INFO_CRTC_FROM_ID		0x01
#define AMDGPU_INFO_HW_IP_INFO			0x02
#define AMDGPU_INFO_HW_IP_COUNT			0x03
#define AMDGPU_INFO_TIMESTAMP			0x05
#define AMDGPU_INFO_FW_VERSION			0x0e
/* AMDGPU_INFO_FW_*, the fw_type of a FW_VERSION query (amdgpu_drm.h indents these inside a comment block, so the extraction above missed them; values as in the
 * header and checked against the types that answered in tools/linux_amdgpu_info_raw.py's run: ME 4, PFP 5, RLC 7, MEC 8, SMC 0xa, SDMA 0xb, ...). */
#define AMDGPU_INFO_FW_GFX_ME		0x04
#define AMDGPU_INFO_FW_GFX_PFP		0x05
#define AMDGPU_INFO_FW_GFX_RLC		0x07
#define AMDGPU_INFO_FW_GFX_MEC		0x08
#define AMDGPU_INFO_FW_SMC		0x0a
#define AMDGPU_INFO_FW_SDMA		0x0b
#define AMDGPU_INFO_FW_MES_KIQ		0x19
#define AMDGPU_INFO_FW_MES		0x1a
#define AMDGPU_INFO_FW_IMU		0x1b
#define AMDGPU_INFO_NUM_BYTES_MOVED		0x0f
#define AMDGPU_INFO_VRAM_USAGE			0x10
#define AMDGPU_INFO_GTT_USAGE			0x11
#define AMDGPU_INFO_GDS_CONFIG			0x13
#define AMDGPU_INFO_VRAM_GTT			0x14
#define AMDGPU_INFO_READ_MMR_REG		0x15
#define AMDGPU_INFO_DEV_INFO			0x16
#define AMDGPU_INFO_VIS_VRAM_USAGE		0x17
#define AMDGPU_INFO_NUM_EVICTIONS		0x18
#define AMDGPU_INFO_MEMORY			0x19
#define AMDGPU_INFO_VCE_CLOCK_TABLE		0x1A
#define AMDGPU_INFO_VBIOS			0x1B
#define AMDGPU_INFO_NUM_HANDLES			0x1C
#define AMDGPU_INFO_SENSOR			0x1D
#define AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS	0x1E
#define AMDGPU_INFO_VRAM_LOST_COUNTER		0x1F
#define AMDGPU_INFO_RAS_ENABLED_FEATURES	0x20
#define AMDGPU_INFO_RAS_ENABLED_UMC			(1 << 0)
#define AMDGPU_INFO_RAS_ENABLED_SDMA			(1 << 1)
#define AMDGPU_INFO_RAS_ENABLED_GFX			(1 << 2)
#define AMDGPU_INFO_RAS_ENABLED_MMHUB			(1 << 3)
#define AMDGPU_INFO_RAS_ENABLED_ATHUB			(1 << 4)
#define AMDGPU_INFO_RAS_ENABLED_PCIE			(1 << 5)
#define AMDGPU_INFO_RAS_ENABLED_HDP			(1 << 6)
#define AMDGPU_INFO_RAS_ENABLED_XGMI			(1 << 7)
#define AMDGPU_INFO_RAS_ENABLED_DF			(1 << 8)
#define AMDGPU_INFO_RAS_ENABLED_SMN			(1 << 9)
#define AMDGPU_INFO_RAS_ENABLED_SEM			(1 << 10)
#define AMDGPU_INFO_RAS_ENABLED_MP0			(1 << 11)
#define AMDGPU_INFO_RAS_ENABLED_MP1			(1 << 12)
#define AMDGPU_INFO_RAS_ENABLED_FUSE			(1 << 13)
#define AMDGPU_INFO_VIDEO_CAPS			0x21
#define AMDGPU_INFO_MAX_IBS			0x22
#define AMDGPU_INFO_GPUVM_FAULT			0x23
#define AMDGPU_INFO_UQ_FW_AREAS			0x24
#define AMDGPU_INFO_MMR_SE_INDEX_SHIFT	0
#define AMDGPU_INFO_MMR_SE_INDEX_MASK	0xff
#define AMDGPU_INFO_MMR_SH_INDEX_SHIFT	8
#define AMDGPU_INFO_MMR_SH_INDEX_MASK	0xff
#define AMDGPU_VRAM_TYPE_UNKNOWN 0
#define AMDGPU_VRAM_TYPE_GDDR1 1
#define AMDGPU_VRAM_TYPE_DDR2  2
#define AMDGPU_VRAM_TYPE_GDDR3 3
#define AMDGPU_VRAM_TYPE_GDDR4 4
#define AMDGPU_VRAM_TYPE_GDDR5 5
#define AMDGPU_VRAM_TYPE_HBM   6
#define AMDGPU_VRAM_TYPE_DDR3  7
#define AMDGPU_VRAM_TYPE_DDR4  8
#define AMDGPU_VRAM_TYPE_GDDR6 9
#define AMDGPU_VRAM_TYPE_DDR5  10
#define AMDGPU_VRAM_TYPE_LPDDR4 11
#define AMDGPU_VRAM_TYPE_LPDDR5 12
#define AMDGPU_VRAM_TYPE_HBM3E 13
#define AMDGPU_VRAM_TYPE_HBM4 14
#define AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG2			0
#define AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4			1
#define AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_VC1			2
#define AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_MPEG4_AVC		3
#define AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_HEVC			4
#define AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_JPEG			5
#define AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_VP9			6
#define AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_AV1			7
#define AMDGPU_INFO_VIDEO_CAPS_CODEC_IDX_COUNT			8
#define AMDGPU_FAMILY_UNKNOWN			0
#define AMDGPU_FAMILY_SI			110 /* Hainan, Oland, Verde, Pitcairn, Tahiti */
#define AMDGPU_FAMILY_CI			120 /* Bonaire, Hawaii */
#define AMDGPU_FAMILY_KV			125 /* Kaveri, Kabini, Mullins */
#define AMDGPU_FAMILY_VI			130 /* Iceland, Tonga */
#define AMDGPU_FAMILY_CZ			135 /* Carrizo, Stoney */
#define AMDGPU_FAMILY_AI			141 /* Vega10 */
#define AMDGPU_FAMILY_RV			142 /* Raven */
#define AMDGPU_FAMILY_NV			143 /* Navi10 */
#define AMDGPU_FAMILY_VGH			144 /* Van Gogh */
#define AMDGPU_FAMILY_GC_11_0_0			145 /* GC 11.0.0 */
#define AMDGPU_FAMILY_YC			146 /* Yellow Carp */
#define AMDGPU_FAMILY_GC_11_0_1			148 /* GC 11.0.1 */
#define AMDGPU_FAMILY_GC_10_3_6			149 /* GC 10.3.6 */
#define AMDGPU_FAMILY_GC_10_3_7			151 /* GC 10.3.7 */
#define AMDGPU_FAMILY_GC_11_5_0			150 /* GC 11.5.0 */
#define AMDGPU_FAMILY_GC_12_0_0			152 /* GC 12.0.0 */

struct drm_amdgpu_query_fw {
	/** AMDGPU_INFO_FW_* */
	__u32 fw_type;
	/**
	 * Index of the IP if there are more IPs of
	 * the same type.
	 */
	__u32 ip_instance;
	/**
	 * Index of the engine. Whether this is used depends
	 * on the firmware type. (e.g. MEC, SDMA)
	 */
	__u32 index;
	__u32 _pad;
};

struct drm_amdgpu_info {
	/* Where the return value will be stored */
	__u64 return_pointer;
	/* The size of the return value. Just like "size" in "snprintf",
	 * it limits how many bytes the kernel can write. */
	__u32 return_size;
	/* The query request id. */
	__u32 query;

	union {
		struct {
			__u32 id;
			__u32 _pad;
		} mode_crtc;

		struct {
			/** AMDGPU_HW_IP_* */
			__u32 type;
			/**
			 * Index of the IP if there are more IPs of the same
			 * type. Ignored by AMDGPU_INFO_HW_IP_COUNT.
			 */
			__u32 ip_instance;
		} query_hw_ip;

		struct {
			__u32 dword_offset;
			/** number of registers to read */
			__u32 count;
			__u32 instance;
			/** For future use, no flags defined so far */
			__u32 flags;
		} read_mmr_reg;

		struct drm_amdgpu_query_fw query_fw;

		struct {
			__u32 type;
			__u32 offset;
		} vbios_info;

		struct {
			__u32 type;
		} sensor_info;

		struct {
			__u32 type;
		} video_cap;
	};
};

struct drm_amdgpu_info_hw_ip {
	/** Version of h/w IP */
	__u32  hw_ip_version_major;
	__u32  hw_ip_version_minor;
	/** Capabilities */
	__u64  capabilities_flags;
	/** command buffer address start alignment*/
	__u32  ib_start_alignment;
	/** command buffer size alignment*/
	__u32  ib_size_alignment;
	/** Bitmask of available rings. Bit 0 means ring 0, etc. */
	__u32  available_rings;
	/** version info: bits 23:16 major, 15:8 minor, 7:0 revision */
	__u32  ip_discovery_version;
};

struct drm_amdgpu_info_firmware {
	__u32 ver;
	__u32 feature;
};

struct drm_amdgpu_heap_info {
	/** max. physical memory */
	__u64 total_heap_size;

	/** Theoretical max. available memory in the given heap */
	__u64 usable_heap_size;

	/**
	 * Number of bytes allocated in the heap. This includes all processes
	 * and private allocations in the kernel. It changes when new buffers
	 * are allocated, freed, and moved. It cannot be larger than
	 * heap_size.
	 */
	__u64 heap_usage;

	/**
	 * Theoretical possible max. size of buffer which
	 * could be allocated in the given heap
	 */
	__u64 max_allocation;
};

struct drm_amdgpu_memory_info {
	struct drm_amdgpu_heap_info vram;
	struct drm_amdgpu_heap_info cpu_accessible_vram;
	struct drm_amdgpu_heap_info gtt;
};

struct drm_amdgpu_info_vram_gtt {
	__u64 vram_size;
	__u64 vram_cpu_accessible_size;
	__u64 gtt_size;
};

struct drm_amdgpu_info_device {
	/** PCI Device ID */
	__u32 device_id;
	/** Internal chip revision: A0, A1, etc.) */
	__u32 chip_rev;
	__u32 external_rev;
	/** Revision id in PCI Config space */
	__u32 pci_rev;
	__u32 family;
	__u32 num_shader_engines;
	__u32 num_shader_arrays_per_engine;
	/* in KHz */
	__u32 gpu_counter_freq;
	__u64 max_engine_clock;
	__u64 max_memory_clock;
	/* cu information */
	__u32 cu_active_number;
	/* NOTE: cu_ao_mask is INVALID, DON'T use it */
	__u32 cu_ao_mask;
	__u32 cu_bitmap[4][4];
	/** Render backend pipe mask. One render backend is CB+DB. */
	__u32 enabled_rb_pipes_mask;
	__u32 num_rb_pipes;
	__u32 num_hw_gfx_contexts;
	/* PCIe version (the smaller of the GPU and the CPU/motherboard) */
	__u32 pcie_gen;
	__u64 ids_flags;
	/** Starting virtual address for UMDs. */
	__u64 virtual_address_offset;
	/** The maximum virtual address */
	__u64 virtual_address_max;
	/** Required alignment of virtual addresses. */
	__u32 virtual_address_alignment;
	/** Page table entry - fragment size */
	__u32 pte_fragment_size;
	__u32 gart_page_size;
	/** constant engine ram size*/
	__u32 ce_ram_size;
	/** video memory type info*/
	__u32 vram_type;
	/** video memory bit width*/
	__u32 vram_bit_width;
	/* vce harvesting instance */
	__u32 vce_harvest_config;
	/* gfx double offchip LDS buffers */
	__u32 gc_double_offchip_lds_buf;
	/* NGG Primitive Buffer */
	__u64 prim_buf_gpu_addr;
	/* NGG Position Buffer */
	__u64 pos_buf_gpu_addr;
	/* NGG Control Sideband */
	__u64 cntl_sb_buf_gpu_addr;
	/* NGG Parameter Cache */
	__u64 param_buf_gpu_addr;
	__u32 prim_buf_size;
	__u32 pos_buf_size;
	__u32 cntl_sb_buf_size;
	__u32 param_buf_size;
	/* wavefront size*/
	__u32 wave_front_size;
	/* shader visible vgprs*/
	__u32 num_shader_visible_vgprs;
	/* CU per shader array*/
	__u32 num_cu_per_sh;
	/* number of tcc blocks*/
	__u32 num_tcc_blocks;
	/* gs vgt table depth*/
	__u32 gs_vgt_table_depth;
	/* gs primitive buffer depth*/
	__u32 gs_prim_buffer_depth;
	/* max gs wavefront per vgt*/
	__u32 max_gs_waves_per_vgt;
	/* PCIe number of lanes (the smaller of the GPU and the CPU/motherboard) */
	__u32 pcie_num_lanes;
	/* always on cu bitmap */
	__u32 cu_ao_bitmap[4][4];
	/** Starting high virtual address for UMDs. */
	__u64 high_va_offset;
	/** The maximum high virtual address */
	__u64 high_va_max;
	/* gfx10 pa_sc_tile_steering_override */
	__u32 pa_sc_tile_steering_override;
	/* disabled TCCs */
	__u64 tcc_disabled_mask;
	__u64 min_engine_clock;
	__u64 min_memory_clock;
	/* The following fields are only set on gfx11+, older chips set 0. */
	__u32 tcp_cache_size;       /* AKA GL0, VMEM cache */
	__u32 num_sqc_per_wgp;
	__u32 sqc_data_cache_size;  /* AKA SMEM cache */
	__u32 sqc_inst_cache_size;
	__u32 gl1c_cache_size;
	__u32 gl2c_cache_size;
	__u64 mall_size;            /* AKA infinity cache */
	/* high 32 bits of the rb pipes mask */
	__u32 enabled_rb_pipes_mask_hi;
	/* shadow area size for gfx11 */
	__u32 shadow_size;
	/* shadow area base virtual alignment for gfx11 */
	__u32 shadow_alignment;
	/* context save area size for gfx11 */
	__u32 csa_size;
	/* context save area base virtual alignment for gfx11 */
	__u32 csa_alignment;
	/* Userq IP mask (1 << AMDGPU_HW_IP_*) */
	__u32 userq_ip_mask;
	__u32 pad;
};

#endif
