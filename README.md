# mxlink

A loader and monitor for the PS2: ps2link's EE side rewritten on maxsdk, with
ps2link's IOP module (`iop/`) and protocol, so `ps2client` and scripts around
it work with mxlink as with ps2link.

What it adds to ps2link:

- It loads ELFs itself, on the EE: programs linked into maxsdk's virtual
  memory (`MX_MEMORY=vm`) run directly (`ps2client execee host:app.elf`), no
  `mxload.elf` in between. Other programs, ps2sdk's included, get ps2link's
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
- A short boot sound (`iop/chime.c`), after every IOP reset: at boot, and
  when a program that loaded IOP modules returns. It resets the SPU2 first
  (the IOP's reset alone leaves the SPU2 playing). A return with the IOP kept
  makes no sound: audsrv stops the stream and the samples, and stays running
  for the next program. The module does not stay on the IOP.
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

- `MXLINK.ELF`: resident at 0x94000-0xFF800, below the programs (as ps2link);
  the one to install.
- `MXOS/*.irx`: the IOP modules. mxlink reads them from `mc0:/MXOS` at each
  IOP start, with what the IOP has or else with the ROM's memory card drivers,
  so it needs them there: without them it has no network.
- `mxlink-high.elf`: the same at 0x1E00000, to try a build without
  installing it: from a running loader, `ps2client execee
  host:mxlink-high.elf` (its IOP reset ends the connection: expected), then
  the same with `host:MXLINK.ELF`, then the tests. A reboot starts the
  installed loader again.
- `make SHOT=1`: on PCSX2 (no network there) the start stops at SMAP; this
  saves the screen to `host:mxlink.tga` there and prints the test end marker.
- Needs maxsdk (its own copy of libmx, built for size, in
  `build/tools/mxlink/maxsdk`), the IOP gcc and ps2sdk's IOP build files
  (`PS2MAX_IOP_BIN`, `PS2MAX_IOP_SDK`) for `iop/`, and ps2sdk's prebuilt IOP
  modules (`PS2MAX_IRX`). Python only to change the boot sound
  (`iop/chime_blip.py` writes `iop/chime_blip.h`, which is committed).

## Install

Fill `mc0:/MXOS` first: `make mxos-fill`, then from the directory holding
`MXOS/`, with a loader on the PS2, `ps2client execee host:mxos-fill.elf`.

Copy `MXLINK.ELF` to a memory card and have your boot loader start it, as
with ps2link; keep the file it replaces, to go back to if the new one does
not start. The network address: `IPCONFIG.DAT` next to the ELF (`ip mask
gw`), else 192.168.1.10.
