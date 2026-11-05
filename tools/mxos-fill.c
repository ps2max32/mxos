/* Fills mc0:/MXOS from host:MXOS (make: $(OUT)/MXOS), and reads back what it
   wrote. Run it with ps2client from the directory above MXOS. */
#include <mx/debug.h>
#include <mx/sif.h>
#include <mx/iop.h>
#include <mx/timer.h>
static const char*const N[]={"chime","poweroff","ps2dev9","netman","smap","ps2ip-nm","udptty","ioptrap","ps2link","sio2man","mcman","mcserv","padman","libsd","audsrv"};
static unsigned char a[131072] __attribute__((aligned(64))),b[131072] __attribute__((aligned(64)));
static int end(int s){mx_printf("@@PS2TEST_END@@ status=%d\n",s);return s;}
static void cat(char*d,const char*x,const char*y,const char*z){unsigned n=0;while(*x)d[n++]=*x++;while(*y)d[n++]=*y++;while(*z)d[n++]=*z++;d[n]=0;}
int main(void){
  if(mx_sif_init()<0)return end(2);
  /* the card through what the IOP has, else through the ROM's drivers */
  int d=mx_fio_dopen("mc0:/");
  if(d<0){mx_printf("SIO2MAN %d MCMAN %d MCSERV %d\n",mx_iop_load_module("rom0:SIO2MAN"),mx_iop_load_module("rom0:MCMAN"),mx_iop_load_module("rom0:MCSERV"));
    for(int i=0;i<30;i++){d=mx_fio_dopen("mc0:/");if(d>=0)break;mx_delay_us(100000);}}
  if(d<0){mx_printf("no card: %d\n",d);return end(3);}
  mx_fio_dclose(d);
  mx_printf("mkdir: %d\n",mx_fio_mkdir("mc0:/MXOS",0));
  unsigned total=0;
  for(unsigned i=0;i<sizeof N/sizeof*N;i++){
    char s[64],t[64];cat(s,"host:MXOS/",N[i],".irx");cat(t,"mc0:/MXOS/",N[i],".irx");
    int f=mx_fio_open(s,MX_FIO_RDONLY);if(f<0){mx_printf("%s: %d\n",s,f);return end(4);}
    int g=mx_fio_open(t,MX_FIO_WRONLY|MX_FIO_CREAT|MX_FIO_TRUNC);if(g<0){mx_printf("%s: %d\n",t,g);return end(5);}
    int n=mx_fio_read(f,a,sizeof a);mx_fio_close(f);
    if(n<=0||n==(int)sizeof a){mx_printf("%s size %d\n",s,n);return end(6);}
    if(mx_fio_write(g,a,n)){mx_printf("%s write failed\n",t);return end(7);}
    mx_fio_close(g);
    g=mx_fio_open(t,MX_FIO_RDONLY);int r=mx_fio_read(g,b,n);mx_fio_close(g);
    int bad=r!=n;for(int k=0;k<n&&!bad;k++)bad=a[k]!=b[k];
    mx_printf("%s %d bytes %s\n",N[i],n,bad?"BAD":"ok");if(bad)return end(8);total+=n;
  }
  mx_printf("total %u\n",total);return end(0);
}
