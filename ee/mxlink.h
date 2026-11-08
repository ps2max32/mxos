#ifndef MXLINK_H
#define MXLINK_H

/* mxlink: the EE side of the PS2 loader and monitor, on maxsdk. It talks to
   ps2client through ps2link's IOP module (ps2link.irx) and protocol, and
   loads programs itself, those linked into maxsdk's VM too. */

#include <mx/debug.h>
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
/* the command thread: its id, and woken (a program ended: program_ended) */
int cmd_thread_id(void);
void cmd_wake(void);
/* main.c: a program has ended without the restart; the command thread ends it */
int end_pending(void);
void program_ended(void);

/* ---- the pad (pad.c) ------------------------------------------------- */

struct mx_pad_state;
/* the pad's settings, applied to what programs read */
struct pad_settings {
  u8 swap_ports; /* port 0 reads port 1 and the other way round */
  u8 swap_xo;    /* Cross and Circle swapped */
};
extern struct pad_settings pad_settings;
/* <mx/kernel.h>'s pad_read */
int pad_read(int port, struct mx_pad_state *s, int sticks);

/* ---- the home menu (home.c): <mx/kernel.h>'s version 7 ------------------ */

struct mx_video_mode;
void home_gs_display(const u64 *regs, u32 mask);
void home_gs_mode(const struct mx_video_mode *m);
u32 home_wanted(void);
int home_run(u32 page, u32 pages);
/* the buttons a program gets: the combination taken for the menu */
u32 home_filter(u32 buttons);
/* the end of the program: its vblank function off */
void home_unhook(void);

/* ---- a program's end without the restart (end.c) ------------------------ */

struct mxk_irq_tables;
/* the threads and semaphores there are, just before a program starts */
void end_snapshot(void);
/* <mx/kernel.h>'s irq_tables: where the program's interrupt slots are */
void end_irq_tables(const struct mxk_irq_tables *t);
/* can a program end so (resident.ld has the program's area)? */
int end_possible(void);
/* its threads, semaphores, handlers, the units; then, after main.c's
   cleanup of the IOP, the kernel's state of it set back */
int end_tables_known(void);
void end_cleanup(void);
void end_kernel_clients(void); /* main.c */
void end_reset_state(void);
void end_counts(u32 *threads_left, u32 *semas_left, u32 *calls_left);
/* what end_cleanup ended: the program's threads and semaphores */
void end_ended(u32 *threads, u32 *semas);

/* the running program's path (main.c) */
const char *status_program(void);

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
/* The memory mxlink takes. */
void exec_self(u32 *start, u32 *end);
/* Is the running program on libmxk (it may end without the restart)? */
int exec_on_libmxk(void);
/* it ended so: no program now */
void exec_ended(void);
/* Stop the running program (its thread). */
void exec_kill(void);
void tlb_restore(void);
/* The BIOS's entry for mxlink's code, for a loader that writes there. */
void tlb_writable(void);

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
/* The program's text for the report of a crash, 0 for a program without the
   kernel's exceptions. */
const char *const *exec_note(void);
/* The crash of the last program: 1, or 0 if it did not crash. */
int mxlink_crash(struct mx_crash *c);

#endif
