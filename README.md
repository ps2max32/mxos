# mxlink

A loader and monitor for the PS2: ps2link's EE side rewritten on maxsdk, with
ps2link's IOP module (`iop/`) and protocol, so `ps2client` and scripts around
it work with mxlink as with ps2link.

What it adds to ps2link:

- It loads ELFs itself, on the EE: programs linked into maxsdk's virtual
  memory (`MX_MEMORY=vm`) run directly (`ps2client execee host:app.elf`), no
  nothing in between. Other programs, ps2sdk's included, get ps2link's
  arguments as they are.
- An ELF below 1 MiB (a loader: another mxlink, ps2link) is started by
  ExecPS2 and takes over the PS2. An ELF over mxlink itself is refused.
- A status screen: the network, the boot path, mxlink's memory, the state,
  and a log of the commands and errors (soft-edged text, 640 x 448 NTSC,
  640 x 512 PAL).
- Programs return to mxlink when they end: no reboot, the IOP and the
  network kept unless the program loaded IOP modules or reset the IOP (then
  mxlink resets it: a little over 4 s). With the IOP kept mxlink cleans up
  after the program: open files, pad ports, sound. Sound and the pad are
  services on the IOP, so programs using them load nothing and the IOP stays.
  maxsdk's `mx_exit` calls mxlink's
  return function (since maxsdk 8f9a72f; crash reports too); the BIOS's Exit
  is replaced for the others, ps2sdk's and older maxsdk ones (`ee/hook.S`).
  The screen shows the last program and its exit status.
- A maxsdk program on `libmxk` (linked into VM, or `MX_KERNEL=1`) ends without
  the restart (ExecPS2) the others get: the command thread terminates and
  deletes its threads and semaphores, removes its interrupt handlers, resets
  the DMA channels, VIF, GIF, IPU, VU0/VU1 and the timers, and sets mxlink's
  state of the program back (`ee/end.c`). The screen keeps its log.
- Held together, Select + Start + L1 + R1 open the home menu over a `libmxk`
  program on maxsdk's frame pipeline (`ee/home.c`): resume, pads 1 and 2
  swapped, Cross and Circle swapped, quit.
- A short boot sound (`iop/chime.c`), after every IOP reset: at boot, and
  when a program that loaded IOP modules returns. It resets the SPU2 first
  (the IOP's reset alone leaves the SPU2 playing). A return with the IOP kept
  makes no sound: audsrv mutes the stream and stays running for the next
  program, and the chime's quiet mode stops the voices. The module does not
  stay on the IOP.
- A shell: `mc0:/MXOS/MXSHELL.ELF`, if it is there, is started at boot and
  after each program it started (`mx_loader_launch`). `ps2client status`
  then says `state=menu`. A host command that needs `host:` stops the shell
  (cleaned up as after a program, the IOP kept, about a second; ps2client
  asks again), and `execee` from a client that does not ask first starts its
  program in the shell's place. A host's program returns to the text screen,
  with its exit status and crash report; `ps2client reset` goes back to the
  shell. A shell that ends itself also leaves the text screen up, so one
  that crashes at its start cannot loop.
- `shell/`, that shell (`make shell`): a carousel of the games in
  `<device>/PS2MAX/<game>/` with a `game.ini` (`package-games.sh`), drawn by
  beam racing in 24 bits. Storage drivers are loaded from `mc0:/MXOS` for the
  chosen device only (`SHELL.CFG` remembers it), so MX4SIO's polling costs
  nothing without one. All paths of a `game.ini` are relative to its
  directory: `title`, `year`, `type`, `elf`, `args`, `description` and
  `cover`, a file made by `tools/mk-cover.py` (the shell only copies it to GS
  memory; without one it draws a pattern from the title). A directory with
  just `<directory>.elf` is a game too. The font is Lato (SIL Open Font License), made into
  `shell/font.bin` by `shell/gen-font.py`. Select leaves the shell.
- The BIOS's TLB is written back at every start (`ee/tlb.c`): ExecPS2 leaves
  it as it is, so a killed program's VM entries would stay. mxlink's code,
  from 0xC0000, is read-only for programs: a wild store there is an exception.
- `ps2client status` says what it is doing (state, program, programs
  returned, last exit status) and which PS2 it is on: its model
  (`SCPH-70004`) and `rom0:ROMVER`, each `unknown` if the PS2 does not say.
  The model is the name stored in the PS2, read from its CD/DVD controller;
  older controllers (below version 0x10500) are not asked, as with ps2sdk's
  `sceCdRM`. The IOP module answers it alone, so it is safe while a program
  runs, unlike `ping`.

Not yet: catching crashes of ps2sdk programs, a "return to mxlink"
command, the VU register dumps.

## Build

```
make all high      # build/tools/mxlink/MXLINK.ELF, mxlink-high.elf
```

- `MXLINK.ELF`: resident at 0x88000-0xFF800, below the programs;
  the one to install.
- `MXOS/*.irx`: the IOP modules. mxlink reads them from `mc0:/MXOS` at each
  IOP start, with what the IOP has or else with the ROM's memory card drivers,
  so it needs them there: without them it has no network. The network runs on
  maxsdk's drivers and stack (`mxdev9`, `mxeth`, `mxnet`), and only on them:
  a PS2 with the PCMCIA DEV9 (SCPH-1xxxx) has none. `mxfast` is the fast path
  for `host:` files and the console: to the ps2client that serves the fast
  path without loss, else by broadcast as ps2sdk's `udptty` sent it.
- `mxlink-high.elf`: the same at 0x1E00000, to try a build without
  installing it: from a running loader, `ps2client execee
  host:mxlink-high.elf` (its IOP reset ends the connection: expected), then
  the same with `host:MXLINK.ELF`, then the tests. A reboot starts the
  installed loader again.
- `make SHOT=1`: on PCSX2 (no network there) the start stops at SMAP; this
  saves the screen to `host:mxlink.tga` there and prints the test end marker.
- Needs maxsdk (its own copy of libmx, built for size, in
  `build/tools/mxlink/maxsdk`), the feature LLVM (clang, ld.lld) and
  ps2sdk's IOP headers and `iopfixup` (`PS2MAX_IOP_SDK`) for `iop/`, and ps2sdk's prebuilt IOP
  modules (`PS2MAX_IRX`). Python only to change the boot sound
  (`iop/chime_blip.py` writes `iop/chime_blip.h`, which is committed).

## Install

Fill `mc0:/MXOS` first: `make mxos-fill`, then from the directory holding
`MXOS/`, with a loader on the PS2, `ps2client execee host:mxos-fill.elf`.

Copy `MXLINK.ELF` to a memory card and have your boot loader start it, as
with ps2link; keep the file it replaces, to go back to if the new one does
not start. The network address: `IPCONFIG.DAT` next to the ELF (`ip mask
gw`), else 192.168.1.10.
