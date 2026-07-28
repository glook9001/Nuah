/*
 * ABI-identical bionic calls can enter glibc through tail-call trampolines.
 * This preserves every argument register (including varargs) and avoids
 * pretending that Android and glibc data structures are interchangeable in
 * the calls that need real translators.
 */

.text

.macro NUAH_FORWARD symbol, version=GLIBC_2.2.5
  .symver nuah_host_\symbol, \symbol@\version
  .globl \symbol
  .type \symbol, @function
\symbol:
  jmp nuah_host_\symbol@PLT
  .size \symbol, .-\symbol
.endm

NUAH_FORWARD abort
NUAH_FORWARD _exit
NUAH_FORWARD exit
NUAH_FORWARD clock
NUAH_FORWARD clock_gettime
NUAH_FORWARD difftime
NUAH_FORWARD gettimeofday
NUAH_FORWARD nanosleep
NUAH_FORWARD time
NUAH_FORWARD usleep
NUAH_FORWARD tzset

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

NUAH_FORWARD epoll_create
NUAH_FORWARD epoll_ctl
NUAH_FORWARD epoll_wait
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

.section .note.GNU-stack,"",@progbits
