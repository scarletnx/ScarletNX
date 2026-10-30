/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#include "nx/runtime/gdbstub.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
int snx_gdb_start(int stop, int port) {
    (void)stop;
    (void)port;
    fputs("scarletnx: the GDB stub is UNIX only\n", stderr);
    return 1;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

/* The lifetime pipe closes even on _exit/crash, so neither the listener nor
   gdbserver can outlive the game. No network access beyond loopback is exposed. */
static int lifetime = -1;
static int relay_write(int out, int life, const char* data, size_t size) {
    struct pollfd fds[2];
    fds[0].fd = life;
    fds[0].events = POLLIN;
    fds[1].fd = out;
    fds[1].events = POLLOUT;
    while (size) {
        ssize_t n;
        int result = poll(fds, 2, -1);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || fds[0].revents || (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
        n = write(out, data, size);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return -1;
        data += n;
        size -= (size_t)n;
    }
    return 0;
}
static void monitor(int listener, int life, int gate, pid_t target, const char* server) {
    struct pollfd fds[2];
    pid_t child = -1;
    int peer = -1, status, transport[2] = {-1, -1};
    char ready, pid[32];
    signal(SIGPIPE, SIG_IGN);
    /* Parent first authorizes ptrace, then lets the helper accept clients. */
    if (read(life, &ready, 1) != 1) goto done;
    fds[0].fd = life;
    fds[0].events = POLLIN;
    fds[1].fd = listener;
    fds[1].events = POLLIN;
    for (;;) {
        int result = poll(fds, 2, -1);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || fds[0].revents) goto done;
        if (fds[1].revents & POLLIN) {
            peer = accept(listener, NULL, NULL);
            if (peer >= 0) break;
            if (errno != EINTR) goto done;
        } else if (fds[1].revents)
            goto done;
    }
    close(listener);
    listener = -1;
    /* gdbserver stops the target while attaching. The startup gate is released
       only once its first protocol reply confirms that attachment finished. */
    sprintf(pid, "%ld", (long)target);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, transport)) goto done;
    child = fork();
    if (!child) {
        close(life);
        close(gate);
        close(transport[0]);
        close(peer);
        if (dup2(transport[1], STDIN_FILENO) < 0 || dup2(transport[1], STDOUT_FILENO) < 0) _exit(126);
        if (transport[1] > STDOUT_FILENO) close(transport[1]);
        execl(server, server, "--once", "--attach", "stdio", pid, (char*)NULL);
        perror("scarletnx: starting gdbserver");
        _exit(127);
    }
    if (child < 0) {
        kill(target, SIGCONT);
        goto done;
    }
    close(transport[1]);
    transport[1] = -1;
    if (fcntl(peer, F_SETFL, O_NONBLOCK) || fcntl(transport[0], F_SETFL, O_NONBLOCK)) goto done;
    {
        struct pollfd relay[3];
        relay[0] = fds[0];
        relay[1].fd = peer;
        relay[1].events = POLLIN;
        relay[2].fd = transport[0];
        relay[2].events = POLLIN;
        for (;;) {
            int i, polled = poll(relay, 3, 100);
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) {
                child = -1;
                kill(target, SIGCONT);
                break;
            }
            if (polled < 0 && errno == EINTR) continue;
            if (polled < 0 || relay[0].revents) break;
            for (i = 1; i <= 2; ++i) {
                if (relay[i].revents & POLLIN) {
                    char buffer[4096];
                    ssize_t n = read(relay[i].fd, buffer, sizeof(buffer));
                    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                    if (n <= 0) goto done;
                    if (i == 2 && gate >= 0) {
                        ready = '1';
                        if (write(gate, &ready, 1) < 0 && errno != EPIPE) perror("scarletnx: debugger startup gate");
                        close(gate);
                        gate = -1;
                    }
                    if (relay_write(relay[3 - i].fd, life, buffer, (size_t)n)) goto done;
                } else if (relay[i].revents)
                    goto done;
            }
        }
    }
done:
    if (child > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (transport[0] >= 0) close(transport[0]);
    if (transport[1] >= 0) close(transport[1]);
    if (listener >= 0) close(listener);
    if (peer >= 0) close(peer);
    if (gate >= 0) close(gate);
    close(life);
    _exit(0);
}

int snx_gdb_start(int stop, int port) {
    struct sockaddr_in address;
    int listener = -1, life[2] = {-1, -1}, gate[2] = {-1, -1}, one = 1;
    pid_t helper = -1;
    char ready = '1';
    char* server;
    const char* why = NULL; /* the failure's own reason, NULL for the syscall's errno */
    if (lifetime >= 0) return 0;
    server = snx_find_tool("gdbserver");
    if (!server) {
        fputs("scarletnx: -S/-s requires gdbserver on PATH\n", stderr);
        return 1;
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) goto failed;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
#ifdef __linux__
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#else
    address.sin_addr.s_addr = htonl(0x7f000001);
#endif
    address.sin_port = htons((unsigned short)port);
    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) || listen(listener, 1) || pipe(life) || pipe(gate))
        goto failed;
    if (fcntl(life[1], F_SETFD, FD_CLOEXEC) < 0) goto failed;
    fflush(NULL);
    helper = fork();
    if (helper < 0) goto failed;
    if (!helper) {
        /* A remote interrupt may signal the target's entire process group. */
        if (setpgid(0, 0)) _exit(126);
        close(life[1]);
        close(gate[0]);
        monitor(listener, life[0], gate[1], getppid(), server);
    }
#ifdef __linux__
    /* Yama normally forbids a child attaching to its parent. Permit only our
       helper and its descendants, never arbitrary processes. */
    if (prctl(PR_SET_PTRACER, (unsigned long)helper, 0UL, 0UL, 0UL) && errno != EINVAL) goto failed;
#endif
    close(listener);
    listener = -1;
    close(life[0]);
    life[0] = -1;
    close(gate[1]);
    gate[1] = -1;
    fprintf(stderr,
            stop ? "Connect gdb to port %i now (tar rem :%i). Waiting before startup.\n"
                 : "GDB stub listening on localhost:%i (tar rem :%i).\n",
            port, port);
    fflush(stderr);
    if (write(life[1], &ready, 1) != 1) goto failed;
    if (stop) {
        ssize_t n;
        do { n = read(gate[0], &ready, 1); } while (n < 0 && errno == EINTR);
        if (n != 1) {
            why = "the debugger helper exited before a client connected";
            goto failed;
        }
    }
    close(gate[0]);
    lifetime = life[1]; /* deliberately held until process exit */
    free(server);
    return 0;
failed:
    fprintf(stderr, "scarletnx: cannot start GDB stub on localhost:%i: %s\n", port, why ? why : strerror(errno));
    if (helper > 0) {
        kill(helper, SIGKILL);
        while (waitpid(helper, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (listener >= 0) close(listener);
    if (life[0] >= 0) close(life[0]);
    if (life[1] >= 0) close(life[1]);
    if (gate[0] >= 0) close(gate[0]);
    if (gate[1] >= 0) close(gate[1]);
    free(server);
    return 1;
}
#endif
