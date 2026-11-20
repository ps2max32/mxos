/* INIT.ELF: the boot script of mx OS (kernel version 46). The kernel starts
   it once at its first start, before the service and the shell, from its
   own directory (that directory is its working directory). It runs RC.SH
   from there, then ends; what it set with mx_setting_set holds for this
   boot only (not written to SETTINGS.DAT), and the kernel goes on with it:
   the service, the shell (shell.path, shell.args), the network and the swap
   file. Lines of words, as a shell's:

     # a comment
     set NAME value...       a variable ($NAME, ${NAME}; also KDIR, ROM, ?)
     echo words...           to the console
     cd DIR                  the working directory
     setting KEY value...    any setting, for this boot
     net dhcp | net IP MASK GW
     swap PATH [MiB]
     service PATH [args...]  the service, started before the shell
     shell PATH [args...]    the shell (a game may be the shell)
     run PATH [args...]      a program in front, waited for; $? its status
     if exists PATH | if equal A B | if match PATTERN TEXT (* at its end)
     if not ...  /  else  /  fi
     exit

   "..." keeps spaces. Paths without a device are in the working directory.
   An error stops the script with its line on the console and in a
   notification; the kernel then goes on as without it. */
#include <mx/notify.h>
#include <mx/proc.h>
#include <mx/setting.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/mx.h>

#define MAXW 16
#define NVAR 16
static struct {
  char name[16];
  char value[MX_SETTING_VALUE];
} var[NVAR];
static int nvar;
static char script[8192];
static int line_no;

static const char *var_get(const char *name) {
  for (int i = 0; i < nvar; i++)
    if (!strcmp(var[i].name, name))
      return var[i].value;
  return "";
}
static void var_set(const char *name, const char *value) {
  int i = 0;
  while (i < nvar && strcmp(var[i].name, name))
    i++;
  if (i == nvar) {
    if (nvar == NVAR)
      return;
    nvar++;
  }
  mx_snprintf(var[i].name, sizeof var[i].name, "%s", name);
  mx_snprintf(var[i].value, sizeof var[i].value, "%s", value);
}

static void fail(const char *what, const char *arg) {
  char text[MX_NOTIFY_TEXT];
  mx_snprintf(text, sizeof text, "RC.SH line %d: %s%s%s", line_no, what, arg ? ": " : "", arg ? arg : "");
  mx_printf("init: %s\n", text);
  mx_notify(MX_NOTIFY_PROGRAM, 1, "Boot script", text);
}

/* the line's words into w (in buf), $NAME and ${NAME} replaced; how many */
static int split(const char *line, char *buf, u32 room, char *w[MAXW]) {
  int n = 0;
  u32 b = 0;
  const char *p = line;
  while (*p && n < MAXW) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p || *p == '#')
      break;
    w[n++] = buf + b;
    int q = 0;
    while (*p && (q || (*p != ' ' && *p != '\t'))) {
      if (*p == '"') {
        q = !q, p++;
        continue;
      }
      if (*p == '$') {
        char name[16];
        u32 k = 0;
        int brace = p[1] == '{';
        p += 1 + brace;
        while (k + 1 < sizeof name && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                                       (*p >= '0' && *p <= '9') || *p == '_' || *p == '?'))
          name[k++] = *p++;
        name[k] = 0;
        if (brace && *p == '}')
          p++;
        for (const char *v = var_get(name); *v && b + 1 < room; v++)
          buf[b++] = *v;
        continue;
      }
      if (b + 1 < room)
        buf[b++] = *p;
      p++;
    }
    buf[b++] = 0;
  }
  return n;
}

/* a path: as written with a device, else in the working directory */
static void full(const char *path, char *out, u32 room) {
  const char *c = path;
  while (*c && *c != ':' && *c != '/')
    c++;
  if (*c == ':') {
    mx_snprintf(out, room, "%s", path);
    return;
  }
  char cwd[96];
  if (mx_fio_getcwd(cwd, sizeof cwd) < 0)
    cwd[0] = 0;
  mx_snprintf(out, room, "%s%s", path[0] == '/' ? "" : cwd, path);
}

static int set(const char *key, const char *value) {
  if (mx_setting_set(key, value) < 0) {
    fail("cannot set", key);
    return -1;
  }
  return 0;
}

/* words a..n-1 joined by spaces (a word with a space in "..."), into out */
static void join(char *const *w, int a, int n, char *out, u32 room) {
  u32 l = 0;
  out[0] = 0;
  for (int i = a; i < n && l < room; i++) {
    int sp = 0;
    for (const char *c = w[i]; *c; c++)
      sp |= *c == ' ';
    l += (u32)mx_snprintf(out + l, room - l, "%s%s%s%s", i > a ? " " : "", sp ? "\"" : "", w[i],
                          sp ? "\"" : "");
  }
}

static int exists(const char *path) {
  char p[96];
  full(path, p, sizeof p);
  int fd = mx_fio_open(p, MX_FIO_RDONLY);
  if (fd >= 0) {
    mx_fio_close(fd);
    return 1;
  }
  fd = mx_fio_dopen(p);
  if (fd >= 0) {
    mx_fio_dclose(fd);
    return 1;
  }
  return 0;
}

static int match(const char *pat, const char *text) {
  u32 n = (u32)strlen(pat);
  if (n && pat[n - 1] == '*')
    return !strncmp(pat, text, n - 1);
  return !strcmp(pat, text);
}

/* if's test: 1 true, 0 false, -1 an error */
static int test(char *const *w, int n) {
  int neg = n > 1 && !strcmp(w[1], "not"), a = 1 + neg, r;
  if (n == a + 2 && !strcmp(w[a], "exists"))
    r = exists(w[a + 1]);
  else if (n == a + 3 && !strcmp(w[a], "equal"))
    r = !strcmp(w[a + 1], w[a + 2]);
  else if (n == a + 3 && !strcmp(w[a], "match"))
    r = match(w[a + 1], w[a + 2]);
  else
    return -1;
  return neg ? !r : r;
}

static int run(char *const *w, int a, int n) {
  char args[256], path[96];
  u32 l = 0;
  full(w[a], path, sizeof path);
  for (int i = a; i < n && l < sizeof args; i++) {
    const char *s = i == a ? path : w[i];
    u32 k = (u32)strlen(s) + 1;
    if (l + k > sizeof args)
      break;
    memcpy(args + l, s, k);
    l += k;
  }
  int id = mx_proc_spawn(n - a, args, l, 0), st = -1;
  if (id < 0 || mx_proc_wait(id, &st, 0) < 0)
    st = -1;
  char v[12];
  mx_snprintf(v, sizeof v, "%d", st);
  var_set("?", v);
  return 0;
}

/* one line: 0 on, 1 exit, -1 an error */
#define DEPTH 8
static int skip[DEPTH], taken[DEPTH], depth;
static int line(char *const *w, int n) {
  if (!n)
    return 0;
  const char *c = w[0];
  int off = depth && skip[depth - 1];
  if (!strcmp(c, "if")) {
    if (depth == DEPTH)
      return fail("if nested too deep", 0), -1;
    int t = off ? 0 : test(w, n);
    if (t < 0)
      return fail("if: not a test", n > 1 ? w[1] : 0), -1;
    skip[depth] = off || !t, taken[depth] = off || t;
    depth++;
    return 0;
  }
  if (!strcmp(c, "else")) {
    if (!depth)
      return fail("else without if", 0), -1;
    int outer = depth > 1 && skip[depth - 2];
    skip[depth - 1] = outer || taken[depth - 1];
    return 0;
  }
  if (!strcmp(c, "fi")) {
    if (!depth)
      return fail("fi without if", 0), -1;
    depth--;
    return 0;
  }
  if (off)
    return 0;
  char v[MX_SETTING_VALUE], p[96];
  if (!strcmp(c, "exit"))
    return 1;
  if (!strcmp(c, "set") && n >= 2) {
    join(w, 2, n, v, sizeof v);
    var_set(w[1], v);
  } else if (!strcmp(c, "echo")) {
    join(w, 1, n, v, sizeof v);
    mx_printf("%s\n", v);
  } else if (!strcmp(c, "cd") && n == 2) {
    if (mx_fio_chdir(w[1]) < 0)
      return fail("cd: no such directory", w[1]), -1;
  } else if (!strcmp(c, "setting") && n >= 2) {
    join(w, 2, n, v, sizeof v);
    return set(w[1], v);
  } else if (!strcmp(c, "net") && n == 2 && !strcmp(w[1], "dhcp")) {
    return set("net.mode", "dhcp");
  } else if (!strcmp(c, "net") && n == 4) {
    return set("net.mode", "fixed") || set("net.ip", w[1]) || set("net.mask", w[2]) ||
                   set("net.gw", w[3])
               ? -1
               : 0;
  } else if (!strcmp(c, "swap") && (n == 2 || n == 3)) {
    full(w[1], p, sizeof p);
    return set("swap.path", p) || set("swap.size", n == 3 ? w[2] : "64") ? -1 : 0;
  } else if ((!strcmp(c, "shell") || !strcmp(c, "service")) && n >= 2) {
    if (!exists(w[1]))
      return fail("no such program", w[1]), -1;
    full(w[1], p, sizeof p);
    if (!strcmp(c, "shell"))
      return set("shell.path", p) || (join(w, 2, n, v, sizeof v), set("shell.args", v)) ? -1 : 0;
    char s[MX_SETTING_VALUE];
    join(w, 2, n, v, sizeof v);
    mx_snprintf(s, sizeof s, "%s%s%s", p, n > 2 ? " " : "", v);
    return set("service", s);
  } else if (!strcmp(c, "run") && n >= 2) {
    if (!exists(w[1]))
      return fail("no such program", w[1]), -1;
    return run(w, 1, n);
  } else {
    fail("not a command", c);
    return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  (void)argc, (void)argv;
  char cwd[96];
  if (mx_fio_getcwd(cwd, sizeof cwd) < 0)
    cwd[0] = 0;
  var_set("KDIR", cwd);
  var_set("?", "0");
  char rom[16] = {0};
  int fd = mx_fio_open("rom0:ROMVER", MX_FIO_RDONLY);
  if (fd >= 0) {
    int r = mx_fio_read(fd, rom, 14);
    rom[r > 0 ? r : 0] = 0;
    mx_fio_close(fd);
  }
  var_set("ROM", rom);
  char path[96];
  full("RC.SH", path, sizeof path);
  fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    mx_printf("init: no %s: nothing to do\n", path);
    return 0;
  }
  int len = mx_fio_read(fd, script, sizeof script - 1);
  mx_fio_close(fd);
  if (len < 0)
    len = 0;
  script[len] = 0;
  mx_printf("init: %s (%d bytes)\n", path, len);
  char *s = script;
  int status = 0;
  while (*s) {
    char *e = s;
    while (*e && *e != '\n')
      e++;
    char *next = *e ? e + 1 : e;
    *e = 0;
    if (e > s && e[-1] == '\r')
      e[-1] = 0;
    line_no++;
    char buf[512], *w[MAXW];
    int n = split(s, buf, sizeof buf, w);
    int r = line(w, n);
    if (r < 0) {
      status = 1;
      break;
    }
    if (r > 0)
      break;
    s = next;
  }
  if (!status && depth)
    fail("if without fi", 0), status = 1;
  mx_printf("init: done (%s)\n", status ? "stopped at an error" : "ok");
  return status;
}
