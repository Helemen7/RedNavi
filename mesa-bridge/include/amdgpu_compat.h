/*
 * The libdrm_amdgpu types and the AMDGPU DRM structures the ac_drm_* functions of Mesa's src/amd/common/ac_linux_drm.h use, copied unchanged from libdrm's
 * amdgpu/amdgpu.h and Mesa's include/drm-uapi/amdgpu_drm.h (MIT licence, Copyright Advanced Micro Devices, Inc. and others, see NOTICE). Inside a Mesa tree these
 * come from libdrm and the uapi header; here they let the bridge be built and tested without either.
 */
#ifndef REDNAVI_AMDGPU_COMPAT_H
#define REDNAVI_AMDGPU_COMPAT_H

#include <stdint.h>

#ifndef __u8
#define __u8 uint8_t
#define __u16 uint16_t
#define __u32 uint32_t
#define __u64 uint64_t
#define __s32 int32_t
#define __s64 int64_t
#endif

typedef struct amdgpu_bo *amdgpu_bo_handle;
typedef struct amdgpu_va *amdgpu_va_handle;

#define AMDGPU_GEM_DOMAIN_CPU		0x1
#define AMDGPU_GEM_DOMAIN_GTT		0x2
#define AMDGPU_GEM_DOMAIN_VRAM		0x4
#define AMDGPU_GEM_DOMAIN_GDS		0x8
#define AMDGPU_GEM_DOMAIN_GWS		0x10
#define AMDGPU_GEM_DOMAIN_OA		0x20
#define AMDGPU_GEM_DOMAIN_DOORBELL	0x40
#define AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED	(1 << 0)
#define AMDGPU_GEM_CREATE_NO_CPU_ACCESS		(1 << 1)
#define AMDGPU_GEM_CREATE_CPU_GTT_USWC		(1 << 2)
#define AMDGPU_GEM_CREATE_VRAM_CLEARED		(1 << 3)
#define AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS	(1 << 5)
#define AMDGPU_GEM_CREATE_VM_ALWAYS_VALID	(1 << 6)
#define AMDGPU_GEM_CREATE_EXPLICIT_SYNC		(1 << 7)
#define AMDGPU_GEM_CREATE_CP_MQD_GFX9		(1 << 8)
#define AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE	(1 << 9)
#define AMDGPU_GEM_CREATE_ENCRYPTED		(1 << 10)
#define AMDGPU_GEM_CREATE_PREEMPTIBLE		(1 << 11)
#define AMDGPU_GEM_CREATE_DISCARDABLE		(1 << 12)
#define AMDGPU_GEM_CREATE_COHERENT		(1 << 13)
#define AMDGPU_GEM_CREATE_UNCACHED		(1 << 14)
#define AMDGPU_GEM_CREATE_EXT_COHERENT		(1 << 15)
#define AMDGPU_GEM_CREATE_GFX12_DCC		(1 << 16)
#define AMDGPU_CTX_OP_ALLOC_CTX	1
#define AMDGPU_CTX_OP_FREE_CTX	2
#define AMDGPU_CTX_OP_QUERY_STATE	3
#define AMDGPU_CTX_OP_QUERY_STATE2	4
#define AMDGPU_CTX_OP_GET_STABLE_PSTATE	5
#define AMDGPU_CTX_OP_SET_STABLE_PSTATE	6
#define AMDGPU_CTX_PRIORITY_UNSET       -2048
#define AMDGPU_CTX_PRIORITY_VERY_LOW    -1023
#define AMDGPU_CTX_PRIORITY_LOW         -512
#define AMDGPU_CTX_PRIORITY_NORMAL      0
#define AMDGPU_CTX_PRIORITY_HIGH        512
#define AMDGPU_CTX_PRIORITY_VERY_HIGH   1023
#define AMDGPU_CTX_STABLE_PSTATE_FLAGS_MASK  0xf
#define AMDGPU_CTX_STABLE_PSTATE_NONE  0
#define AMDGPU_CTX_STABLE_PSTATE_STANDARD  1
#define AMDGPU_CTX_STABLE_PSTATE_MIN_SCLK  2
#define AMDGPU_CTX_STABLE_PSTATE_MIN_MCLK  3
#define AMDGPU_CTX_STABLE_PSTATE_PEAK  4
#define AMDGPU_VM_OP_RESERVE_VMID	1
#define AMDGPU_VM_OP_UNRESERVE_VMID	2
#define AMDGPU_VA_OP_MAP			1
#define AMDGPU_VA_OP_UNMAP			2
#define AMDGPU_VA_OP_CLEAR			3
#define AMDGPU_VA_OP_REPLACE			4
#define AMDGPU_VM_DELAY_UPDATE		(1 << 0)
#define AMDGPU_VM_PAGE_READABLE		(1 << 1)
#define AMDGPU_VM_PAGE_WRITEABLE	(1 << 2)
#define AMDGPU_VM_PAGE_EXECUTABLE	(1 << 3)
#define AMDGPU_VM_PAGE_PRT		(1 << 4)
#define AMDGPU_VM_MTYPE_MASK		(0xf << 5)
#define AMDGPU_VM_MTYPE_DEFAULT		(0 << 5)
#define AMDGPU_VM_MTYPE_NC		(1 << 5)
#define AMDGPU_VM_MTYPE_WC		(2 << 5)
#define AMDGPU_VM_MTYPE_CC		(3 << 5)
#define AMDGPU_VM_MTYPE_UC		(4 << 5)
#define AMDGPU_VM_MTYPE_RW		(5 << 5)
#define AMDGPU_VM_PAGE_NOALLOC		(1 << 9)
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
#define AMDGPU_CHUNK_ID_IB		0x01
#define AMDGPU_CHUNK_ID_FENCE		0x02
#define AMDGPU_CHUNK_ID_DEPENDENCIES	0x03
#define AMDGPU_CHUNK_ID_SYNCOBJ_IN      0x04
#define AMDGPU_CHUNK_ID_SYNCOBJ_OUT     0x05
#define AMDGPU_CHUNK_ID_BO_HANDLES      0x06
#define AMDGPU_CHUNK_ID_SCHEDULED_DEPENDENCIES	0x07
#define AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT    0x08
#define AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL  0x09
#define AMDGPU_CHUNK_ID_CP_GFX_SHADOW   0x0a
#define AMDGPU_IB_FLAG_CE	(1<<0)
#define AMDGPU_IB_FLAG_PREAMBLE (1<<1)
#define AMDGPU_IB_FLAG_PREEMPT (1<<2)
#define AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE (1 << 3)
#define AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID (1 << 4)
#define AMDGPU_IB_FLAGS_SECURE  (1 << 5)
#define AMDGPU_IB_FLAG_EMIT_MEM_SYNC  (1 << 6)
#define AMDGPU_VMHUB_TYPE_MASK			0xff
#define AMDGPU_VMHUB_TYPE_SHIFT			0
#define AMDGPU_VMHUB_TYPE_GFX			0
#define AMDGPU_VMHUB_TYPE_MM0			1
#define AMDGPU_VMHUB_TYPE_MM1			2
#define AMDGPU_VMHUB_IDX_MASK			0xff00
#define AMDGPU_VMHUB_IDX_SHIFT			8
#define AMDGPU_TIMEOUT_INFINITE			0xffffffffffffffffull
#define AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE     (1 << 0)
#define AMDGPU_VA_RANGE_32_BIT		0x1
#define AMDGPU_VA_RANGE_HIGH		0x2
#define AMDGPU_VA_RANGE_REPLAYABLE	0x4

enum amdgpu_bo_handle_type {
	/** GEM flink name (needs DRM authentication, used by DRI2) */
	amdgpu_bo_handle_type_gem_flink_name = 0,

	/** KMS handle which is used by all driver ioctls */
	amdgpu_bo_handle_type_kms = 1,

	/** DMA-buf fd handle */
	amdgpu_bo_handle_type_dma_buf_fd = 2,

	/** Deprecated in favour of and same behaviour as
	 * amdgpu_bo_handle_type_kms, use that instead of this
	 */
	amdgpu_bo_handle_type_kms_noimport = 3,
};

enum amdgpu_gpu_va_range
{
	/** Allocate from "normal"/general range */
	amdgpu_gpu_va_range_general = 0
};

enum amdgpu_sw_info {
	amdgpu_sw_info_address32_hi = 0,
	/** Query the PRT control bit when the half VA range is reserved for
	 * PRT when the device is initialized. This depends on the GFX version.
	 * A return value of ~0 should be ignored.
	 */
	amdgpu_sw_info_address_prt_wa_control_bit = 1,
};

struct amdgpu_bo_alloc_request {
	/** Allocation request. It must be aligned correctly. */
	uint64_t alloc_size;

	/**
	 * It may be required to have some specific alignment requirements
	 * for physical back-up storage (e.g. for displayable surface).
	 * If 0 there is no special alignment requirement
	 */
	uint64_t phys_alignment;

	/**
	 * UMD should specify where to allocate memory and how it
	 * will be accessed by the CPU.
	 */
	uint32_t preferred_heap;

	/** Additional flags passed on allocation */
	uint64_t flags;
};

struct amdgpu_bo_metadata {
	/** Special flag associated with surface */
	uint64_t flags;

	/**
	 * ASIC-specific tiling information (also used by DCE).
	 * The encoding is defined by the AMDGPU_TILING_* definitions.
	 */
	uint64_t tiling_info;

	/** Size of metadata associated with the buffer, in bytes. */
	uint32_t size_metadata;

	/** UMD specific metadata. Opaque for kernel */
	uint32_t umd_metadata[64];
};

struct amdgpu_bo_info {
	/** Allocated memory size */
	uint64_t alloc_size;

	/**
	 * It may be required to have some specific alignment requirements
	 * for physical back-up storage.
	 */
	uint64_t phys_alignment;

	/** Heap where to allocate memory. */
	uint32_t preferred_heap;

	/** Additional allocation flags. */
	uint64_t alloc_flags;

	/** Metadata associated with buffer if any. */
	struct amdgpu_bo_metadata metadata;
};

struct amdgpu_heap_info {
	/** Theoretical max. available memory in the given heap */
	uint64_t heap_size;

	/**
	 * Number of bytes allocated in the heap. This includes all processes
	 * and private allocations in the kernel. It changes when new buffers
	 * are allocated, freed, and moved. It cannot be larger than
	 * heap_size.
	 */
	uint64_t heap_usage;

	/**
	 * Theoretical possible max. size of buffer which
	 * could be allocated in the given heap
	 */
	uint64_t max_allocation;
};

struct amdgpu_gpu_info {
	/** Asic id */
	uint32_t asic_id;
	/** Chip revision */
	uint32_t chip_rev;
	/** Chip external revision */
	uint32_t chip_external_rev;
	/** Family ID */
	uint32_t family_id;
	/** Special flags */
	uint64_t ids_flags;
	/** max engine clock*/
	uint64_t max_engine_clk;
	/** max memory clock */
	uint64_t max_memory_clk;
	/** number of shader engines */
	uint32_t num_shader_engines;
	/** number of shader arrays per engine */
	uint32_t num_shader_arrays_per_engine;
	/**  Number of available good shader pipes */
	uint32_t avail_quad_shader_pipes;
	/**  Max. number of shader pipes.(including good and bad pipes  */
	uint32_t max_quad_shader_pipes;
	/** Number of parameter cache entries per shader quad pipe */
	uint32_t cache_entries_per_quad_pipe;
	/**  Number of available graphics context */
	uint32_t num_hw_gfx_contexts;
	/** Number of render backend pipes */
	uint32_t rb_pipes;
	/**  Enabled render backend pipe mask */
	uint32_t enabled_rb_pipes_mask;
	/** Frequency of GPU Counter */
	uint32_t gpu_counter_freq;
	/** CC_RB_BACKEND_DISABLE.BACKEND_DISABLE per SE */
	uint32_t backend_disable[4];
	/** Value of MC_ARB_RAMCFG register*/
	uint32_t mc_arb_ramcfg;
	/** Value of GB_ADDR_CONFIG */
	uint32_t gb_addr_cfg;
	/** Values of the GB_TILE_MODE0..31 registers */
	uint32_t gb_tile_mode[32];
	/** Values of GB_MACROTILE_MODE0..15 registers */
	uint32_t gb_macro_tile_mode[16];
	/** Value of PA_SC_RASTER_CONFIG register per SE */
	uint32_t pa_sc_raster_cfg[4];
	/** Value of PA_SC_RASTER_CONFIG_1 register per SE */
	uint32_t pa_sc_raster_cfg1[4];
	/* CU info */
	uint32_t cu_active_number;
	uint32_t cu_ao_mask;
	uint32_t cu_bitmap[4][4];
	/* video memory type info*/
	uint32_t vram_type;
	/* video memory bit width*/
	uint32_t vram_bit_width;
	/** constant engine ram size*/
	uint32_t ce_ram_size;
	/* vce harvesting instance */
	uint32_t vce_harvest_config;
	/* PCI revision ID */
	uint32_t pci_rev_id;
};

struct drm_amdgpu_cs_chunk {
	__u32		chunk_id;
	__u32		length_dw;
	__u64		chunk_data;
};

struct drm_amdgpu_cs_chunk_ib {
	__u32 _pad;
	/** AMDGPU_IB_FLAG_* */
	__u32 flags;
	/** Virtual address to begin IB execution */
	__u64 va_start;
	/** Size of submission */
	__u32 ib_bytes;
	/** HW IP to submit to */
	__u32 ip_type;
	/** HW IP index of the same type to submit to  */
	__u32 ip_instance;
	/** Ring index to submit to */
	__u32 ring;
};

struct drm_amdgpu_cs_chunk_fence {
	__u32 handle;
	__u32 offset;
};

struct drm_amdgpu_cs_chunk_data {
	union {
		struct drm_amdgpu_cs_chunk_ib		ib_data;
		struct drm_amdgpu_cs_chunk_fence	fence_data;
	};
};

struct drm_amdgpu_cs_chunk_dep {
	__u32 ip_type;
	__u32 ip_instance;
	__u32 ring;
	__u32 ctx_id;
	__u64 handle;
};

struct drm_amdgpu_cs_chunk_sem {
	__u32 handle;
};

struct drm_amdgpu_cs_chunk_syncobj {
       __u32 handle;
       __u32 flags;
       __u64 point;
};

struct drm_amdgpu_info_uq_metadata_gfx {
	/* shadow area size for gfx11 */
	__u32 shadow_size;
	/* shadow area base virtual alignment for gfx11 */
	__u32 shadow_alignment;
	/* context save area size for gfx11 */
	__u32 csa_size;
	/* context save area base virtual alignment for gfx11 */
	__u32 csa_alignment;
};

struct drm_amdgpu_info_uq_metadata_compute {
	/* EOP size for gfx11 */
	__u32 eop_size;
	/* EOP base virtual alignment for gfx11 */
	__u32 eop_alignment;
};

struct drm_amdgpu_info_uq_metadata_sdma {
	/* context save area size for sdma6 */
	__u32 csa_size;
	/* context save area base virtual alignment for sdma6 */
	__u32 csa_alignment;
};

struct drm_amdgpu_info_uq_metadata {
	union {
		struct drm_amdgpu_info_uq_metadata_gfx gfx;
		struct drm_amdgpu_info_uq_metadata_compute compute;
		struct drm_amdgpu_info_uq_metadata_sdma sdma;
	};
};

#endif
