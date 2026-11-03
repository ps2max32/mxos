# mxlink

The loader and monitor the PS2 runs: ps2link's EE side rewritten on maxsdk,
with ps2link's IOP module (`iop/`) and protocol, so `ps2client` and the
`ps2-hw-test` runner work with it as with ps2link. Installed on the PS2 as
`mc0:/APPS/PS2LINK.ELF` (2026-11-03).

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

Not yet (PLAN.md): a program's exit returning to mxlink without a reboot, the
protocol extensions, the VU register dumps.

## Build

```
make all high      # build/tools/mxlink/MXLINK.ELF, mxlink-high.elf
```

- `MXLINK.ELF`: resident at 0x94000-0xFF800, below the programs (as ps2link);
  the one to install.
- `mxlink-high.elf`: the same at 0x1E00000, to start from a running loader
  for a test: `run.sh build/tools/mxlink/mxlink-high.elf 20` (its IOP reset
  ends the run with a lost connection), then `run.sh
  build/tools/mxlink/MXLINK.ELF 20` to try the real one, then the tests.
- `make SHOT=1`: on PCSX2 (no network there) the start stops at SMAP; this
  saves the screen to `host:mxlink.tga` there and prints the test end marker.
- Needs maxsdk (its own copy of libmx, built for size, in
  `build/tools/mxlink/maxsdk`), the IOP gcc and ps2sdk's IOP build files
  (`PS2MAX_IOP_BIN`, `PS2MAX_IOP_SDK`) for `iop/`, and ps2sdk's prebuilt IOP
  modules (`PS2MAX_IRX`).

## Install

`~/.claude/skills/ps2-hw-test/install-ps2link.sh build/tools/mxlink/MXLINK.ELF`
(backs the card's file up first; holding Up at power-on boots the old
high-loading ps2link). The network address: `IPCONFIG.DAT` next to the ELF
(`ip mask gw`), else 192.168.1.10.
