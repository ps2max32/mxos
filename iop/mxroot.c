/* mxroot.irx: host: as a directory of another device, for programs started
   from storage instead of by a PC. Its one argument is that directory, e.g.
   "usb0:/PS2MAX/quake1/": host:id1/pak0.pak then opens
   usb0:/PS2MAX/quake1/id1/pak0.pak. It replaces ps2link.irx's host: until the
   next IOP reset. The games name their files host:..., so they run as they
   are, their swap file and saves included. */
#include <types.h>
#include <irx.h>
#include <ioman.h>
#include <loadcore.h>
#include <sysclib.h>
#include "irx_imports.h"

IRX_ID("mxroot", 1, 0);

#define PATH_MAX_LEN 256
#define STAT_X 64 /* iomanX's stat: drivers written for it fill in this much */

static char root[128];
static u32 root_len;

static const char *full(char *p, const char *name) {
  while (*name == '/')
    name++;
  u32 n = strlen(name);
  if (root_len + n >= PATH_MAX_LEN)
    n = PATH_MAX_LEN - 1 - root_len;
  memcpy(p, root, root_len);
  memcpy(p + root_len, name, n);
  p[root_len + n] = 0;
  return p;
}

static int rootOpen(iop_file_t *f, const char *name, int mode) {
  char p[PATH_MAX_LEN];
  int fd = open(full(p, name), mode);
  if (fd < 0)
    return fd;
  f->privdata = (void *)fd;
  return 0;
}
static int rootClose(iop_file_t *f) { return close((int)f->privdata); }
static int rootRead(iop_file_t *f, void *b, int n) { return read((int)f->privdata, b, n); }
static int rootWrite(iop_file_t *f, void *b, int n) { return write((int)f->privdata, b, n); }
static int rootLseek(iop_file_t *f, int off, int whence) {
  return lseek((int)f->privdata, off, whence);
}
static int rootRemove(iop_file_t *f, const char *name) {
  char p[PATH_MAX_LEN];
  (void)f;
  return remove(full(p, name));
}
static int rootMkdir(iop_file_t *f, const char *name) {
  char p[PATH_MAX_LEN];
  (void)f;
  return mkdir(full(p, name));
}
static int rootRmdir(iop_file_t *f, const char *name) {
  char p[PATH_MAX_LEN];
  (void)f;
  return rmdir(full(p, name));
}
static int rootDopen(iop_file_t *f, const char *name) {
  char p[PATH_MAX_LEN];
  int fd = dopen(full(p, name), 0);
  if (fd < 0)
    return fd;
  f->privdata = (void *)fd;
  return 0;
}
static int rootDclose(iop_file_t *f) { return dclose((int)f->privdata); }
/* the drivers on usb: and the like fill in iomanX's larger structures: here
   they get room for that, and the caller what it expects */
static int rootDread(iop_file_t *f, io_dirent_t *d) {
  static union {
    io_dirent_t io;
    char x[STAT_X + 256 + 8];
  } e;
  memset(&e, 0, sizeof e);
  int r = dread((int)f->privdata, &e.io);
  if (r > 0) {
    memcpy(&d->stat, &e.io.stat, sizeof d->stat);
    memcpy(d->name, e.io.name[0] ? e.io.name : e.x + STAT_X, sizeof d->name);
  }
  return r;
}
static int rootGetstat(iop_file_t *f, const char *name, io_stat_t *st) {
  char p[PATH_MAX_LEN];
  static char x[STAT_X];
  (void)f;
  int r = getstat(full(p, name), (io_stat_t *)x);
  if (r >= 0)
    memcpy(st, x, sizeof *st);
  return r;
}
static int rootInit(iop_device_t *d) { (void)d; return 0; }
static int rootDeinit(iop_device_t *d) { (void)d; return 0; }
static int rootNone(void) { return -5; }

static iop_device_ops_t ops = {
    (void *)rootInit,  (void *)rootDeinit, (void *)rootNone,   (void *)rootOpen,
    (void *)rootClose, (void *)rootRead,   (void *)rootWrite,  (void *)rootLseek,
    (void *)rootNone,  (void *)rootRemove, (void *)rootMkdir,  (void *)rootRmdir,
    (void *)rootDopen, (void *)rootDclose, (void *)rootDread,  (void *)rootGetstat,
    (void *)rootNone};
static iop_device_t dev = {"host", IOP_DT_FS, 1, "mxroot", &ops};

int _start(int argc, char **argv) {
  if (argc < 2)
    return MODULE_NO_RESIDENT_END;
  root_len = strlen(argv[1]);
  if (root_len >= sizeof root)
    return MODULE_NO_RESIDENT_END;
  memcpy(root, argv[1], root_len + 1);
  DelDrv("host");
  AddDrv(&dev);
  return MODULE_RESIDENT_END;
}
