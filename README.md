# mxos

The kernel of mx OS, the PS2's resident OS of ps2max: processes with address
spaces of their own, virtual memory, the kernel's threads and interrupts, the
pad, the display and the home menu, the boot: `MXKERNEL.ELF`. It grew out of
mxlink, ps2link's EE side rewritten on maxsdk (the loader was called so until
2026-11-10). The host link is now
[mxlink](https://github.com/ps2max32/mxlink)'s app, a service beside the
programs (`MXLINK.ELF`), with ps2link's protocol and IOP module, so `ps2client`
and the scripts around it work as with ps2link; the kernel keeps a fallback for
the host (`ee/cmd.c`). The shell is
[mxshell](https://github.com/ps2max32/mxshell). Where it goes: maxsdk's
`docs/mx-os-plan.md` and `docs/mx-os-apps-plan.md`; what it offers programs:
maxsdk's `docs/mx-os.md` and `docs/kernel.md`.

What it adds to ps2link:

- It loads ELFs itself, on the EE: programs linked into maxsdk's virtual
  memory (`MX_MEMORY=vm`) run directly (`ps2client execee host:app.elf`), no
  nothing in between. Other programs, ps2sdk's included, get ps2link's
  arguments as they are.
- An ELF in the kernel's own memory (another kernel) replaces the running
  one. An ELF below 1 MiB (a loader such as ps2link) is refused, and so is a
  program over the kernel.
- A status screen: the network, the boot path, the kernel's memory, the state,
  and a log of the commands and errors (soft-edged text, 640 x 448 NTSC,
  640 x 512 PAL).
- Programs return to the kernel when they end: no reboot, the IOP and the
  network kept unless the program loaded IOP modules or reset the IOP (then
  the kernel resets it: a little over 4 s). With the IOP kept the kernel cleans up
  after the program: open files, pad ports, sound. Sound and the pad are
  services on the IOP, so programs using them load nothing and the IOP stays.
  maxsdk's `mx_exit` calls the kernel's
  return function (since maxsdk 8f9a72f; crash reports too); the BIOS's Exit
  is replaced for the others, ps2sdk's and older maxsdk ones (`ee/hook.S`).
  The screen shows the last program and its exit status.
- A maxsdk program on `libmxk` (linked into VM, or `MX_KERNEL=1`) ends without
  the restart (ExecPS2) the others get: the command thread terminates and
  deletes its threads and semaphores, removes its interrupt handlers, resets
  the DMA channels, VIF, GIF, IPU, VU0/VU1 and the timers, and sets the kernel's
  state of the program back (`ee/end.c`). The screen keeps its log.
- Held together, Select + Start + L1 + R1 open the home menu over a `libmxk`
  program on maxsdk's frame pipeline (`ee/home.c`): resume, pads 1 and 2
  swapped, Cross and Circle swapped, quit.
- A short boot sound (`iop/chime.c`), after every IOP reset: at boot, and
  when a program that loaded IOP modules returns. It resets the SPU2 first
  (the IOP's reset alone leaves the SPU2 playing). A return with the IOP kept
  makes no sound: maxsdk's mxsnd (booted with libsd) stops the ending
  program's voices and stream and stays running for the next program. The
  module does not stay on the IOP.
- A service: `mc0:/MXOS/SERVICES.CFG` names a program (a path and its
  arguments) that starts at the boot and after a restart, before the shell,
  and stays beside every program in front (kernel version 18). It is the
  host link's app (`MXLINK.ELF`, from apps/mxlink): from kernel version
  20 it takes ps2client's `execee` and ping, which the kernel passes on to
  it; the kernel serves the other commands, and all of them while no service
  runs.
- A shell: `mc0:/MXOS/MXSHELL.ELF`, if it is there, is started at boot and
  after each program it started (`mx_loader_launch`). A game built on
  maxsdk for kernel version 9 runs beside it instead: the shell waits in
  memory (its core at the top, under its own ASID) and goes on at once when
  the game ends; an older game takes all memory and the shell starts again
  after it. `ps2client status`
  then says `state=menu`. A host command that needs `host:` stops the shell
  (cleaned up as after a program, the IOP kept, about a second; ps2client
  asks again), and `execee` from a client that does not ask first starts its
  program in the shell's place. A host's program returns to the text screen,
  with its exit status and crash report; `ps2client reset` goes back to the
  shell. A shell that ends itself also leaves the text screen up, so one
  that crashes at its start cannot loop.
- The shell itself is mxshell (`make shell` builds it from `apps/mxshell` into
  `MXOS/`).
- The BIOS's TLB is written back at every start (`ee/tlb.c`): ExecPS2 leaves
  it as it is, so a killed program's VM entries would stay. The kernel's code,
  from 0x1F00000, is read-only for programs: a wild store there is an exception.
- On a console whose video it sets itself the kernel does not use the BIOS
  after its first start (maxsdk's `docs/kernel.md`); `make KBIOS_GONE=1` is a
  test build that also fills the BIOS's memory with breaks.
- `ps2client status` says what it is doing (state, program, programs
  returned, last exit status) and which PS2 it is on: its model
  (`SCPH-70004`) and `rom0:ROMVER`, each `unknown` if the PS2 does not say.
  The model is the name stored in the PS2, read from its CD/DVD controller;
  older controllers (below version 0x10500) are not asked, as with ps2sdk's
  `sceCdRM`. The IOP module answers it alone, so it is safe while a program
  runs, unlike `ping`.

Not yet: catching crashes of ps2sdk programs, a "return to the kernel"
command, the VU register dumps.

## Build

```
make all           # build/apps/mxos/MXKERNEL.ELF
make shell         # mxshell into build/apps/mxos/MXOS
```

- `MXKERNEL.ELF`: resident in the last MiB of main memory
  (0x1F00000-0x1FFFFFF), above the programs; the one to install. To try a
  build without installing it: `ps2client execee host:MXKERNEL.ELF` replaces
  the running kernel (the connection ends: expected); a reset boots the
  installed one again.
- `MXOS/*.irx`: the IOP modules. The kernel reads them from `mc0:/MXOS` at each
  IOP start, with what the IOP has or else with the ROM's memory card drivers,
  so it needs them there: without them it has no network. `ps2link.irx` comes
  from `apps/mxlink` (`MXLINK=<directory>`), the shell from `apps/mxshell`
  (`MXSHELL=<directory>`). The network runs on
  maxsdk's drivers and stack (`mxdev9`, `mxeth`, `mxnet`), and only on them:
  a PS2 with the PCMCIA DEV9 (SCPH-1xxxx) has none. `mxfast` is the fast path
  for `host:` files and the console: to the ps2client that serves the fast
  path without loss, else by broadcast as ps2sdk's `udptty` sent it.
  The kernel reads its modules, its settings and its shell from the directory
  it was started from (`argv[0]`'s); started from `host:`, or without
  `chime.irx` there, from `MODULE_DIR`, `mc0:/MXOS/` (`make MODULE_DIR=...`). To try new
  modules: copy them (and `MXSHELL.ELF`, `MXLINK.ELF` with a `SERVICES.CFG`
  naming it there) with `MXKERNEL.ELF` to a
  directory of their own, e.g. `mc0:/MXOS/NEW`, and boot that kernel as the
  one-shot (`once mc0: mc?:/MXOS/NEW/MXKERNEL.ELF`): if it fails, a reset boots
  the default. The memory card driver is maxsdk's `mxmc` (16 open files).
- `MXOS/icon.sys`, `MXOS/MXOS.ICN`: the folder's icon in the PS2's card
  browser ("MX OS / kernel"), from `icon/`. `tools/mx-icon.py` makes it, and
  mxboot's, in the shell's look: a bevelled MX on a two-layer plinth (OS and
  KERNEL; BOOT and MAIN or a region), under one warm light (`--preview` draws the files
  back as a picture). It needs the shell's font and Python with numpy and
  Pillow; the files are kept in git, so a build does not run it.
- `make SHOT=1`: on PCSX2 (no network there) the start stops at SMAP; this
  saves the screen to `host:mxkernel.tga` there and prints the test end marker.
- Needs maxsdk (its own copy of libmx, built for size, in
  `build/apps/mxos/maxsdk`), the feature LLVM (clang, ld.lld) and
  ps2sdk's IOP headers (`PS2MAX_IOP_SDK`) for `iop/` and mxlink's, and ps2sdk's prebuilt IOP
  modules (`PS2MAX_IRX`). Python only to change the boot sound
  (`iop/chime_blip.py` writes `iop/chime_blip.h`, which is committed).

## Install

Fill `mc0:/MXOS` first: `make mxos-fill`, then from the directory holding
`MXOS/`, with a loader on the PS2, `ps2client execee host:mxos-fill.elf`.

Copy `MXKERNEL.ELF` to a memory card and have your boot loader start it, as
with ps2link; keep the file it replaces, to go back to if the new one does
not start. The network address: `IPCONFIG.DAT` next to the ELF (`ip mask
gw`), else 192.168.1.10.
