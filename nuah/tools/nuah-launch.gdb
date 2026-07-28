set pagination off
set confirm off
set print thread-events off
set debuginfod enabled off
set follow-fork-mode parent
set detach-on-fork on
set logging file /home/pepe/Documents/sober/Investigation/captures/nuah-gdb-live.log
set logging overwrite on
set logging redirect off
set logging enabled on

handle SIGPIPE nostop noprint pass
handle SIGALRM nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGCHLD nostop noprint pass
handle SIGWINCH nostop noprint pass
handle SIG34 nostop noprint pass
handle SIGSEGV nostop noprint pass

# ART uses SIGSEGV for managed null checks, so intercepting it makes startup
# unusably slow. Linux/systemd preserves a core if its signal chain cannot
# recover. Keep GDB focused on uncommon native faults and explicit aborts.
catch signal SIGBUS SIGILL SIGFPE SIGSYS
commands
  silent
  printf "\n===== NUAH NATIVE SIGNAL =====\n"
  info program
  bt 60
  info registers
  x/24i $pc-32
  continue
end

# An abort is terminal. Its focused stack plus short per-thread stacks are
# enough to identify the owner without freezing for minutes.
catch signal SIGABRT
commands
  silent
  printf "\n===== NUAH ABORT =====\n"
  info program
  bt 80
  info registers
  thread apply all bt 8
  quit
end

run
