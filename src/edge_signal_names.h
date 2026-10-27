#ifndef EDGE_SIGNAL_NAMES_H_
#define EDGE_SIGNAL_NAMES_H_

#include <csignal>

// Returns the name of a signal number ("SIGSEGV"), or nullptr when the
// platform does not define it. Covers the same signals as os.constants.signals
// (edge_os.cc), like Node's signo_string().
inline const char* EdgeSignalName(int signum) {
  switch (signum) {
#define EDGE_SIGNAL_NAME_CASE(name) \
  case name:                        \
    return #name;
#ifdef SIGHUP
    EDGE_SIGNAL_NAME_CASE(SIGHUP)
#endif
#ifdef SIGINT
    EDGE_SIGNAL_NAME_CASE(SIGINT)
#endif
#ifdef SIGQUIT
    EDGE_SIGNAL_NAME_CASE(SIGQUIT)
#endif
#ifdef SIGILL
    EDGE_SIGNAL_NAME_CASE(SIGILL)
#endif
#ifdef SIGTRAP
    EDGE_SIGNAL_NAME_CASE(SIGTRAP)
#endif
#ifdef SIGABRT
    EDGE_SIGNAL_NAME_CASE(SIGABRT)
#endif
// SIGIOT is SIGABRT on the platforms Edge supports.
#ifdef SIGBUS
    EDGE_SIGNAL_NAME_CASE(SIGBUS)
#endif
#ifdef SIGFPE
    EDGE_SIGNAL_NAME_CASE(SIGFPE)
#endif
#ifdef SIGKILL
    EDGE_SIGNAL_NAME_CASE(SIGKILL)
#endif
#ifdef SIGUSR1
    EDGE_SIGNAL_NAME_CASE(SIGUSR1)
#endif
#ifdef SIGSEGV
    EDGE_SIGNAL_NAME_CASE(SIGSEGV)
#endif
#ifdef SIGUSR2
    EDGE_SIGNAL_NAME_CASE(SIGUSR2)
#endif
#ifdef SIGPIPE
    EDGE_SIGNAL_NAME_CASE(SIGPIPE)
#endif
#ifdef SIGALRM
    EDGE_SIGNAL_NAME_CASE(SIGALRM)
#endif
#ifdef SIGTERM
    EDGE_SIGNAL_NAME_CASE(SIGTERM)
#endif
#if defined(SIGSTKFLT) && (!defined(SIGBUS) || SIGSTKFLT != SIGBUS)
    EDGE_SIGNAL_NAME_CASE(SIGSTKFLT)
#endif
#ifdef SIGCHLD
    EDGE_SIGNAL_NAME_CASE(SIGCHLD)
#endif
#ifdef SIGCONT
    EDGE_SIGNAL_NAME_CASE(SIGCONT)
#endif
#ifdef SIGSTOP
    EDGE_SIGNAL_NAME_CASE(SIGSTOP)
#endif
#ifdef SIGTSTP
    EDGE_SIGNAL_NAME_CASE(SIGTSTP)
#endif
#ifdef SIGBREAK
    EDGE_SIGNAL_NAME_CASE(SIGBREAK)
#endif
#ifdef SIGTTIN
    EDGE_SIGNAL_NAME_CASE(SIGTTIN)
#endif
#ifdef SIGTTOU
    EDGE_SIGNAL_NAME_CASE(SIGTTOU)
#endif
#ifdef SIGURG
    EDGE_SIGNAL_NAME_CASE(SIGURG)
#endif
#ifdef SIGXCPU
    EDGE_SIGNAL_NAME_CASE(SIGXCPU)
#endif
#ifdef SIGXFSZ
    EDGE_SIGNAL_NAME_CASE(SIGXFSZ)
#endif
#ifdef SIGVTALRM
    EDGE_SIGNAL_NAME_CASE(SIGVTALRM)
#endif
#ifdef SIGPROF
    EDGE_SIGNAL_NAME_CASE(SIGPROF)
#endif
#ifdef SIGWINCH
    EDGE_SIGNAL_NAME_CASE(SIGWINCH)
#endif
// SIGPOLL is SIGIO where both exist.
#ifdef SIGIO
    EDGE_SIGNAL_NAME_CASE(SIGIO)
#elif defined(SIGPOLL)
    EDGE_SIGNAL_NAME_CASE(SIGPOLL)
#endif
#if defined(SIGLOST) && (!defined(SIGPWR) || SIGLOST != SIGPWR)
    EDGE_SIGNAL_NAME_CASE(SIGLOST)
#endif
#if defined(SIGPWR) && (!defined(SIGINFO) || SIGPWR != SIGINFO)
    EDGE_SIGNAL_NAME_CASE(SIGPWR)
#endif
#ifdef SIGINFO
    EDGE_SIGNAL_NAME_CASE(SIGINFO)
#endif
#ifdef SIGSYS
    EDGE_SIGNAL_NAME_CASE(SIGSYS)
#endif
#undef EDGE_SIGNAL_NAME_CASE
    default:
      return nullptr;
  }
}

#endif  // EDGE_SIGNAL_NAMES_H_
