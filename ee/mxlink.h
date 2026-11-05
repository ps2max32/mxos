#ifndef MXLINK_H
#define MXLINK_H

/* mxlink: the EE side of the PS2 loader and monitor, on maxsdk. It talks to
   ps2client through ps2link's IOP module (ps2link.irx) and protocol, and
   loads programs itself, those linked into maxsdk's VM too. */

#include <mx/types.h>

#define MXLINK_VERSION "0.1"

/* ---- the console (con.c): the status screen ----------------------------- */

/* 640 x 448 (NTSC) or 512 (PAL): characters 7.5 pixels wide (con.c),
   rows of 18 lines; con_rows rows (24, 28), CON_ROWS at most */
#define CON_COLS 84
#define CON_ROWS 28
extern u32 con_rows;

#define CON_WHITE 0xE0E0E0
#define CON_GREY 0x808080
#define CON_GREEN 0x60E060
#define CON_YELLOW 0x40E0E0
#define CON_RED 0x6060FF
#define CON_CYAN 0xE0E060

extern const u8 con_font[95][5];

/* Set up the GS and the screen (640 x 448 NTSC, 640 x 512 PAL, interlaced).
   Thread code. */
void con_init(void);
/* Text at (col, row) in colour 0xBBGGRR; the rest of the row is not
   touched. */
void con_text(u32 col, u32 row, u32 rgb, const char *s);
/* Row row cleared, then text formatted as mx_printf. */
void con_line(u32 row, u32 rgb, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* The event log at the bottom of the screen: one line more, scrolled; also
   on the debug output when it is safe (no program running). */
void con_log(u32 rgb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- the status screen (main.c) ---------------------------------------- */

enum { ST_BOOT, ST_READY, ST_RUNNING, ST_MENU };
void status_state(int state, const char *what);
int status_get(void);

/* ---- commands from the host (cmd.c) ------------------------------------ */

int cmd_init(void);

/* ---- programs (exec.c) -------------------------------------------------- */

/* Load the ELF at path and start it in a thread of its own with the
   arguments (argc strings, one after the other in args, argv[0] the path).
   Returns 0, < 0 with the reason on the console. */
int exec_elf(int argc, const char *args, u32 args_len);
/* Start that program from the command thread, as an EXECEE. */
void cmd_launch(int argc, const char *args, u32 args_len);
/* 1 while a program runs */
int exec_running(void);
/* Is the program running the shell (mc0:/MXOS/MXSHELL.ELF)? */
int exec_in_shell(void);
/* The next exec_elf starts the shell. */
void exec_shell_next(void);
/* Is this the resident mxlink, below the programs (not mxlink-high)? */
int exec_resident(void);
/* Stop the running program (its thread). */
void exec_kill(void);
void tlb_restore(void);

/* the restart (main.c): the IOP reset and mxlink started again, as ps2link
   does on a reset command */
void mxlink_restart(void) __attribute__((noreturn));
/* a program's return (<mx/loader.h>'s ret): mxlink started again, the IOP
   kept when the program left it as it was */
void mxlink_return(int status) __attribute__((noreturn));
/* syscall 4 (the BIOS's Exit) for the programs that do not call it (hook.S) */
void mxlink_exit_hook(void);
/* the path of the program started, for the screen after its return */
void exec_started(const char *path);
/* A program's mx_loader_launch: mxlink restarts and starts that program,
   the IOP as it is. */
/* The shell stopped for a host, the IOP kept: argc 0: to the text screen,
   else this program in its place. */
void mxlink_takeover(int argc, const char *args, u32 args_len) __attribute__((noreturn));
/* A host's program: it comes back to the text screen, not to the shell. */
void mxlink_host_run(void);
void mxlink_launch(int argc, const char *args, u32 args_len, const char *swap)
    __attribute__((noreturn));
/* The next program's swap file, for one exec_elf. */
void exec_swap(const char *swap);
/* programs returned to mxlink since it booted, the last one's exit status */
u32 exec_returns(void);
s32 exec_last_status(void);

#endif
