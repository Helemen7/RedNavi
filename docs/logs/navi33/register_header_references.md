# Navi 33 Linux register-header reference matrix

**Source tag:** CachyOS Linux `cachyos-7.3.1-1`
**Commit:** `36ae4c219cfbd02576048c7ca1d366cea5db8034`
**Target:** RX 7600 / Navi 33, IP Discovery versions from the captured 39-record inventory.

This is a source-reference map, not authorization to reuse or write these registers. The kernel header families below are the families referenced by the matching Linux source or already used by RedNavi's generated read/write code. ASIC-specific semantics and write-stage safety still require review.

## Candidate source header families

| Functional block | RX 7600 IP Discovery | Matching Linux reference family | Source evidence |
|---|---:|---|---|
| GC / GFX | GC 11.0.2 | `gc_11_0_0_offset.h`, `gc_11_0_0_sh_mask.h`, `gc_11_0_0_default.h` | `gfx_v11_0.c` includes these generic GC 11.0.0 headers |
| SDMA | SDMA 6.0.2 | Shared GC 11.0.0 headers, HDP 6.0.0 headers, `sdma_v6_0_0_pkt_open.h`, NBIO v4.3 | `sdma_v6_0.c` includes these; no separate `sdma_6_0_2_offset.h` / `sh_mask.h` was found in the matching `asic_reg/sdma` directory |
| HDP | HDP 6.0.1 | HDP 6.0.0 reference family | Confirm exact register use before reusing |
| NBIO | NBIO 4.3.1 | NBIO 4.3.0 offset / sh-mask family; `nbio_v4_3.h` | Generic v4.3 driver family |
| MMHUB | MMHUB 3.0.2 | MMHUB 3.0.0 offset / sh-mask family | Existing RedNavi generated header provenance |
| OSSSYS | OSSSYS 6.0.2 | OSSSYS 6.0.0 offset / sh-mask family | Existing RedNavi generated header provenance |
| DCN / DMU | DMU 3.2.1 | DCN 3.2.0 offset / sh-mask family | Existing RedNavi display register-list provenance; validate exact DMU/ASIC differences |
| VCN | VCN IP Discovery 4.0.4 | VCN 4.0.0 offset / sh-mask family | Generic VCN 4.0 implementation and register headers |

## Git blob provenance for key register headers

All blobs below are from the source tag stated above.

| Header | Git blob SHA |
|---|---|
| `gc/gc_11_0_0_offset.h` | `a3bcdf632066c7d45e968992351fa0c7c0285598` |
| `gc/gc_11_0_0_sh_mask.h` | `3088a4a13cb50b918dd190711ec2a84532a28200` |
| `gc/gc_11_0_0_default.h` | `a86d38f2d03df132647928bb45da4252cb4970b3` |
| `hdp/hdp_6_0_0_offset.h` | `b2dc63d28ba83ae2917dbab793e50850569789a9` |
| `hdp/hdp_6_0_0_sh_mask.h` | `e1905a114cacdad030427ec073690ffbbc35356a` |
| `nbio/nbio_4_3_0_offset.h` | `4b489d64deaaecc1e32e08d0de4193bd3fdedd45` |
| `nbio/nbio_4_3_0_sh_mask.h` | `d038fd915351ccb94832b9aef5f1f824b6e6145d` |
| `mmhub/mmhub_3_0_0_offset.h` | `88be857ad3efc03c000a8e8fd3884f90ff1fde5f` |
| `mmhub/mmhub_3_0_0_sh_mask.h` | `b354346ec62e55b3ae48a5b54ab3d3a458a87546` |
| `oss/osssys_6_0_0_offset.h` | `969e006b859bb1832825d076827ae582501f93d0` |
| `oss/osssys_6_0_0_sh_mask.h` | `a672a91e58f04802a396714472f9f79b80baa12b` |
| `dcn/dcn_3_2_0_offset.h` | `bc4524f9b462a3f726115e1139f1731607c7aafd` |
| `dcn/dcn_3_2_0_sh_mask.h` | `a89fe19c4c8425af8fdff92d810b00c9804b320e` |
| `vcn/vcn_4_0_0_offset.h` | `63b79745d537022d35b269277f847a8f05f485f2` |
| `vcn/vcn_4_0_0_sh_mask.h` | `251090d126d1ecab1450270b21dbac951fc61abb` |
| `amdgpu/sdma_v6_0_0_pkt_open.h` | `d8cf830916b9767b3c12c4bbd31feee10f96fc7d` |
| `amdgpu/nbio_v4_3.h` | `399037cdf4fbdf317ff566b8d89d3f7fd180df74` |

## Existing RedNavi generated-header audit

The current branch already has generated headers whose provenance comments say:

- `RedNavi/gc_regs.h` — generated from `gc_11_0_0` headers.
- `RedNavi/nbio_regs.h` — generated from `nbio_4_3_0` headers.
- `RedNavi/mmhub_regs.h` — generated from `mmhub_3_0_0` headers.
- `RedNavi/osssys_regs.h` — generated from `osssys_6_0_0` headers.
- `RedNavi/dcn_reglist.h` — generated from `dcn_3_2_0_offset.h`.

**Metadata discrepancy to resolve with the maintainer:** `RedNavi/asic.cpp` currently describes the Navi 32 GC header as `gc_11_0_3`, but the actual `gc_regs.h` generator banner says `gc_11_0_0`. This draft does not silently rewrite that metadata. Confirm the intended source/version label and correct it in a separate reviewed change.

The fact that the generic Linux header families overlap with RedNavi's current generated headers does not prove the Navi 32 register programming sequence is safe on Navi 33. In particular, do not reuse the RX 7800 XT display replay, board connector mapping, memory layout, firmware selection, or ASIC initialization sequence based on header-family overlap alone.

## Remaining checks

1. Inspect each register touched by RedNavi's write stages and compare its meaning, base-index behavior, and masks against the Navi 33 Linux implementation.
2. Verify whether GC 11.0.2 and DCN 3.2.1 require ASIC-specific register overrides despite the generic header families.
3. Decide whether RedNavi needs SDMA, VCN, and additional IP register definitions at all for its planned initial scope; do not add unused generated headers.
4. Keep all of these as source references until maintainer review and an ASIC-specific write-stage design are complete.
