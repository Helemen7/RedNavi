#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

"""Turn the Linux register dump of a lit pipe into the replay table the probe applies for a test pattern.

Reads docs/logs/linux-reference/dcn_pipe_linux.txt (tools/linux_dcn_pipe_dump.py output while amdgpu showed a
picture on path 0) and a curated, ordered list of the configuration registers worth copying, in the order DC
programs them (dcn20_enable_stream_timing, stream encoder setup, then the pattern generator). Writes
RedNavi/dcn_replay.h. Status, counter and CRC registers are deliberately not copied.

    python3 tools/gen_dcn_replay.py docs/logs/linux-reference/dcn_pipe_linux.txt docs/logs/linux-reference/dcn_plane_linux_linear.txt

A second board gets its own table: --name gives the symbol a suffix and the file its name, for example
--name navi33_7480 writes kRNReplay_navi33_7480 into RedNavi/dcn_replay_navi33_7480.h. The register list below
is the one of pipe 0 on DIG4 / DP4; a board on another DIG needs its own list.
"""
import sys

# (group, register name, optional override value)
SPEC = [
    (1, "DP_DTO0_PHASE", None), (1, "DP_DTO0_MODULO", None), (1, "OTG_PIXEL_RATE_DIV", None),
    (1, "OTG0_PIXEL_RATE_CNTL", None), (1, "OTG0_PHYPLL_PIXEL_RATE_CNTL", None), (1, "SYMCLKE_CLOCK_ENABLE", None),
    (2, "ODM0_OPTC_INPUT_CLOCK_CONTROL", None), (2, "OTG0_OTG_CLOCK_CONTROL", None), (2, "OPTC_CLOCK_CONTROL", None),
    (3, "OTG0_OTG_H_TOTAL", None), (3, "OTG0_OTG_H_BLANK_START_END", None), (3, "OTG0_OTG_H_SYNC_A", None),
    (3, "OTG0_OTG_H_SYNC_A_CNTL", None), (3, "OTG0_OTG_H_TIMING_CNTL", None), (3, "OTG0_OTG_V_TOTAL", None),
    (3, "OTG0_OTG_V_TOTAL_MIN", None), (3, "OTG0_OTG_V_TOTAL_MAX", None), (3, "OTG0_OTG_V_TOTAL_CONTROL", None),
    (3, "OTG0_OTG_V_BLANK_START_END", None), (3, "OTG0_OTG_V_SYNC_A", None), (3, "OTG0_OTG_V_SYNC_A_CNTL", None),
    (3, "OTG0_OTG_VSTARTUP_PARAM", None), (3, "OTG0_OTG_VUPDATE_PARAM", None), (3, "OTG0_OTG_VREADY_PARAM", None),
    (3, "ODM0_OPTC_DATA_SOURCE_SELECT", None), (3, "ODM0_OPTC_DATA_FORMAT_CONTROL", None),
    (4, "OPP_PIPE0_OPP_PIPE_CONTROL", None), (4, "FMT0_FMT_CONTROL", None), (4, "FMT0_FMT_BIT_DEPTH_CONTROL", None),
    (4, "FMT0_FMT_CLAMP_CNTL", None), (4, "FMT0_FMT_DYNAMIC_EXP_CNTL", None), (4, "DPG0_DPG_DIMENSIONS", None),
    (5, "MPC_OUT0_MUX", None), (5, "MPC_OUT0_DENORM_CONTROL", None), (5, "MPC_OUT0_DENORM_CLAMP_G_Y", None),
    (5, "MPC_OUT0_DENORM_CLAMP_B_CB", None), (5, "MPC_OUT0_CSC_MODE", None),
    (6, "DIG4_DIG_FE_CNTL", None), (6, "DIG4_DIG_BE_CNTL", None), (6, "DIG4_DIG_FIFO_CTRL0", None),
    (6, "DIG4_DIG_FIFO_CTRL1", None), (6, "DP4_DP_PIXEL_FORMAT", None), (6, "DP4_DP_MSA_COLORIMETRY", None),
    (6, "DP4_DP_MSA_MISC", None), (6, "DP4_DP_VID_TIMING", None), (6, "DP4_DP_VID_N", None), (6, "DP4_DP_VID_M", None),
    (6, "DP4_DP_MSA_TIMING_PARAM1", None), (6, "DP4_DP_MSA_TIMING_PARAM2", None), (6, "DP4_DP_MSA_TIMING_PARAM3", None),
    (6, "DP4_DP_MSA_TIMING_PARAM4", None), (6, "DP4_DP_VID_MSA_VBID", None), (6, "DP4_DP_LINK_FRAMING_CNTL", None),
    (6, "DP4_DP_STEER_FIFO", None),
    (7, "OTG0_OTG_CONTROL", 0x00001201),                       # master enable, without the read-only status bit
    (8, "DPG0_DPG_CONTROL", 0x00661001),                       # DPG_EN, colour squares RGB, 8 bpc, VRES/HRES 6
    (9, "DP4_DP_VID_STREAM_CNTL", 0x00000001),                 # DP_VID_STREAM_ENABLE
    # Plane (linear XRGB8888, values from the Linux text-console capture, dcn_plane_linux_linear.txt), applied while the pattern still shows.
    (10, "DCN_VM_FB_LOCATION_BASE", None), (10, "DCN_VM_FB_LOCATION_TOP", None), (10, "DCN_VM_FB_OFFSET", None),
    (10, "DCHUBBUB_SDPIF_CFG0", None), (10, "DCHUBBUB_SDPIF_CFG1", None), (10, "DCHUBBUB_COMPBUF_CTRL", None),
    (10, "DCHUBBUB_DET0_CTRL", None), (10, "DCHUBBUB_ARB_DF_REQ_OUTSTAND", None), (10, "DCHUBBUB_ARB_SAT_LEVEL", None),
] + [(10, f"DCHUBBUB_ARB_{n}_{w}", None) for w in "ABCD" for n in (
    "DATA_URGENCY_WATERMARK", "USR_RETRAINING_WATERMARK", "REFCYC_PER_TRIP_TO_MEMORY", "ALLOW_SR_ENTER_WATERMARK",
    "ALLOW_SR_EXIT_WATERMARK", "UCLK_PSTATE_CHANGE_WATERMARK", "FCLK_PSTATE_CHANGE_WATERMARK", "FRAC_URG_BW_NOM", "FRAC_URG_BW_FLIP")] + [
    (11, "DPPCLK0_DTO_PARAM", None), (11, "DPPCLK_DTO_CTRL", None), (11, "HUBP0_HUBP_CLK_CNTL", None), (11, "DPP_TOP0_DPP_CONTROL", None),
    (11, "CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT", None), (11, "CNVC_CFG0_FORMAT_CONTROL", None),
    (11, "CNVC_CFG0_FCNV_FP_SCALE_R", None), (11, "CNVC_CFG0_FCNV_FP_SCALE_G", None), (11, "CNVC_CFG0_FCNV_FP_SCALE_B", None),
    (11, "CNVC_CFG0_PRE_CSC_MODE", None), (11, "CNVC_CFG0_CNVC_COEF_FORMAT", None),
    (11, "DSCL0_SCL_MODE", None), (11, "DSCL0_SCL_TAP_CONTROL", None), (11, "DSCL0_DSCL_CONTROL", None), (11, "DSCL0_DSCL_2TAP_CONTROL", None),
    (11, "DSCL0_SCL_HORZ_FILTER_SCALE_RATIO", None), (11, "DSCL0_SCL_HORZ_FILTER_INIT", None), (11, "DSCL0_SCL_VERT_FILTER_SCALE_RATIO", None),
    (11, "DSCL0_SCL_VERT_FILTER_INIT", None), (11, "DSCL0_SCL_VERT_FILTER_INIT_BOT", None), (11, "DSCL0_SCL_BLACK_COLOR", None),
    (11, "DSCL0_RECOUT_START", None), (11, "DSCL0_RECOUT_SIZE", None), (11, "DSCL0_MPC_SIZE", None), (11, "DSCL0_LB_DATA_FORMAT", None),
    (11, "DSCL0_LB_MEMORY_CTRL", None), (11, "DSCL0_OBUF_CONTROL", None),
    (11, "CM0_CM_CONTROL", None), (11, "CM0_CM_POST_CSC_CONTROL", None), (11, "CM0_CM_GAMUT_REMAP_CONTROL", None),
    (11, "CM0_CM_GAMCOR_CONTROL", None), (11, "CM0_CM_HDR_MULT_COEF", None),
    (12, "MPCC0_MPCC_TOP_SEL", None), (12, "MPCC0_MPCC_BOT_SEL", None), (12, "MPCC0_MPCC_OPP_ID", None), (12, "MPCC0_MPCC_CONTROL", None),
    (12, "MPCC0_MPCC_SM_CONTROL", None), (12, "MPCC0_MPCC_UPDATE_LOCK_SEL", None), (12, "MPCC0_MPCC_TOP_GAIN", None),
    (12, "MPCC0_MPCC_BOT_GAIN_INSIDE", None), (12, "MPCC0_MPCC_BOT_GAIN_OUTSIDE", None), (12, "MPCC0_MPCC_MOVABLE_CM_LOCATION_CONTROL", None),
    (12, "MPCC0_MPCC_BG_R_CR", None), (12, "MPCC0_MPCC_BG_G_Y", None), (12, "MPCC0_MPCC_BG_B_CB", None),
] + [(13, n, None) for n in (
    "HUBP0_DCSURF_SURFACE_CONFIG", "HUBP0_DCSURF_ADDR_CONFIG", "HUBP0_DCSURF_TILING_CONFIG",
    "HUBP0_DCSURF_PRI_VIEWPORT_START", "HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION", "HUBP0_DCSURF_PRI_VIEWPORT_START_C", "HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_C",
    "HUBP0_DCSURF_SEC_VIEWPORT_START", "HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION", "HUBP0_DCSURF_SEC_VIEWPORT_START_C", "HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION_C",
    "HUBP0_DCHUBP_REQ_SIZE_CONFIG", "HUBP0_DCHUBP_REQ_SIZE_CONFIG_C", "HUBP0_DCHUBP_CNTL", "HUBP0_DCHUBP_VMPG_CONFIG", "HUBP0_DCHUBP_MALL_CONFIG",
    "HUBPREQ0_DCSURF_SURFACE_PITCH", "HUBPREQ0_DCSURF_SURFACE_PITCH_C", "HUBPREQ0_VMID_SETTINGS_0",
    "HUBPREQ0_DCSURF_SURFACE_CONTROL", "HUBPREQ0_DCSURF_FLIP_CONTROL", "HUBPREQ0_DCSURF_FLIP_CONTROL2",
    "HUBPREQ0_DCN_EXPANSION_MODE", "HUBPREQ0_DCN_TTU_QOS_WM", "HUBPREQ0_DCN_GLOBAL_TTU_CNTL",
    "HUBPREQ0_DCN_SURF0_TTU_CNTL0", "HUBPREQ0_DCN_SURF0_TTU_CNTL1", "HUBPREQ0_DCN_SURF1_TTU_CNTL0", "HUBPREQ0_DCN_SURF1_TTU_CNTL1",
    "HUBPREQ0_DCN_CUR0_TTU_CNTL0", "HUBPREQ0_DCN_CUR0_TTU_CNTL1", "HUBPREQ0_DCN_DMDATA_VM_CNTL",
    "HUBPREQ0_BLANK_OFFSET_0", "HUBPREQ0_BLANK_OFFSET_1", "HUBPREQ0_DST_DIMENSIONS", "HUBPREQ0_DST_AFTER_SCALER",
    "HUBPREQ0_PREFETCH_SETTINGS", "HUBPREQ0_PREFETCH_SETTINGS_C",
    "HUBPREQ0_VBLANK_PARAMETERS_0", "HUBPREQ0_VBLANK_PARAMETERS_1", "HUBPREQ0_VBLANK_PARAMETERS_2", "HUBPREQ0_VBLANK_PARAMETERS_3", "HUBPREQ0_VBLANK_PARAMETERS_4",
    "HUBPREQ0_FLIP_PARAMETERS_0", "HUBPREQ0_FLIP_PARAMETERS_1", "HUBPREQ0_FLIP_PARAMETERS_2",
    "HUBPREQ0_NOM_PARAMETERS_0", "HUBPREQ0_NOM_PARAMETERS_1", "HUBPREQ0_NOM_PARAMETERS_2", "HUBPREQ0_NOM_PARAMETERS_3",
    "HUBPREQ0_NOM_PARAMETERS_4", "HUBPREQ0_NOM_PARAMETERS_5", "HUBPREQ0_NOM_PARAMETERS_6", "HUBPREQ0_NOM_PARAMETERS_7",
    "HUBPREQ0_PER_LINE_DELIVERY_PRE", "HUBPREQ0_PER_LINE_DELIVERY", "HUBPREQ0_CURSOR_SETTINGS",
    "HUBPREQ0_REF_FREQ_TO_PIX_FREQ", "HUBPREQ0_DST_Y_DELTA_DRQ_LIMIT")] + [
    # the surface address goes last; its value is replaced by the probe's own scanout buffer
    (13, "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0x00000080),
    (13, "HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS", 0x08000000),
    (14, "DPG0_DPG_CONTROL", 0x00000000),                      # pattern generator off: the plane shows
]


def main():
    args = sys.argv[1:]
    suffix = ""
    if "--name" in args:
        i = args.index("--name")
        if i + 1 >= len(args) or not args[i + 1].replace("_", "").isalnum():
            sys.exit(__doc__)
        suffix = "_" + args[i + 1]
        del args[i:i + 2]
    if len(args) not in (1, 2):
        sys.exit(__doc__)
    vals = {}
    for path in args:                   # later files override earlier ones
        for line in open(path):
            if line.startswith("#") or not line.strip():
                continue
            name, idx, off, val = line.split()
            vals[name] = (int(idx), int(off, 16), int(val, 16))
    with open(f"RedNavi/dcn_replay{suffix}.h", "w") as f:
        f.write(f"// Generated by tools/gen_dcn_replay.py from {args[0]}.\n")
        f.write("// Registers of one lit DCN 3.2 pipe (OTG0 -> DIG4/DP4, 1080p60) as Linux left them, in programming order.\n")
        f.write('#include "asic.h"\n')
        f.write(f"static const RNReplayReg kRNReplay{suffix}[] = {{\n")
        for g, name, ov in SPEC:
            idx, off, v = vals[name]
            f.write(f'    {{{g}, {idx}, 0x{off:x}, 0x{(v if ov is None else ov):08x}u, "{name}"}},\n')
        f.write("};\n")
    print(len(SPEC), "registers")


if __name__ == "__main__":
    main()
