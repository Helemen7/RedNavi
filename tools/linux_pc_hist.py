#!/usr/bin/env python3
"""Histogram of the program counters of the two MES pipes and the MEC pipe 0 on Linux (run as root, amdgpu loaded), to see which addresses the KIQ pipe
visits and how often, at idle and under load. Read only; debugfs ring-bank select as tools/linux_pipe_pcs.py.

    sudo python3 tools/linux_pc_hist.py ~/pc_hist_idle.txt 3000          # idle, 3000 samples, about 1 ms apart
    sudo python3 tools/linux_pc_hist.py ~/pc_hist_load.txt 3000          # run a GPU load in another terminal at the same time
"""
import collections
import glob
import os
import struct
import sys
import time

CP_MEC_RS64_INSTR_PNTR = 0xC908
CP_MES_INSTR_PNTR = 0xC813


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    card = [d for d in glob.glob("/sys/kernel/debug/dri/*") if os.path.exists(d + "/amdgpu_regs")]
    if not card:
        sys.exit("no amdgpu debugfs")
    fd = os.open(card[0] + "/amdgpu_regs", os.O_RDONLY)
    if os.path.exists(card[0] + "/amdgpu_gfxoff"):
        os.write(os.open(card[0] + "/amdgpu_gfxoff", os.O_RDWR), struct.pack("<I", 0))

    def read(dword, me, pipe):
        return struct.unpack("<I", os.pread(fd, 4, (1 << 61) | (me << 24) | (pipe << 34) | (dword * 4)))[0]

    hist = {"mes_sched": collections.Counter(), "mes_kiq": collections.Counter(), "mec_pipe0": collections.Counter()}
    t0 = time.time()
    for _ in range(int(sys.argv[2])):
        hist["mes_sched"][read(CP_MES_INSTR_PNTR, 3, 0)] += 1
        hist["mes_kiq"][read(CP_MES_INSTR_PNTR, 3, 1)] += 1
        hist["mec_pipe0"][read(CP_MEC_RS64_INSTR_PNTR, 1, 0)] += 1
        time.sleep(0.001)
    with open(sys.argv[1], "w") as out:
        out.write("# %d samples in %.1f s, kernel %s\n" % (int(sys.argv[2]), time.time() - t0, os.uname().release))
        for name, c in hist.items():
            out.write("%s: %s\n" % (name, ", ".join("0x%04x x%d" % (pc, n) for pc, n in c.most_common())))
    print(open(sys.argv[1]).read())


if __name__ == "__main__":
    main()
