# The kernel's IOP modules

## chime.irx

The boot sound (`chime.c`): it resets the SPU2 and plays a short sound, then
unloads. The sound is `chime_blip.h`, made by `chime_blip.py`. Its start result
reports what it saw, and the kernel logs that.

## mxroot.irx

`host:` as a directory of another device, for programs started from storage
instead of by a PC (`mxroot.c`).

## Building

Both are built by `../Makefile` with maxsdk's IOP toolchain
(`libs/maxsdk/iop/iop.mk`): the feature clang for `mipsel-none-irx`, linked by
`ld.lld --irx`. `irx_imports.h` (from ps2link) only includes headers.
ps2link's module, `ps2link.irx`, is mxlink's (`apps/mxlink`).
