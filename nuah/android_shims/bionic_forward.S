/*
 * ABI-identical bionic calls can enter glibc through tail-call trampolines.
 * This preserves every argument register (including varargs) and avoids
 * pretending that Android and glibc data structures are interchangeable in
 * the calls that need real translators.
 */

.text

.macro NUAH_FORWARD symbol, version=GLIBC_2.2.5
  .symver nuah_host_\symbol, \symbol@\version
  /*
   * A translated implementation in bionic.cpp always wins.  The weak
   * tail-call is the complete, reviewed ABI-identical fallback set; this
   * avoids growing a symbol-name chain in the loader after every smoke run.
   */
  .weak \symbol
  .type \symbol, @function
\symbol:
  jmp nuah_host_\symbol@PLT
  .size \symbol, .-\symbol
.endm

/* abort is implemented by bionic.cpp so constructor diagnostics retain the
 * Android caller before control reaches glibc. */
NUAH_FORWARD _exit
NUAH_FORWARD exit
NUAH_FORWARD clock
NUAH_FORWARD clock_gettime
NUAH_FORWARD difftime
NUAH_FORWARD gettimeofday
NUAH_FORWARD nanosleep
NUAH_FORWARD time
NUAH_FORWARD usleep
NUAH_FORWARD gmtime
NUAH_FORWARD gmtime_r
NUAH_FORWARD localtime
NUAH_FORWARD localtime_r
NUAH_FORWARD mktime
NUAH_FORWARD strftime

NUAH_FORWARD getegid
NUAH_FORWARD geteuid
NUAH_FORWARD getgid
NUAH_FORWARD getpagesize
NUAH_FORWARD getpid
NUAH_FORWARD getppid
NUAH_FORWARD getpriority
NUAH_FORWARD getuid
NUAH_FORWARD setpriority
NUAH_FORWARD sysconf
NUAH_FORWARD waitpid
NUAH_FORWARD getentropy, GLIBC_2.25
NUAH_FORWARD signal
NUAH_FORWARD sigaltstack

NUAH_FORWARD accept
NUAH_FORWARD bind
NUAH_FORWARD connect
NUAH_FORWARD freeaddrinfo
NUAH_FORWARD gai_strerror
NUAH_FORWARD getaddrinfo
NUAH_FORWARD gethostname
NUAH_FORWARD getnameinfo
NUAH_FORWARD getpeername
NUAH_FORWARD getsockname
NUAH_FORWARD getsockopt
NUAH_FORWARD if_indextoname
NUAH_FORWARD if_nametoindex
NUAH_FORWARD inet_ntop
NUAH_FORWARD inet_pton
NUAH_FORWARD listen
NUAH_FORWARD recvfrom
NUAH_FORWARD recvmsg
NUAH_FORWARD select
NUAH_FORWARD sendmsg
NUAH_FORWARD sendto
NUAH_FORWARD setsockopt
NUAH_FORWARD shutdown
NUAH_FORWARD socket
NUAH_FORWARD socketpair

NUAH_FORWARD epoll_create, GLIBC_2.3.2
NUAH_FORWARD epoll_ctl, GLIBC_2.3.2
NUAH_FORWARD epoll_wait, GLIBC_2.3.2
NUAH_FORWARD ioctl
NUAH_FORWARD madvise
NUAH_FORWARD mlock
NUAH_FORWARD msync
NUAH_FORWARD pipe
NUAH_FORWARD poll
NUAH_FORWARD posix_fallocate
NUAH_FORWARD prctl
NUAH_FORWARD ptrace
NUAH_FORWARD raise
NUAH_FORWARD rename
NUAH_FORWARD rmdir
NUAH_FORWARD sched_get_priority_max
NUAH_FORWARD sched_get_priority_min
NUAH_FORWARD sched_getparam
NUAH_FORWARD sched_getscheduler
NUAH_FORWARD sched_setscheduler
NUAH_FORWARD sched_yield
NUAH_FORWARD syscall
NUAH_FORWARD sysinfo
NUAH_FORWARD uname
NUAH_FORWARD utime
NUAH_FORWARD utimes

/* String/numeric and stdio calls use host-owned opaque FILE/DIR handles. */
NUAH_FORWARD atof
NUAH_FORWARD atoi
NUAH_FORWARD atol
NUAH_FORWARD atoll
NUAH_FORWARD bsearch
NUAH_FORWARD mbtowc
NUAH_FORWARD mbrlen
NUAH_FORWARD mbrtowc
NUAH_FORWARD mbsnrtowcs
NUAH_FORWARD mbsrtowcs
NUAH_FORWARD memrchr
NUAH_FORWARD qsort
NUAH_FORWARD rand
NUAH_FORWARD random
NUAH_FORWARD snprintf
NUAH_FORWARD srand
NUAH_FORWARD sscanf
NUAH_FORWARD strcspn
NUAH_FORWARD strpbrk
NUAH_FORWARD strspn
NUAH_FORWARD strtod
NUAH_FORWARD strtof
NUAH_FORWARD strtol
NUAH_FORWARD strtoll
NUAH_FORWARD strtoul
NUAH_FORWARD strtoull
NUAH_FORWARD vasprintf
NUAH_FORWARD vsnprintf
NUAH_FORWARD vsscanf
NUAH_FORWARD wcslen
NUAH_FORWARD wmemchr
NUAH_FORWARD wmemcmp
NUAH_FORWARD wcrtomb
NUAH_FORWARD wcsnrtombs
NUAH_FORWARD btowc
NUAH_FORWARD execv
NUAH_FORWARD execve
NUAH_FORWARD fork
NUAH_FORWARD getcwd
NUAH_FORWARD getenv
NUAH_FORWARD gethostbyname
NUAH_FORWARD isspace
NUAH_FORWARD localeconv
NUAH_FORWARD pread64
NUAH_FORWARD statvfs
NUAH_FORWARD tcgetattr
NUAH_FORWARD tcsetattr
NUAH_FORWARD tolower
NUAH_FORWARD wctob

NUAH_FORWARD clearerr
NUAH_FORWARD closedir
NUAH_FORWARD closelog
NUAH_FORWARD fclose
NUAH_FORWARD fdopen
NUAH_FORWARD feof
NUAH_FORWARD ferror
NUAH_FORWARD fflush
NUAH_FORWARD fgets
NUAH_FORWARD fileno
NUAH_FORWARD fopen
NUAH_FORWARD fprintf
NUAH_FORWARD fputc
NUAH_FORWARD fputs
NUAH_FORWARD fputwc
NUAH_FORWARD fread
NUAH_FORWARD fscanf
NUAH_FORWARD fseek
NUAH_FORWARD fseeko
NUAH_FORWARD ftell
NUAH_FORWARD ftello
NUAH_FORWARD fwrite
NUAH_FORWARD getc
NUAH_FORWARD getwc
NUAH_FORWARD openlog
NUAH_FORWARD opendir
NUAH_FORWARD printf
NUAH_FORWARD puts
NUAH_FORWARD readdir
NUAH_FORWARD setvbuf
NUAH_FORWARD syslog
NUAH_FORWARD ungetc
NUAH_FORWARD ungetwc
NUAH_FORWARD vfprintf
NUAH_FORWARD vprintf

NUAH_FORWARD accept4, GLIBC_2.10
NUAH_FORWARD epoll_create1, GLIBC_2.9
NUAH_FORWARD eventfd, GLIBC_2.7
NUAH_FORWARD getauxval, GLIBC_2.16
NUAH_FORWARD gettid, GLIBC_2.30
NUAH_FORWARD mremap, GLIBC_2.2.5
NUAH_FORWARD recvmmsg, GLIBC_2.12
NUAH_FORWARD sched_getcpu, GLIBC_2.6
NUAH_FORWARD sendmmsg, GLIBC_2.14
NUAH_FORWARD timerfd_create, GLIBC_2.8
NUAH_FORWARD timerfd_settime, GLIBC_2.8
NUAH_FORWARD arc4random_buf, GLIBC_2.36
NUAH_FORWARD realpath, GLIBC_2.3

/* Opaque locale handles originate in these host calls and stay host-owned. */
NUAH_FORWARD freelocale, GLIBC_2.3
NUAH_FORWARD iswalpha_l, GLIBC_2.3
NUAH_FORWARD iswblank_l, GLIBC_2.3
NUAH_FORWARD iswcntrl_l, GLIBC_2.3
NUAH_FORWARD iswdigit_l, GLIBC_2.3
NUAH_FORWARD iswlower_l, GLIBC_2.3
NUAH_FORWARD iswprint_l, GLIBC_2.3
NUAH_FORWARD iswpunct_l, GLIBC_2.3
NUAH_FORWARD iswspace_l, GLIBC_2.3
NUAH_FORWARD iswupper_l, GLIBC_2.3
NUAH_FORWARD iswxdigit_l, GLIBC_2.3
NUAH_FORWARD newlocale, GLIBC_2.3
NUAH_FORWARD strcoll_l, GLIBC_2.3
NUAH_FORWARD strftime_l, GLIBC_2.3
NUAH_FORWARD strtold_l, GLIBC_2.3
NUAH_FORWARD strtoll_l, GLIBC_2.3.3
NUAH_FORWARD strtoull_l, GLIBC_2.3.3
NUAH_FORWARD strxfrm_l, GLIBC_2.3
NUAH_FORWARD towlower_l, GLIBC_2.3
NUAH_FORWARD towupper_l, GLIBC_2.3
NUAH_FORWARD uselocale, GLIBC_2.3
NUAH_FORWARD wcscoll_l, GLIBC_2.3
NUAH_FORWARD wcsxfrm_l, GLIBC_2.3

.section .note.GNU-stack,"",@progbits
