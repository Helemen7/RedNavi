// The user client interface of RedNaviProbe, shared by the kext and user space programs (tools/rn_test.c, later the Mesa backend).
// Every method takes and returns one struct through IOConnectCallStructMethod; the sizes are checked exactly, so a layout change
// means a new RN_UAPI_VERSION. Plain C, fixed-width types only. The method set mirrors the DRM_AMDGPU_* commands RADV uses
// (docs/M5_PLAN.md), so that a backend for Mesa's ac_drm_* layer maps one to one.
#ifndef REDNAVI_RN_UAPI_H
#define REDNAVI_RN_UAPI_H

#include <stdint.h>

#define RN_UAPI_VERSION 3

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

struct rn_uc_submit {
    uint64_t ib_va;            // inside one mapping of this connection
    uint32_t ib_dwords;
    uint32_t flags;            // 0
};

struct rn_uc_submit_result {
    uint32_t seq;
    uint32_t pad;
};

struct rn_uc_wait_seq {
    uint32_t seq;              // a sequence number a submit returned (a later one is refused)
    uint32_t timeout_ms;       // 0: only look (a fence status query); at most 10000
};

struct rn_uc_wait_seq_result {
    uint32_t reached;
    uint32_t signaled;         // the fence value now: every sequence number up to it is done
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
