# mxlink plan

A maxsdk replacement for ps2link's EE side: compatible with ps2client and
ps2link's protocol, loads maxsdk VM programs directly, and becomes a resident
monitor that programs return to without a reboot (option 2 of the
2026-11-03 options; UI level U1 first).

## Steps

1. **Port (done; passed on the PS2 2026-11-03: ping, maxsdk hello, a
   VM-linked test without mxload).** EE side on maxsdk at 0x94000-0xFF800,
   ps2link.irx (built here from iop/, byte for byte ps2link's) and protocol
   as they are, the ELF loader of
   maxsdk's mxload built in (VM segments left to the program), a text status
   console (U1). `make high` builds a copy at 0x1E00000 to start from a
   running ps2link (`execee`) for tests without installing it.
   Checked on PCSX2: start, ExecPS2 restart, IOP reset, IOP modules up to
   SMAP (PCSX2 has no network), the console (`make SHOT=1`).
2. **Return without a reboot.** Done for maxsdk programs (2026-11-03, on
   the PS2: hello, a VM program, a crash, about 3 s from one run to the
   next): mxlink passes ret_magic/ret after ps2link's argument block
   (<mx/loader.h>), mx_exit calls it, mxlink restarts by ExecPS2 and keeps
   the IOP unless the program loaded modules (mx_iop_last_module). The
   others (ps2sdk's, older maxsdk ones): syscall 4 (the BIOS's Exit)
   replaced (ee/hook.S, Open PS2 Loader's way), checked with a plain exit,
   a module loaded, the IOP reset by the program: back in a little over
   4 s with an IOP reset. Still to do: exceptions of ps2sdk programs, DMA/VU
   state checks. The plan as it was: Hook syscall 4 (Exit) with SetSyscall, and
   give maxsdk programs a return address in the argument block; on return
   ExecPS2 mxlink itself (the kernel drops the program's threads, semaphores
   and handlers; memory stays), then restore the SIF (CHANGE_SADDR to
   mxlink's buffer), GS, DMA, VIF/VU, TLB. Reset the IOP only when the
   program changed it (its module list against the one at the start).
   Catch exceptions of ps2sdk programs: report on screen and to the host,
   back to the monitor.
3. **Protocol extensions** (versioned, old commands unchanged): a status
   query that does not cut off a running program's files, "return to
   monitor", the exit status as a packet, a GS screenshot. Changes to
   mxlink's IOP module (iop/: several host connections in net_fsys.c). Then the
   hw-test runner without its reboot wait.
4. U2 (pad launcher, file browser, log, crash screen) and U3 (host TUI)
   later.

## Open points

- Checked on the PS2 2026-11-03: reset (ExecPS2 restart, IOP reset, modules
  again) and the refusal of an ELF over mxlink itself (exec of
  mxlink-high.elf from mxlink-high hung the PS2 before).

- Not ported yet: dumpreg of VU0/VU1 (REGVU0, REGVU1).
- While a program runs, mxlink cannot use the IOP's file server (the
  program's SIF init took the replies): dumpmem, writemem, dumpreg, gsexec
  and ping are refused then; ps2link had the same limit.
- The command thread (priority 20) does not run while a maxsdk VM program
  runs above it (the VM moves the program to priority 2): reset then needs
  step 2's handling in the interrupt.
