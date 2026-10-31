#!/usr/bin/env python3
"""Read, on Linux with amdgpu loaded (run as root), the per-pipe state RedNavi compares with: the MEC pipes' program counters, the two MES pipes' program
counters, and the hardware queue descriptors of the MEC queues and of the MES queues. It uses the debugfs register file with the "ring" bank
selection (bit 61 of the file offset, then ME at bit 24, pipe at 34, queue at 44, VMID at 54), which does what GRBM_GFX_CNTL does in the driver.

    sudo python3 tools/linux_pipe_pcs.py ~/pipe_pcs_linux.txt

Read only. The GC is kept awake the same way tools/linux_gc_regs.py does (amdgpu_gfxoff written 0).
"""
import glob
import os
import struct
import sys
import time

# absolute dword offsets on this card (GC bases 0x1260 0xa000 0x1c000 0x2402c00; the values are the ones tools/linux_gc_regs.py prints)
CP_MEC_RS64_INSTR_PNTR = 0xC908
CP_MES_INSTR_PNTR = 0xC813
CP_MES_CNTL = 0xC807
MES_REGS = [('START', 51200), ('START_HI', 51357), ('INTR_RTN', 51201), ('INTR_RTN_HI', 51202), ('CNTL', 51207), ('MIE', 51214), ('PC', 51219), ('MSCR_HI', 51220), ('MSCR_LO', 51221), ('MSTAT_LO', 51222), ('MSTAT_HI', 51223), ('MEPC', 51224), ('MCAUSE', 51226), ('MBADADDR', 51228), ('MIP', 51230), ('IC_OP', 51232), ('DC_OP', 51255), ('GP0', 51267), ('GP0H', 51268), ('GP1', 51269), ('GP1H', 51270), ('GP2', 51271), ('GP2H', 51272), ('GP3', 51273), ('GP3H', 51274), ('GP4', 51275), ('GP4H', 51276), ('IC_BASE', 63568), ('IC_BASE_HI', 63569), ('IC_CNTL', 63570), ('DC_BASE', 63572), ('DC_BASE_HI', 63573)]
HQD = [("CP_HQD_ACTIVE", 0x320B), ("CP_HQD_VMID", 0x320C), ("CP_MQD_BASE_ADDR", 0x3209), ("CP_HQD_PQ_BASE", 0x3211), ("CP_HQD_PQ_CONTROL", 0x321A),
       ("CP_HQD_PQ_DOORBELL_CONTROL", 0x3218), ("CP_HQD_PQ_RPTR", 0x3213), ("CP_HQD_PQ_WPTR_LO", 0x323F), ("CP_HQD_PERSISTENT_STATE", 0x320D)]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    card = None
    for d in sorted(glob.glob("/sys/kernel/debug/dri/*")):
        try:
            if "amdgpu" in open(d + "/name").read() and os.path.exists(d + "/amdgpu_regs"):
                card = d
                break
        except OSError:
            pass
    if not card:
        sys.exit("no amdgpu debugfs (driver loaded? root?)")
    fd = os.open(card + "/amdgpu_regs", os.O_RDONLY)
    if os.path.exists(card + "/amdgpu_gfxoff"):
        g = os.open(card + "/amdgpu_gfxoff", os.O_RDWR)
        os.write(g, struct.pack("<I", 0))
    out = open(sys.argv[1], "a")

    def emit(line):
        out.write(line + "\n")
        out.flush()
        os.fsync(out.fileno())

    def read(dword, me=0, pipe=0, queue=0):
        pos = (1 << 61) | (me << 24) | (pipe << 34) | (queue << 44) | (dword * 4)
        return struct.unpack("<I", os.pread(fd, 4, pos))[0]

    emit("# kernel: " + os.uname().release)
    emit("# cmdline: " + open("/proc/cmdline").read().strip())
    for sample in range(3):
        for pipe in range(4):
            emit("MEC_PC me1 pipe%d sample%d 0x%08x" % (pipe, sample, read(CP_MEC_RS64_INSTR_PNTR, 1, pipe)))
        for pipe in range(2):
            emit("MES_PC me3 pipe%d sample%d 0x%08x" % (pipe, sample, read(CP_MES_INSTR_PNTR, 3, pipe)))
        time.sleep(0.05)
    for pipe in range(2):
        for name, dword in MES_REGS:
            emit("MESREG pipe%d %s 0x%08x" % (pipe, name, read(dword, 3, pipe)))
    emit("CP_MES_CNTL 0x%08x" % read(CP_MES_CNTL, 3, 0))
    for me, pipes, queues in ((1, 4, 8), (3, 2, 1)):
        for pipe in range(pipes):
            for queue in range(queues):
                active = read(0x320B, me, pipe, queue)
                if me == 1 and not active:
                    emit("HQD me%d pipe%d q%d CP_HQD_ACTIVE 0x%08x" % (me, pipe, queue, active))
                    continue
                for name, dword in HQD:
                    emit("HQD me%d pipe%d q%d %s 0x%08x" % (me, pipe, queue, name, read(dword, me, pipe, queue)))
    emit("# done")


if __name__ == "__main__":
    main()
