#ifndef _DATY_INC
#define _DATY_INC

//#define _XOPEN_SOURCE 501

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// these should be enough most of the time
#define ESC "\033"
#define TERM_RESET "c"
#define TERM_SMCUP "[?1049h"
#define TERM_RMCUP "[?1049l"

extern char const* errfile;
extern unsigned int errline;
extern char const* errmsg;
extern int errdid;
extern void _die();

#define die(__msg) do {  \
    errfile = __FILE__;  \
    errline = __LINE__;  \
    errmsg = __msg;      \
    errdid = errno;      \
    _die();              \
} while (1)
#define try(_v, _r) do {      \
    _v = _r;                  \
    if (_v < 0) {             \
        errfile = __FILE__;   \
        errline = __LINE__;   \
        errmsg = #_r;         \
        errdid = errno;       \
        goto finally;         \
    }                         \
} while (0)

#define BUF_SIZE 65535

#define sig_handle(_sign, _fn, _flags) do {  \
  struct sigaction sa;                       \
  sa.sa_handler = _fn;                       \
  sigemptyset(&sa.sa_mask);                  \
  sa.sa_flags = _flags;                      \
  int r;                                     \
  try(r, sigaction(_sign, &sa, NULL));       \
} while (0)

#endif // _DATY_INC