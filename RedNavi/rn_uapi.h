// The user client interface of RedNaviProbe, shared by the kext and user space programs (tools/rn_test.c, later the Mesa backend).
// Every method takes and returns one struct through IOConnectCallStructMethod; the sizes are checked exactly, so a layout change
// means a new RN_UAPI_VERSION. Plain C, fixed-width types only. The method set mirrors the DRM_AMDGPU_* commands RADV uses
// (docs/M5_PLAN.md), so that a backend for Mesa's ac_drm_* layer maps one to one.
#ifndef REDNAVI_RN_UAPI_H
#define REDNAVI_RN_UAPI_H

#include <stdint.h>

#include "rn_drm_uapi.h"

#define RN_UAPI_VERSION 5

enum {
    RN_UC_INFO = 0,        // out rn_uc_info
    RN_UC_BO_CREATE = 1,   // in rn_uc_bo_create, out rn_uc_bo_info
    RN_UC_BO_FREE = 2,     // in rn_uc_bo_free
    RN_UC_SUBMIT = 3,      // in rn_uc_submit, out rn_uc_submit_result
    RN_UC_BO_WAIT = 4,     // in rn_uc_bo_wait, out rn_uc_bo_wait_result
    RN_UC_DIAG = 5,        // in rn_uc_diag_req, out rn_uc_diag
    RN_UC_WAIT_SEQ = 6,    // in rn_uc_wait_seq, out rn_uc_wait_seq_result
    RN_UC_VA_MAP = 7,      // in rn_uc_va_map
    RN_UC_VA_UNMAP = 8,    // in rn_uc_va_unmap
    RN_UC_QUEUE_RESET = 9, // out rn_uc_reset_result
    RN_UC_INFO_QUERY = 10, // in struct drm_amdgpu_info (return_pointer unused), out: the answer, as many bytes as the caller asks for in return_size
    RN_UC_SYNCOBJ_CREATE = 11,   // in rn_uc_syncobj_create, out rn_uc_syncobj_create_result
    RN_UC_SYNCOBJ_DESTROY = 12,  // in rn_uc_syncobj_destroy
    RN_UC_SYNCOBJ_WAIT = 13,     // in rn_uc_syncobj_wait, out rn_uc_syncobj_wait_result
    RN_UC_SYNCOBJ_RESET = 14,    // in rn_uc_syncobj_array
    RN_UC_SYNCOBJ_SIGNAL = 15,   // in rn_uc_syncobj_array (points: 0 signals the binary state, a point signals the timeline up to it)
    RN_UC_SYNCOBJ_QUERY = 16,    // in rn_uc_syncobj_array, out rn_uc_syncobj_array (points filled)
    RN_UC_SYNCOBJ_TRANSFER = 17, // in rn_uc_syncobj_transfer
    RN_UC_COUNT
};

// A buffer is mapped into the caller with IOConnectMapMemory64(connection, handle, ...): the memory type is the handle.
//
// Every connection owns a GPU virtual address space (a VMID, 1 to 7, so at most 7 connections at a time): the lower half of the 48-bit space,
// four levels of page table as in Linux. Command buffers are addressed in it: a buffer is put there with RN_UC_VA_MAP at an address the caller picks
// from [vm_va_start, vm_va_start + vm_va_bytes) (the lowest 2 MB are kept free, as amdgpu does), and only what is mapped is reachable by the GPU
// for this connection's command buffers. Addresses with bit 47 set (the upper half, 0xffff800000000000 and up in canonical form) are refused: see docs/COMPAT.md. Mapping is read, write and execute. The gpu_va that RN_UC_BO_CREATE returns is the
// buffer's address in the kernel's own window and is not valid in a connection's address space.

struct rn_uc_info {
    uint32_t version;          // RN_UAPI_VERSION
    uint32_t doorbell_mapped;  // 1 when the compute queue can take submits
    uint64_t gart_base;        // GPU address of the window the buffers live in
    uint64_t gart_bytes;       // size of the window (addresses are never reused, see docs/COMPAT.md)
    uint64_t max_bo_bytes;
    uint32_t bo_count;         // buffers of this connection
    uint32_t bo_limit;         // buffers one connection can hold
    uint64_t vm_va_start;      // the lowest address that can be mapped; the space ends at vm_va_start + vm_va_bytes = 2^47
    uint64_t vm_va_bytes;
    uint32_t vmid;
    uint32_t va_map_limit;     // mappings one connection can hold
};

struct rn_uc_bo_create {
    uint64_t size;             // bytes, rounded up to 4 KB
    uint32_t flags;            // 0
    uint32_t pad;
};

struct rn_uc_bo_info {
    uint32_t handle;           // 1 and up, local to the connection
    uint32_t pad;
    uint64_t gpu_va;
    uint64_t size;
};

struct rn_uc_bo_free {
    uint32_t handle;
    uint32_t pad;
};

// Every connection has one context with one fence: each submit gets the next sequence number (1, 2, 3, ...) and the GPU writes it to a
// kernel-owned dword when the IB has finished, so a sequence number is done when the fence has reached it. Sequence numbers are 32 bits and
// compared modulo 2^32; a submit that is refused (ring full, bad address) does not use one.
struct rn_uc_va_map {
    uint32_t handle;
    uint32_t flags;            // 0
    uint64_t va;               // 4 KB aligned, inside the window, not overlapping another mapping
    uint64_t offset;           // into the buffer, 4 KB aligned
    uint64_t size;             // 4 KB aligned, offset + size within the buffer
};

struct rn_uc_va_unmap {
    uint64_t va;               // exactly the va and size of one mapping
    uint64_t size;
};

// Synchronisation objects, after DRM's (drm_syncobj_*), per connection: a handle names an object of this connection. One object has a binary state
// (point 0: signaled or not) and a timeline (points above 0, a 64-bit value that only grows). Work that signals an object does it when its fence is done:
// `rn_uc_sync_ref` is drm_amdgpu_cs_chunk_syncobj (handle, flags, point) and goes in a submit as a wait before the IB or a signal after it.
// Arrays are inline, up to RN_UC_SYNC_MAX entries, where DRM passes a pointer.
#define RN_UC_SYNC_MAX 16
#define RN_UC_SYNCOBJ_CREATE_SIGNALED 1u                 // DRM_SYNCOBJ_CREATE_SIGNALED
#define RN_UC_SYNCOBJ_WAIT_ALL 1u                        // DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL
#define RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT 2u                 // DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT: wait for work to be attached first
#define RN_UC_SYNCOBJ_WAIT_AVAILABLE 4u                  // DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE: work attached is enough, it need not be done

struct rn_uc_syncobj_create { uint32_t flags; uint32_t pad; };
struct rn_uc_syncobj_create_result { uint32_t handle; uint32_t pad; };
struct rn_uc_syncobj_destroy { uint32_t handle; uint32_t pad; };

struct rn_uc_syncobj_wait {
    int64_t timeout_nsec;      // relative, at most 10 s, 0 only looks
    uint32_t count;            // handles
    uint32_t flags;            // RN_UC_SYNCOBJ_WAIT_*
    uint32_t handles[RN_UC_SYNC_MAX];
    uint64_t points[RN_UC_SYNC_MAX];   // 0: the binary state
};
struct rn_uc_syncobj_wait_result { uint32_t first_signaled; uint32_t pad; };

struct rn_uc_syncobj_array {
    uint32_t count;
    uint32_t flags;
    uint32_t handles[RN_UC_SYNC_MAX];
    uint64_t points[RN_UC_SYNC_MAX];
};

struct rn_uc_syncobj_transfer {
    uint32_t src_handle, dst_handle;
    uint64_t src_point, dst_point;
    uint32_t flags, pad;
};

struct drm_amdgpu_cs_chunk_syncobj_ref {                  // drm_amdgpu_cs_chunk_syncobj, field for field
    uint32_t handle;
    uint32_t flags;
    uint64_t point;
};
typedef struct drm_amdgpu_cs_chunk_syncobj_ref rn_uc_sync_ref;

#define RN_UC_SUBMIT_SYNC_MAX 8
struct rn_uc_submit {
    uint64_t ib_va;            // inside one mapping of this connection
    uint32_t ib_dwords;
    uint32_t flags;            // 0
    uint32_t in_count;         // syncobjs to wait for before the IB runs (the CPU waits before the packets go to the ring, at most 10 s)
    uint32_t out_count;        // syncobjs the IB signals when it is done (at their point)
    rn_uc_sync_ref in[RN_UC_SUBMIT_SYNC_MAX];
    rn_uc_sync_ref out[RN_UC_SUBMIT_SYNC_MAX];
};

struct rn_uc_submit_result {
    uint32_t seq;
    uint32_t pad;
};

struct rn_uc_wait_seq {
    uint32_t seq;              // a sequence number a submit returned (a later one is refused)
    uint32_t timeout_ms;       // 0: only look (a fence status query); at most 10000
};

// A shader that hangs or faults can leave the compute queue stuck. When a wait for a sequence number ran for rednavi_hang_ms (boot-arg, default 8000) without
// the fence moving, the kernel resets the queue (as Linux resets a hung kernel compute queue through the MES): everything in the ring and running is
// dropped, the fences of all connections jump to their last submitted number so that nobody waits for good, and the numbers that did not complete are
// reported as lost to the connection that owned them (RN_UC_SEQ_LOST, from wait_seq). The queue is usable again afterwards. RN_UC_QUEUE_RESET asks for
// the same reset right away, for a client that has its own watchdog.
#define RN_UC_SEQ_DONE 0
#define RN_UC_SEQ_LOST 1
struct rn_uc_wait_seq_result {
    uint32_t reached;
    uint32_t signaled;         // the fence value now: every sequence number up to it is done
    uint32_t status;           // RN_UC_SEQ_DONE, or RN_UC_SEQ_LOST: reached because the queue was reset, the work did not (all) run
    uint32_t pad;
};

struct rn_uc_reset_result {
    uint32_t status;           // 0: the queue was reset and mapped again; otherwise the step that failed (see rn_compute_queue_reset in gfx11.cpp)
    uint32_t resets;           // resets since boot
    uint32_t lost_from;        // this connection's sequence numbers lost in the reset, from..to; none when lost_from > lost_to
    uint32_t lost_to;
};

struct rn_uc_bo_wait {
    uint32_t handle;
    uint32_t offset;           // byte offset of a dword
    uint32_t value;            // wait until the dword equals it
    uint32_t timeout_ms;       // at most 10000
};

struct rn_uc_bo_wait_result {
    uint32_t reached;
    uint32_t pad;
};

#define RN_UC_DIAG_CLEAR_FAULT 1u   // after reading, clear the latched GCVM_L2_PROTECTION_FAULT_STATUS (it keeps the first fault until then)
struct rn_uc_diag_req {
    uint32_t flags;
    uint32_t pad;
};

struct rn_uc_diag {
    uint32_t hqd_rptr, hqd_wptr, hqd_active, cp_stat;
    uint32_t fault_status, fault_addr_lo, fault_addr_hi, ring_wptr;
    uint32_t sdma0_version, grbm_status, rlc_gpm_stat;
    uint32_t hqd_doorbell_control, cp_pq_status, doorbell_range_lower, doorbell_range_upper, doorbell_aper_en;
    uint32_t pq_base, pq_control, mqd_base;
    uint32_t ring_rptr_word, ring_wptr_word, driver_wptr, ring_full_waits;
    uint32_t seq_submitted, seq_signaled;     // this connection's context
};

#endif
