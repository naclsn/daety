#include "server.h"

/// create and bind a local socket; exits on failure
static int bind_local_socket(char const* filename, int listen_n) {
  int sock = -1;
  struct sockaddr_un addr = {.sun_family= AF_LOCAL};

  try(sock, socket(PF_LOCAL, SOCK_STREAM, 0));

  strncpy(addr.sun_path, filename, sizeof addr.sun_path);

  int r;
  try(r, bind(sock, (struct sockaddr*)&addr, SUN_LEN(&addr)));
  try(r, listen(sock, listen_n));

  return sock;
finally:
  close(sock);
  _die();
  return -1;
}

#define IDX_SOCK 0
#define IDX_TERM 1
#define IDX_CLIS 2
#define IDX_COUNT 8

static char const* crap_name = NULL; // ZZZ: crap (storing argument pointer into static...)
static struct pollfd fds[IDX_COUNT];
static int fds_count = 0;
static pid_t cpid = 0;
static struct winsize wss[IDX_COUNT-IDX_CLIS];
static int curr_ws = -1;
static bool is_alt = false;

/// cleanup SIGINT handler
static void cleanup(int sign) {
  #define lastsay(_c) write(STDOUT_FILENO, "server: " _c "\n", strlen("server: " _c "\n"));

  lastsay("cleaning");
  if (crap_name) unlink(crap_name);

  if (sign) {
    lastsay("terminating program (1s)");
    kill(cpid, SIGTERM);
    sleep(1);
  }

  // get exit code
  int wst;
  char code = 0;
  if (0 == waitpid(cpid, &wst, WNOHANG)) {
    lastsay("program is not stopping.. waiting");
    kill(cpid, SIGTERM);
    sleep(3);
    // try again before escalating
    if (0 == waitpid(cpid, &wst, WNOHANG)) {
      lastsay("program still not stopping, killing");
      kill(cpid, SIGKILL);
      waitpid(cpid, &wst, 0);
    }
  }
  if (WIFEXITED(wst)) code = WEXITSTATUS(wst);

  lastsay("closing connections")
  for (int k = 0; k < fds_count; k++) {
    // send exit code to client
    write(fds[k].fd, &code, 1);
    close(fds[k].fd);
  }

  lastsay("done");
  if (sign) _exit(0);
}

/// fork and start program on a new pty
static pid_t fork_program(char** args) {
  pid_t cpid;
  try(cpid, forkpty(&fds[IDX_TERM].fd, NULL, NULL, NULL));
  fds_count++;

  // parent (server)
  if (0 < cpid) return cpid;

  // child (program)
  try(cpid, execvp(args[0], args)); // TODO: catch and report err

finally:
  _die();
  return -1;
}

/// output using ^x sequences
static void putesc(char const* buf, int len) {
  do {
    if (*buf < 0x20) {
      putchar('^');
      putchar(*buf | 0x40);
    } else putchar(*buf);
    buf++;
  } while (--len);
}

void server(char const* name, char** args, bool verbose, bool quiet) {
  int r;
  cpid = fork_program(args);

  sig_handle(SIGINT, cleanup, SA_RESETHAND);
  sig_handle(SIGTERM, cleanup, SA_RESETHAND);

  fds[IDX_TERM].events = POLLIN;

  crap_name = name;
  fds[IDX_SOCK].fd = bind_local_socket(name, IDX_COUNT-IDX_CLIS);
  fds[IDX_SOCK].events = POLLIN;
  fds_count = IDX_CLIS;

  if (!quiet) puts("server: listening");

  char buf[BUF_SIZE];
  int len;
  while (1) {
    int n;
    try(n, poll(fds, fds_count, -1));

    // notification from a client
    for (int i = IDX_CLIS; i < fds_count; i++) {
      bool remove = POLLHUP & fds[i].revents;

      // client got input
      if (!remove && POLLIN & fds[i].revents) {
        try(len, read(fds[i].fd, buf, BUF_SIZE));
        if (!quiet) {
          printf("<%d> (%dB) ", fds[i].fd, len);
          if (verbose) putesc(buf, len);
          putchar('\n');
        }
        remove = 0 == len;

        // program input
        if (0 != len) try(r, write(fds[IDX_TERM].fd, buf, len));
      }

      // client was closed
      if (remove) {
        if (!quiet) printf("server: -%d\n", fds[i].fd);
        close(fds[i].fd);

        fds_count--;
        for (int j = i; j < fds_count; j++) {
          fds[j] = fds[j+1];
          wss[j-IDX_CLIS] = wss[j-IDX_CLIS+1];
        }

        if (i-IDX_CLIS == curr_ws) {
          if (IDX_CLIS < fds_count) {
            // find new smaller size
            curr_ws = 0;
            for (int k = 1; k < fds_count-IDX_CLIS; k++) {
              if (wss[k].ws_col < wss[curr_ws].ws_col && wss[k].ws_row < wss[curr_ws].ws_row)
                curr_ws = k;
            }
            if (!quiet) printf("server: new size %dx%d\n", wss[curr_ws].ws_col, wss[curr_ws].ws_row);
            try(r, ioctl(fds[IDX_TERM].fd, TIOCSWINSZ, wss+curr_ws));
          } else {
            // use a 'standard' 80x24
            curr_ws = -1;
            struct winsize ws = {.ws_col= 80, .ws_row= 24};
            if (!quiet) printf("server: 'standard' size %dx%d\n", ws.ws_col, ws.ws_row);
            try(r, ioctl(fds[IDX_TERM].fd, TIOCSWINSZ, &ws));
          }
        }
      }
    } // for (clients)

    // progam is done
    if (POLLHUP & fds[IDX_TERM].revents) {
      if (!quiet) puts("server: program done");
      break;
    }

    // program output (only if there are clients listening)
    if (POLLIN & fds[IDX_TERM].revents) {
      try(len, read(fds[IDX_TERM].fd, buf, BUF_SIZE));
      if (!quiet) {
        printf("<prog> (%dB) ", len);
        if (verbose) putesc(buf, len);
        putchar('\n');
      }

      // progam is done (eof)
      if (0 == len) {
        if (!quiet) puts("server: program done (eof)");
        break;
      }

      // echo back to every clients
      for (int j = IDX_CLIS; j < fds_count; j++)
        try(r, write(fds[j].fd, buf, len));

      // scan for enter/leave alt
      char const* found = buf-1;
      int rest = len;
      while (0 < rest && NULL != (found = memchr(found+1, *ESC, rest))) {
        rest-= found-buf;
        if (!is_alt && 0 == memcmp(TERM_SMCUP, found+1, strlen(TERM_SMCUP))) {
          if (!quiet) puts("server: entering alt");
          is_alt = true;
        } else if (is_alt && 0 == memcmp(TERM_RMCUP, found+1, strlen(TERM_RMCUP))) {
          if (!quiet) puts("server: leaving alt");
          is_alt = false;
        }
      }
    }

    // new incoming connection
    if (POLLIN & fds[IDX_SOCK].revents) {
      struct pollfd* pfd = fds+fds_count;
      struct winsize* ws = wss+fds_count-IDX_CLIS;

      try(pfd->fd, accept(fds[IDX_SOCK].fd, NULL, NULL));
      if (!quiet) printf("server: +%d\n", pfd->fd);

      // receive new size to adopt
      try(len, recv(pfd->fd, ws, sizeof *ws, MSG_WAITALL));

      // sync or abort and close on failure
      if (sizeof *ws == len) {
        if (!quiet) printf("server: %dx%d\n", ws->ws_col, ws->ws_row);
        if (-1 == curr_ws || (ws->ws_col < wss[curr_ws].ws_col && ws->ws_row < wss[curr_ws].ws_row)) {
          if (!quiet) printf("server: new size %dx%d\n", ws->ws_col, ws->ws_row);
          curr_ws = fds_count-IDX_CLIS;
          try(r, ioctl(fds[IDX_TERM].fd, TIOCSWINSZ, ws));
        }

        // enter alt screen if needed (and other term states that i dont know of)
        if (is_alt) {
          try(r, write(pfd->fd, ESC TERM_SMCUP, strlen(ESC TERM_SMCUP)));
        }

        pfd->events = POLLIN;
        fds_count++;
      } else {
        if (!quiet) puts("server: aborting connection");
        close(pfd->fd);
      }
    }
  } // while (poll)

finally:
  cleanup(0);
  if (errdid) _die();
}