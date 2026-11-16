// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// The table of ASICs the probe may write to, keyed by the versions in the card's IP discovery table.
// A card that matches no row stays read-only (IP discovery and VBIOS dumps only).
// How to add a row: docs/ADDING_A_GPU.md.
#ifndef REDNAVI_ASIC_H
#define REDNAVI_ASIC_H

#include <stdint.h>

// Hardware IDs of the IP discovery table (Linux include/soc15_hw_ip.h).
#define RN_HWID_MP1   1
#define RN_HWID_GC    11
#define RN_HWID_SDMA0 42
#define RN_HWID_MP0   255
#define RN_HWID_DMU   271

struct rn_ip_version {
    uint8_t major, minor, rev;
};

// What the Linux driver answered to DRM_AMDGPU_INFO on an ASIC (tools/linux_amdgpu_info_raw.py, tools/gen_linux_info_header.py).
// The user client answers RADV with it.
struct rn_drm_fw {
    uint32_t type, version, feature;
};
struct rn_drm_info {
    const uint8_t *dev_info;              // DEV_INFO
    uint32_t dev_info_size;
    const uint32_t *hw_ip_count;          // HW_IP_COUNT, one per IP type (10)
    const uint8_t (*hw_ip_info)[40];      // HW_IP_INFO of instance 0, one per IP type (10)
    const struct rn_drm_fw *fw;           // FW_VERSION
    uint32_t fw_count;
    const uint32_t *max_ibs;              // MAX_IBS, one per IP type
    uint32_t max_ibs_count;
};

// One register of the display replay (tools/gen_dcn_replay.py writes the tables).
struct RNReplayReg { unsigned char group; unsigned char baseIdx; unsigned short dword; unsigned int value; const char *name; };

// The connector the driver lights on a board: what the VBIOS display object table says about that path
// (docs/DC_PLAN.md, step 1) and the AUX engine its DDC line is on.
struct rn_dp_path {
    uint8_t aux;              // DCN AUX engine (0-based) the monitor answers on
    uint8_t phy_id;           // phyid of dig_transmitter_control_parameters_v1_6 (UNIPHY A = 0 ... E = 4)
    uint8_t hpd_sel;          // hpdsel of the same structure (HPD pin id)
    uint8_t connector_id;     // connector object id (19 is DisplayPort)
    uint8_t lanes;            // lanes wired to the connector
    uint8_t dig;              // DIG back end (and DP block) instance the connector's transmitter drives; the DIGn and DPn registers sit 0x100 dwords apart
};

// A DIG or DP register offset of instance `dig`, from the offset it has on instance 4 (the RX 7800 XT's, what the code was written with).
// dcn_3_2_0_offset.h places DIG0..DIG4 and DP0..DP4 at a stride of 0x100 dwords (DIG0_DIG_FE_CNTL 0x208b, DIG4_DIG_FE_CNTL 0x248b).
static inline uint32_t rn_dig_off(uint32_t dig4_offset, uint8_t dig) { return dig4_offset + (static_cast<int32_t>(dig) - 4) * 0x100; }

// One board of an ASIC that somebody has run the driver on.
struct rn_asic_board {
    uint16_t pci_device;      // PCI device ID (vendor 0x1002); 0 ends the list
    const char *model;        // name System Information shows
    uint32_t vram_mb;         // "VRAM,totalMB" property of the PCI node
    const struct RNReplayReg *replay;   // display registers of one lit pipe on this board, in programming order (dcnReplay)
    uint32_t replay_count;
    struct rn_dp_path dp;     // the DisplayPort path the replay table drives
    uint32_t psp_tmr_mb;      // VRAM offset of the PSP TMR in MB (rednavi_psp_tmr=2): "reserve ... from 0x8..." in the Linux log of this board; 0 = not known
    // 1: the register offsets, firmware and layout constants compiled into the kext are those of this board, and the write stages may run on it (verified on the board).
    // 0: the board is known (it is named in the "asic" property and its Linux data is kept in the row) and the probe stays read-only on it.
    uint8_t write_ok;
};

struct rn_asic {
    const char *name;                         // name of the die
    struct rn_ip_version gc, dcn, mp0, mp1, sdma;   // what the IP discovery table must say (GC, DMU, MP0, MP1, SDMA0)
    const struct rn_asic_board *boards;       // boards verified on hardware
    const struct rn_drm_info *drm_info;       // answers of the DRM_AMDGPU_INFO queries
    // What the code paths of this ASIC were built from. Names only: the firmware and the register headers are compiled in
    // and not selected through the row yet. Published in the "asic" property so a report shows which ones a build used.
    const char *firmware;                     // linux-firmware files (amdgpu/), separated by spaces
    const char *reg_headers;                  // generated register headers and the Linux headers they come from
    const char *dev_info;                     // the DRM_AMDGPU_INFO answers of the Linux driver on this ASIC
    const char *display_ref;                  // the Linux register capture the display replay is generated from
};

// Each ASIC translation unit exports one profile; the registry is kept in asic.cpp.

// Position of the entry of instance 0 of a hardware ID in an IP discovery table, 0 when there is none.
// *base64 says whether the base addresses after the entry are 64 bits wide. Every read is bounds-checked.
uint32_t rn_disc_find_ip(const uint8_t *table, uint32_t len, uint16_t hw_id, bool *base64);

// Version of instance 0 of a hardware ID. False when the table has no such block.
bool rn_disc_ip_version(const uint8_t *table, uint32_t len, uint16_t hw_id, struct rn_ip_version *out);

// The row whose five IP versions all equal the table's and that lists the PCI device ID, or NULL.
// Boards of one die can share a device ID (the RX 7800 XT and the RX 7700 XT are both 1002:747E): with vram_mb not 0, a board matches only when the memory size
// the card reports (RCC_CONFIG_MEMSIZE, 16 MB under the nominal size on the boards seen) is within 64 MB of the row's. vram_mb 0 takes the first board of the ID.
// *board, when not NULL, receives the matching board.
const struct rn_asic *rn_asic_lookup(const uint8_t *table, uint32_t len, uint16_t pci_device, const struct rn_asic_board **board, uint32_t vram_mb = 0);

// Select the GC register offsets (gc_regs.h, read through rn_gc) for a GC version: 11.0.3 has its own memory hub offsets; 11.0.0 to 11.0.2 use the gc_11_0_0 ones
// (Linux has no gc_11_0_2 headers). Returns 0 for a version without a table. Until it is called rn_gc points at the 11.0.3 table.
int rn_gc_select(struct rn_ip_version gc);

#endif
