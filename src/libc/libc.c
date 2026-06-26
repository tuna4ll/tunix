#include "include/tunix_libc.h"

char **t_environ;
extern int main(int argc, char **argv, char **envp);

long t_syscall0(long n) { long r; __asm__ volatile("syscall":"=a"(r):"a"(n):"rcx","r11","memory"); return r; }
long t_syscall1(long n,long a1) { long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1):"rcx","r11","memory"); return r; }
long t_syscall2(long n,long a1,long a2) { long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2):"rcx","r11","memory"); return r; }
long t_syscall3(long n,long a1,long a2,long a3) { long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2),"d"(a3):"rcx","r11","memory"); return r; }
long t_syscall4(long n,long a1,long a2,long a3,long a4) { register long r10 __asm__("r10")=a4; long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2),"d"(a3),"r"(r10):"rcx","r11","memory"); return r; }
long t_syscall5(long n,long a1,long a2,long a3,long a4,long a5) { register long r10 __asm__("r10")=a4; register long r8 __asm__("r8")=a5; long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8):"rcx","r11","memory"); return r; }
long t_syscall6(long n,long a1,long a2,long a3,long a4,long a5,long a6) { register long r10 __asm__("r10")=a4; register long r8 __asm__("r8")=a5; register long r9 __asm__("r9")=a6; long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory"); return r; }

long t_read(int fd,void *b,size_t n){return t_syscall3(0,fd,(long)b,(long)n);} long t_write(int fd,const void *b,size_t n){return t_syscall3(1,fd,(long)b,(long)n);}
int t_open(const char *p,int f,int m){return (int)t_syscall3(2,(long)p,f,m);} int t_close(int fd){return (int)t_syscall1(3,fd);} int t_poll(struct t_pollfd *f,unsigned long n,int t){return (int)t_syscall3(7,(long)f,(long)n,t);} long t_lseek(int fd,long o,int w){return t_syscall3(8,fd,o,w);}
int t_socket(int d,int t,int p){return (int)t_syscall3(41,d,t,p);} int t_socketpair(int d,int t,int p,int f[2]){return (int)t_syscall4(53,d,t,p,(long)f);} int t_bind(int f,const struct t_sockaddr_un*a,unsigned long n){return (int)t_syscall3(49,f,(long)a,(long)n);} int t_listen(int f,int b){return (int)t_syscall2(50,f,b);} int t_accept(int f){return (int)t_syscall3(43,f,0,0);} int t_connect(int f,const struct t_sockaddr_un*a,unsigned long n){return (int)t_syscall3(42,f,(long)a,(long)n);}
int t_pipe(int f[2]){return (int)t_syscall1(22,(long)f);} int t_dup2(int a,int b){return (int)t_syscall2(33,a,b);} long t_fork(void){return t_syscall0(57);}
int t_execve(const char *p,char *const a[],char *const e[]){return (int)t_syscall3(59,(long)p,(long)a,(long)e);} long t_waitpid(long p,int *s,int o){return t_syscall4(61,p,(long)s,o,0);}
void t_exit(int s){t_syscall1(60,s);for(;;)__asm__ volatile("hlt");} long t_getpid(void){return t_syscall0(39);} long t_gettid(void){return t_syscall0(186);} long t_getppid(void){return t_syscall0(110);}
int t_setpgid(long p,long g){return (int)t_syscall2(109,p,g);} long t_getpgrp(void){return t_syscall0(111);} long t_setsid(void){return t_syscall0(112);}
int t_kill(long p,int s){return (int)t_syscall2(62,p,s);}
extern void __tunix_sigreturn(void);
int t_sigaction(int s,const struct t_sigaction*a,struct t_sigaction*o){
    if(!a)return (int)t_syscall4(13,s,0,(long)o,8);
    struct t_sigaction prepared=*a;
    if(prepared.handler>1&&prepared.restorer==0)prepared.restorer=(uint64_t)(uintptr_t)__tunix_sigreturn;
    return (int)t_syscall4(13,s,(long)&prepared,(long)o,8);
}
int t_sigprocmask(int h,const uint64_t*s,uint64_t*o){return (int)t_syscall4(14,h,(long)s,(long)o,8);} int t_ioctl(int f,unsigned long r,void *a){return (int)t_syscall3(16,f,(long)r,(long)a);}
void *t_mmap(void *a,size_t n,int p,int f,int fd,uint64_t o){long r=t_syscall6(9,(long)a,(long)n,p,f,fd,(long)o);return r<0?T_MAP_FAILED:(void *)(uintptr_t)r;}
int t_munmap(void *a,size_t n){return (int)t_syscall2(11,(long)a,(long)n);} int t_mprotect(void*a,size_t n,int p){return (int)t_syscall3(10,(long)a,(long)n,p);} int t_ftruncate(int f,uint64_t n){return (int)t_syscall2(77,f,(long)n);}
int t_chdir(const char *p){return (int)t_syscall1(80,(long)p);} char *t_getcwd(char *b,size_t n){long r=t_syscall2(79,(long)b,(long)n);return r<0?0:b;}
int t_mkdir(const char *p,int m){(void)m;return (int)t_syscall2(83,(long)p,m);} int t_unlink(const char *p){return (int)t_syscall1(87,(long)p);} long t_getdents64(int f,void *b,size_t n){return t_syscall3(217,f,(long)b,(long)n);}
int t_uname(struct t_utsname *n){return (int)t_syscall1(63,(long)n);} void t_yield(void){(void)t_syscall0(24);}
int t_clock_gettime(struct t_timespec *time){return (int)t_syscall2(228,1,(long)time);}
int t_nanosleep(const struct t_timespec *request,struct t_timespec *remaining){return (int)t_syscall2(35,(long)request,(long)remaining);}
int t_futex(uint32_t *address,int operation,uint32_t value,const struct t_timespec *timeout){return (int)t_syscall6(202,(long)address,operation,value,(long)timeout,0,0);}
void t_sleep_ms(uint64_t milliseconds){struct t_timespec request={(int64_t)(milliseconds/1000ULL),(int64_t)((milliseconds%1000ULL)*1000000ULL)};while(t_nanosleep(&request,0)==-T_EINTR){}}

size_t t_strlen(const char *s){size_t n=0;if(s)while(s[n])n++;return n;} int t_strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return(unsigned char)*a-(unsigned char)*b;}
int t_strncmp(const char*a,const char*b,size_t n){while(n&&*a&&*a==*b){a++;b++;n--;}return n?((unsigned char)*a-(unsigned char)*b):0;}
char *t_strcpy(char*d,const char*s){char*r=d;while((*d++=*s++));return r;} char *t_strncpy(char*d,const char*s,size_t n){char*r=d;while(n&&*s){*d++=*s++;n--;}while(n--)*d++=0;return r;}
void *t_memcpy(void*d,const void*s,size_t n){unsigned char*o=d;const unsigned char*i=s;while(n--)*o++=*i++;return d;} void *t_memset(void*d,int v,size_t n){unsigned char*o=d;while(n--)*o++=(unsigned char)v;return d;}
char *t_strchr(const char*s,int v){for(;*s;s++)if(*s==(char)v)return(char*)s;return v==0?(char*)s:0;} const char *t_basename(const char*p){const char*r=p;for(;*p;p++)if(*p=='/')r=p+1;return r;}
char *t_getenv(const char *name){size_t n=t_strlen(name);if(!t_environ)return 0;for(char **e=t_environ;*e;e++)if(t_strncmp(*e,name,n)==0&&(*e)[n]=='=')return *e+n+1;return 0;}
void t_puts(const char*s){if(s)t_write(1,s,t_strlen(s));} void t_puterr(const char*s){if(s)t_write(2,s,t_strlen(s));}
void t_print_long(long v){char b[32];size_t n=0;unsigned long x;if(v<0){t_write(1,"-",1);x=(unsigned long)(-v);}else x=(unsigned long)v;do{b[n++]=(char)('0'+x%10);x/=10;}while(x);while(n)t_write(1,&b[--n],1);}
int t_read_retry(int fd,void*b,size_t n){for(;;){long r=t_read(fd,b,n);if(r==-T_EAGAIN){t_yield();continue;}return(int)r;}}

void __tunix_start(long argc,char **argv,char **envp){t_environ=envp;int status=main((int)argc,argv,envp);t_exit(status);}
