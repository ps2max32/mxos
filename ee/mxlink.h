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
/* <mx/kernel.h>'s pad_init: the SIO2 ports open (mx__pad_init), a pad that
   was there before keeps its port while it is found again */
int pad_init(void);
/* <mx/kernel.h>'s pad_read */
int pad_read(int port, struct mx_pad_state *s, int sticks);
/* the ports a program can read: the two SIO2 ones and mxpad.irx's slots */
#define PAD_PORTS 6
/* the same without the settings: the SIO2 pads, then the USB pads */
int pad_merged(int port, struct mx_pad_state *s, int sticks);
/* after the IOP's set-up: the USB pads (mxpad.irx), if it runs */
void pad_usb_start(void);
/* <mx/kernel.h>'s pad_rumble (version 11); and at a program's end, every
   motor it left running off */
int pad_rumble(int port, u32 small, u32 big);
void pad_rumble_off(void);

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
void end_snapshot(int proc);
/* <mx/kernel.h>'s irq_tables: where the program's interrupt slots are */
void end_irq_tables(const struct mxk_irq_tables *t);
/* can a program end so (resident.ld has the program's area)? */
int end_possible(void);
/* its threads, semaphores, handlers, the units; then, after main.c's
   cleanup of the IOP, the kernel's state of it set back */
int end_tables_known(void);
int end_park(int self, int beside);
void end_unpark(void);
void end_drop(void);
u32 end_state_size(void);
void end_state_save(void *to);
void end_state_load(const void *from);
void end_state_swap(void *copy);
void end_unpark_beside(void);
/* kproc.c: a process beside the one in front (step 2e-2) */
struct vm_space;
void kproc_side_begin(int proc, void *state, u32 core_slots, u32 core_end, struct vm_space *space);
/* vm.c's address spaces (mx OS plan, phase 3a) */
struct vm_space *mx__vm_space_new(u32 size);
void mx__vm_space_switch(struct vm_space *s);
struct vm_space *mx__vm_space_now(void);
void mx__vm_space_free(struct vm_space *s);
void mx__vm_space_limit(struct vm_space *s, u32 bytes);
int mx__vm_swap_open(const char *cfg_swap);
void mx__vm_shutdown(void);
void mx__vm_swap_close(void);
void mx__vm_tlb_again(void);
void mx__vm_tlb_fix(void);
void mx__vm_swap_size(u32 bytes);
const char *kswap_path(u32 *bytes);
void *mx__vm_frames_take_aligned(u32 bytes, u32 align);
void mx__vm_frames_give(void *p, u32 bytes);
int exec_vm_kept(void);
struct vm_space *exec_front_space(void);
void kproc_side_end(void);
int kproc_side(void);
u32 kproc_switches(void);
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
int exec_shell_was(void);
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
/* kirq.c: the kernel first at INTC and DMAC interrupts (step 2d-1) */
void kirq_on(void);
void kirq_off(void);
/* kirq.c, ksys.S: the BIOS syscalls of the program started last, counted
   (mx OS plan step 2h-3) */
void ksys_watch_set(u32 asid);
const u32 *ksys_watch_end(void);
/* the kernel's own BIOS syscalls since its boot (step 2h-4) */
const u32 *ksys_kernel_counts(void);
/* a syscall handler of the kernel's own (ksys.S; step 2h-4a): 0, or -1 */
int ksys_set(int number, void *handler);
/* before a program: the BIOS thread's SetupThread arguments none (kirq.c) */
void kirq_setup_args_clear(void);
void kirq_counts(u32 *intc, u32 *dmac);
u32 kirq_bios_count(void);
void kirq_sif0_off(void);
int kirq_kept(void);
void kirq_keep(void);
int kirq_is_on(void);
u32 kpeek32(u32 a);
void kpoke32(u32 a, u32 v);
u32 kpeek8(u32 a);
void kpoke8(u32 a, u32 v);
u32 kpeek16(u32 a);
void kpoke16(u32 a, u32 v);
/* kvideo.c: the CRTC timing without the BIOS (kernel version 22) */
struct mx_crtc;
void kvideo_init(void);
int kvideo_crtc_set(const struct mx_crtc *c);
void kvideo_board_offset(int code, int *dx, int *dy);
int tlb_core_map(u32 va_end, u32 off);
/* PCSX2's TLB ignores the ASID (tlb.c): measured at the start, the way round */
void tlb_emu_asid(u32 asid);
extern int tlb_emu;
int tlb_ignores_asid(void);
void tlb_core_keep(u32 n, u32 va_end);
/* The BIOS's entry for mxlink's code, for a loader that writes there. */
void tlb_writable(void);

/* the restart (main.c): the IOP reset and mxlink started again, as ps2link
   does on a reset command */
void mxlink_restart(void) __attribute__((noreturn));
/* main.c: lines about a hang, kept over the restart for the host */
void hang_note(const char *line);
void hang_threads(void);
/* what the first steps found, said once the screen is there */
void kirq_report(void);
void kvideo_report(void);
void hang_log(void);
/* a program's return (<mx/loader.h>'s ret): mxlink started again, the IOP
   kept when the program left it as it was */
void mxlink_return(int status) __attribute__((noreturn));
/* the path of the program started, for the screen after its return */
void exec_started(const char *path);
/* A program's mx_loader_launch: mxlink restarts and starts that program,
   the IOP as it is. */
/* The shell stopped for a host, the IOP kept: argc 0: to the text screen,
   else this program in its place. */
void mxlink_takeover(int argc, const char *args, u32 args_len) __attribute__((noreturn));
/* A host's program: it comes back to the text screen, not to the shell. */
void mxlink_host_run(void);
/* mx_loader_launch: the caller ends, or waits and gets the other's exit
   status (step 2c, exec_park) */
int mxlink_launch(int argc, const char *args, u32 args_len, const char *swap);
int mxlink_launch_beside(int argc, const char *args, u32 args_len, const char *swap);
/* step 2c (exec.c) */
int exec_parked(void);
int exec_park(int beside);
int exec_park_done(void);
int exec_park_status(void);
void exec_unpark(int status);
int exec_park_drop(void);
void exec_park_forget(void);
/* the pad's ports closed (main.c) */
void pads_close(void);
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

/* processes and messages (kernel version 13; kmsg.c, exec.c) */
struct mx_proc_info;
struct mx_msg;
int end_proc_front(void);
int end_proc_parked(void);
const char *parked_program(int level);
int kproc_list(struct mx_proc_info *p, int max);
int kproc_self(void);
void exec_end_range(u32 *lo, u32 *hi);
int kproc_spawn(int argc, const char *args, u32 args_len, const char *swap);
int kproc_wait(int id, int *status, int nohang);
int kproc_kill(int id, int status);
void exec_park_wait_set(void);
void exec_park_wait_clear(void);
int exec_park_child(void);
void exec_park_started(void);
void exec_park_service_set(void);
int exec_park_service(void);
int exec_service_caller(void);
void exec_park_start_failed(void);
void exec_front_ended(int status);
int kproc_service(void);
void kmsg_init(void);
void kmsg_proc_end(int proc);
int kmsg_port_create(const char *name);
int kmsg_port_find(const char *name);
int kmsg_port_delete(int port);
int kmsg_send(int port, const void *req, u32 len, void *reply, u32 max);
int kmsg_receive(int port, void *buf, u32 max, struct mx_msg *m);
int kmsg_reply(int msg, const void *reply, u32 len);
int kmsg_events(int port);
int kmsg_post(int proc, const void *data, u32 len);
int kmsg_iop_channel(u32 cid, int port);
int kmsg_host_post(const void *data, u32 len);
int kmem_copy(u32 phys, void *buf, u32 n, int write);
void klog_init(void);
void klog_write(const char *s, u32 n);
int klog_tty_write(const char *s, u32 n);
int klog_read(u32 *pos, void *buf, u32 max, int *proc, int wait);

#endif
