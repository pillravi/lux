/*
 * Copyright 2012-2017 Tail-f Systems AB
 *
 * See the file "LICENSE" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * Run a program in the slave end of a pseudo tty. Used to run our
 * interactive tests with.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif
#ifdef __NetBSD__
#define _NETBSD_SOURCE /* needed for pty funcs */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/errno.h>

#ifdef DEBUG
#define DBG(...) dprintf(dbgfd, __VA_ARGS__)
#else
#define DBG(...)
#endif

#define PRINT(reason)                                                   \
    fprintf(stderr, "\nrunpty: %s: %s\n", reason, strerror(errno));     \
    DBG("\n\n%s: %sy\n", reason, strerror(errno))                       \

#define EXIT(reason)                                                    \
    {                                                                   \
        PRINT(reason);                                                  \
        exit(1);                                                        \
    }

#define QUIT(reason)                                                    \
    {                                                                   \
        PRINT(reason);                                                  \
        goto quit;                                                      \
    }

#ifdef __sun__
#include <stropts.h>
#define NEED_STREAMS
#endif
#ifdef USE_OPENPTY
#include <util.h>
#endif

static int quit = 0;
void sighdlr(int sig) {
    switch (sig) {
    case SIGCHLD:
        break;
    default:
        quit++;
        break;
    }
    return;
}

#ifdef USE_OPENPTY
static char *openmaster(int *master, int *slave)
{
    int m, s;
    static char path[1024];
    if (openpty(&m, &s, path, NULL, NULL) < 0) return NULL;
    *master = m;
    *slave = s;
    return path;
}
#else
static char *openmaster(int *master, int *slave)
{
    int m;
    char *path;
    if ((m = posix_openpt(O_RDWR | O_NOCTTY)) < 0) return NULL;
    if (grantpt(m) < 0) goto fail;
    if (unlockpt(m) < 0) goto fail;
    if ((path = ptsname(m)) == NULL) goto fail;
    *master = m;
    *slave = -1;
    return path;
fail:
    close(m);
    return NULL;
}
#endif

int prev_fd_flags, prev_outfd_flags;

static void set_nonblocking(int fd, int outfd)
{
    /* configure socket for non-blocking io */
    prev_fd_flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, prev_fd_flags | O_NONBLOCK);

    prev_outfd_flags = fcntl(outfd, F_GETFL, 0);
    fcntl(outfd, F_SETFL, prev_outfd_flags | O_NONBLOCK);
}

void restore_blocking(int fd, int outfd)
{
    /* restore blocking io */
    fcntl(fd, F_SETFL, prev_fd_flags);
    fcntl(outfd, F_SETFL, prev_outfd_flags);
}

int main(int argc, char *argv[])
{
    int in = fileno(stdin);
    int out = fileno(stdout);
    int inr = 0;
    int inw = 0;
    int outr = 0;
    int outw = 0;
    fd_set readfdset;
    fd_set writefdset;
    char inbuf[BUFSIZ*4];  /* *4 appears to fix bug on mac os x */
    char outbuf[BUFSIZ*4]; /* *4 appears to fix bug on mac os x */
    int master;
    int slave;
    char *slavepath;
    pid_t child;
    int status;
    int incnt = 0;
    int outcnt = 0;

#ifdef DEBUG
    int dbgfd = open("runpty.dbg", O_WRONLY|O_CREAT);
    if (dbgfd < 0 ) {
        perror("open runpty.dbg failed");
        exit(1);
    }
#endif
    if (argc < 2) {
        exit(1);
    }

    if ((slavepath = openmaster(&master, &slave)) == NULL) {
        EXIT("failed to open pty");
    }

    /* QNX requires euid 0 to get a pty - get rid of any setuid-ness now */
    if (setuid(getuid()))
        ; /* Ignore return value */

    if ((child = fork()) < 0) {
        EXIT("fork failed");
    }

    if (child == 0) {
        /* child */
        close(master);
        if (setsid() < 0) {
            EXIT("child failed to setsid()");
        }
        if (slave < 0 && (slave = open(slavepath, O_RDWR)) < 0) {
            EXIT("open slave pty in child failed");
        }
#ifdef NEED_STREAMS
        if (ioctl(slave, I_FIND, "ptem") == 0 &&
            ioctl(slave, I_PUSH, "ptem") < 0) {
            EXIT("failed to push STREAMS module 'ptem'");
        }
        if (ioctl(slave, I_FIND, "ldterm") == 0 &&
            ioctl(slave, I_PUSH, "ldterm") < 0) {
            EXIT("failed to push STREAMS module 'ldterm'");
        }
#endif
#if defined(TIOCSCTTY) && !defined(__QNX__)
        if (ioctl(slave, TIOCSCTTY, NULL) < 0) {
            EXIT("TIOCSCTTY failed in child");
        }
#endif
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        execvp(argv[1], argv+1);
        EXIT("exec in child failed");
    }

    if (slave >= 0) {
        close(slave);
    }

    set_nonblocking(in, master);

    signal(SIGINT, sighdlr);
    signal(SIGTERM, sighdlr);

    for (;;) {
        FD_ZERO(&readfdset);
        FD_ZERO(&writefdset);

        if (inw == inr) {
            DBG("IN(%d):  Waiting for more\n", incnt);
            FD_SET(in, &readfdset);
        } else {
            DBG("IN(%d):  Written %d of %d bytes, waiting for %d more\n",
                incnt, inw, inr, inr-inw);
            FD_SET(master, &writefdset);
        }
        if (outw == outr) {
            DBG("OUT(%d): Waiting for more\n", outcnt);
            FD_SET(master, &readfdset);
        } else {
            DBG("OUT(%d): Written %d of %d bytes, waiting for %d more\n",
                outcnt, outw, outr, outr-outw);
            FD_SET(out, &writefdset);
        }

        if (select(master+1, &readfdset, &writefdset, NULL, NULL) < 0) {
            if (quit) QUIT("select");
        }

        if (FD_ISSET(in, &readfdset)) {
            incnt++;
            inr = read(in, inbuf, sizeof(inbuf));
            DBG("IN(%d):  Read    %d bytes", incnt, inr);
            if (inr < 1) QUIT("read in");
            inw = write(master, inbuf, inr);
            DBG(", wrote %d bytes\n", inw);
            if (inw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) inw = 0;
            if (inw < 0) QUIT("write in");
        }
        if (FD_ISSET(master, &writefdset)) {
            int left = inr-inw;
            int w = write(master, inbuf+inw, left);
            DBG("IN(%d):  Wrote   %d of %d bytes", incnt, w, left);
            if (w < 1) QUIT("write in");
            inw += w;
            DBG(", in total %d of %d\n", inw, inr);
        }
        if (FD_ISSET(master, &readfdset)) {
            outcnt++;
            outr = read(master, outbuf, sizeof(outbuf));
            DBG("OUT(%d): Read    %d bytes", outcnt, outr);
            if (outr < 1) QUIT("read out");
            outw = write(out, outbuf, outr);
            DBG(", wrote %d bytes\n", outw);
            if (outw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) outw = 0;
            if (outw < 0) QUIT("write out");
    }
        if (FD_ISSET(out, &writefdset)) {
            int left = outr-outw;
            int w = write(out, outbuf+outw, left);
            DBG("OUT(%d): Wrote   %d of %d bytes", outcnt, w, left);
            if (w < 1) QUIT("write out");
            outw += w;
            DBG(", in total %d of %d\n", outw, outr);
        }
    }
quit:
#ifdef DEBUG
    close(dbgfd);
#endif
    close(master);

    if (waitpid(child, &status, WNOHANG) == 0) {
        /* Child hasn't terminated, give it some time and if it still
         * hasn't quit, kill it*/
        struct timeval tv;
        tv.tv_sec = 5; tv.tv_usec = 0;
        signal(SIGCHLD, sighdlr);
        /* Wait for SIGCHLD or timeout */
        if (select(0, NULL, NULL, NULL, &tv) == 0) {
            kill(child, SIGKILL);
        }
        waitpid(child, &status, 0);
    }

    restore_blocking(in, master);

    if (WIFEXITED(status)) {
        fprintf(stderr, "Child exited with status %d\n", WEXITSTATUS(status));
        exit(WEXITSTATUS(status));
    }

    if (WIFSIGNALED(status))
        fprintf(stderr, "Child terminated by signal %d\n", WTERMSIG(status));

    exit(1);
}
