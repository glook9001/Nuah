/* ABI-identical bionic calls enter glibc via tail-call trampolines; translators handle the rest. */
.text
.macro NUAH_FORWARD symbol, version=GLIBC_2.2.5
  .symver nuah_host_\symbol, \symbol@\version
  /* Translated bionic.cpp wins; weak tail-call is the ABI-identical fallback. */
  .weak \symbol; .type \symbol, @function
\symbol: jmp nuah_host_\symbol@PLT; .size \symbol, .-\symbol
.endm
.macro NUAH_FWD2 a, b
  NUAH_FORWARD \a; NUAH_FORWARD \b
.endm
.macro NUAH_FWD3 a, b, c
  NUAH_FORWARD \a; NUAH_FORWARD \b; NUAH_FORWARD \c
.endm
.macro NUAH_FWD4 a, b, c, d
  NUAH_FORWARD \a; NUAH_FORWARD \b; NUAH_FORWARD \c; NUAH_FORWARD \d
.endm
.macro NUAH_FWD5 a, b, c, d, e
  NUAH_FORWARD \a; NUAH_FORWARD \b; NUAH_FORWARD \c; NUAH_FORWARD \d; NUAH_FORWARD \e
.endm
.macro NUAH_FWD2V version, a, b
  NUAH_FORWARD \a, \version; NUAH_FORWARD \b, \version
.endm
.macro NUAH_FWD3V version, a, b, c
  NUAH_FORWARD \a, \version; NUAH_FORWARD \b, \version; NUAH_FORWARD \c, \version
.endm
.macro NUAH_FWD5V version, a, b, c, d, e
  NUAH_FORWARD \a, \version; NUAH_FORWARD \b, \version; NUAH_FORWARD \c, \version; NUAH_FORWARD \d, \version; NUAH_FORWARD \e, \version
.endm
/* abort lives in bionic.cpp so diagnostics keep the Android caller. */
NUAH_FWD4 _exit, exit, clock, clock_gettime; NUAH_FWD4 difftime, gettimeofday, nanosleep, time; NUAH_FWD4 usleep, gmtime, gmtime_r, localtime; NUAH_FWD3 localtime_r, mktime, strftime
NUAH_FWD4 getegid, geteuid, getgid, getpagesize; NUAH_FWD4 getpid, getppid, getpriority, getuid; NUAH_FWD3 setpriority, sysconf, waitpid; NUAH_FORWARD getentropy, GLIBC_2.25; NUAH_FWD2 signal, sigaltstack
/* Loader leaves: dlsym impl would recurse resolving itself. */
NUAH_FWD5 memcpy, memmove, memset, memcmp, memchr; NUAH_FWD5 strlen, strnlen, strcmp, strncmp, strcpy; NUAH_FWD4 strncpy, strcat, strncat, strchr; NUAH_FWD4 strrchr, strstr, strcasecmp, strncasecmp
NUAH_FWD5 accept, bind, connect, freeaddrinfo, gai_strerror; NUAH_FWD5 getaddrinfo, gethostname, getnameinfo, getpeername, getsockname; NUAH_FWD5 getsockopt, if_indextoname, if_nametoindex, inet_ntop, inet_pton; NUAH_FWD5 listen, recvfrom, recvmsg, select, sendmsg
NUAH_FWD5 sendto, setsockopt, shutdown, socket, socketpair; NUAH_FWD3V GLIBC_2.3.2, epoll_create, epoll_ctl, epoll_wait; NUAH_FWD5 ioctl, mlock, msync, pipe, poll; NUAH_FWD5 posix_fallocate, prctl, ptrace, raise, rename
NUAH_FWD5 rmdir, sched_get_priority_max, sched_get_priority_min, sched_getparam, sched_getscheduler; NUAH_FWD5 sched_setscheduler, sched_yield, syscall, sysinfo, uname; NUAH_FWD2 utime, utimes; NUAH_FWD5 atof, atoi, atol, atoll, bsearch; NUAH_FWD5 mbtowc, mbrlen, mbrtowc, mbsnrtowcs, mbsrtowcs
/* String/numeric+stdio use host-owned FILE/DIR handles. */
NUAH_FWD5 memrchr, qsort, rand, random, snprintf; NUAH_FWD5 srand, sscanf, strcspn, strpbrk, strspn; NUAH_FWD5 strtod, strtof, strtol, strtoll, strtoul; NUAH_FWD5 strtoull, vasprintf, vsnprintf, vsscanf, wcslen
NUAH_FWD5 wmemchr, wmemcmp, wcrtomb, wcsnrtombs, btowc; NUAH_FWD5 execv, execve, fork, getcwd, getenv; NUAH_FWD5 gethostbyname, isspace, localeconv, pread64, statvfs; NUAH_FWD4 tcgetattr, tcsetattr, tolower, wctob
NUAH_FWD5 clearerr, closedir, closelog, fclose, fdopen; NUAH_FWD5 feof, ferror, fflush, fgets, fileno; NUAH_FWD5 fopen, fprintf, fputc, fputs, fputwc; NUAH_FWD5 fread, fscanf, fseek, fseeko, ftell
NUAH_FWD5 ftello, fwrite, getc, getwc, openlog; NUAH_FWD5 opendir, printf, puts, readdir, setvbuf; NUAH_FWD5 syslog, ungetc, ungetwc, vfprintf, vprintf
NUAH_FORWARD accept4, GLIBC_2.10; NUAH_FORWARD epoll_create1, GLIBC_2.9; NUAH_FORWARD eventfd, GLIBC_2.7; NUAH_FORWARD getauxval, GLIBC_2.16; NUAH_FORWARD gettid, GLIBC_2.30; NUAH_FORWARD mremap, GLIBC_2.2.5; NUAH_FORWARD recvmmsg, GLIBC_2.12; NUAH_FORWARD sched_getcpu, GLIBC_2.6
NUAH_FORWARD sendmmsg, GLIBC_2.14; NUAH_FWD2V GLIBC_2.8, timerfd_create, timerfd_settime; NUAH_FORWARD arc4random_buf, GLIBC_2.36; NUAH_FORWARD realpath, GLIBC_2.3
/* Opaque locale handles stay host-owned. */
NUAH_FWD5V GLIBC_2.3, freelocale, iswalpha_l, iswblank_l, iswcntrl_l, iswdigit_l; NUAH_FWD5V GLIBC_2.3, iswlower_l, iswprint_l, iswpunct_l, iswspace_l, iswupper_l; NUAH_FWD5V GLIBC_2.3, iswxdigit_l, newlocale, strcoll_l, strftime_l, strtold_l; NUAH_FWD2V GLIBC_2.3.3, strtoll_l, strtoull_l; NUAH_FWD5V GLIBC_2.3, strxfrm_l, towlower_l, towupper_l, uselocale, wcscoll_l; NUAH_FORWARD wcsxfrm_l, GLIBC_2.3
.section .note.GNU-stack,"",@progbits
