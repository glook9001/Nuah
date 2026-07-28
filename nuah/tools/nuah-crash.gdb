set pagination off
set confirm off
set print thread-events off
set logging file /home/pepe/Documents/sober/Investigation/captures/nuah-gdb-crash.log
set logging overwrite on
set logging redirect off
set logging enabled on

# ART, WebKit, audio and networking use these during normal operation.
handle SIGPIPE nostop noprint pass
handle SIGALRM nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGCHLD nostop noprint pass
handle SIGWINCH nostop noprint pass
handle SIG34 nostop noprint pass

catch signal SIGSEGV SIGABRT SIGBUS SIGILL SIGFPE SIGSYS
commands
  silent
  printf "\n===== NUAH FATAL SIGNAL =====\n"
  info program
  info registers
  bt 80
  thread apply all bt 8
  generate-core-file /home/pepe/Documents/sober/Investigation/captures/nuah-runtime.core
  detach
  quit
end

continue
