/*
 ============================================================================
 Name        : hev-fsh-client-port-forward.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2018 everyone.
 Description : Fsh client port forward
 ============================================================================
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>

#include "hev-fsh-client-port-forward.h"
#include "hev-fsh-client-port-accept.h"
#include "hev-fsh-protocol.h"
#include "hev-memory-allocator.h"
#include "hev-task.h"
#include "hev-task-io-socket.h"

#define TASK_STACK_SIZE (64 * 4096)
#define KEEP_ALIVE_INTERVAL (30 * 1000)

struct _HevFshClientPortForward
{
    HevFshClientBase base;

    const char *token;
    const char *srv_addr;
    unsigned int srv_port;

    HevTask *task;
};

static void hev_fsh_client_port_forward_task_entry (void *data);
static void hev_fsh_client_port_forward_destroy (HevFshClientBase *self);

HevFshClientPortForward *
hev_fsh_client_port_forward_new (const char *address, unsigned int port,
                                 const char *srv_addr, unsigned int srv_port,
                                 const char *token)
{
    HevFshClientPortForward *self;

    self = hev_malloc0 (sizeof (HevFshClientPortForward));
    if (!self) {
        fprintf (stderr, "Allocate client port forward failed!\n");
        return NULL;
    }

    if (0 > hev_fsh_client_base_construct (&self->base, address, port)) {
        fprintf (stderr, "Construct client base failed!\n");
        hev_free (self);
        return NULL;
    }

    self->task = hev_task_new (TASK_STACK_SIZE);
    if (!self->task) {
        fprintf (stderr, "Create client port's task failed!\n");
        hev_free (self);
        return NULL;
    }

    self->token = token;
    self->srv_addr = srv_addr;
    self->srv_port = srv_port;
    self->base._destroy = hev_fsh_client_port_forward_destroy;

    hev_task_run (self->task, hev_fsh_client_port_forward_task_entry, self);

    return self;
}

static void
hev_fsh_client_port_forward_destroy (HevFshClientBase *self)
{
    hev_free (self);
}

static void
hev_fsh_client_port_forward_task_entry (void *data)
{
    HevTask *task = hev_task_self ();
    HevFshClientPortForward *self = data;
    HevFshMessage message;
    HevFshMessageToken send_token;
    HevFshMessageToken recv_token;
    HevFshToken token;
    int sock_fd, wait_keep_alive = 0;
    unsigned int sleep_ms = KEEP_ALIVE_INTERVAL;
    ssize_t len;
    char token_str[40];
    char *token_src;

    sock_fd = self->base.fd;
    hev_task_add_fd (task, sock_fd, EPOLLIN | EPOLLOUT);

    if (hev_task_io_socket_connect (sock_fd, &self->base.address,
                                    sizeof (struct sockaddr_in), NULL,
                                    NULL) < 0) {
        fprintf (stderr, "Connect to server failed!\n");
        return;
    }

    message.ver = self->token ? 2 : 1;
    message.cmd = HEV_FSH_CMD_LOGIN;

    len = hev_task_io_socket_send (sock_fd, &message, sizeof (message),
                                   MSG_WAITALL, NULL, NULL);
    if (len <= 0)
        return;

    if (self->token) {
        if (hev_fsh_protocol_token_from_string (send_token.token,
                                                self->token) == -1) {
            fprintf (stderr, "Can't parse token!\n");
            return;
        }

        len = hev_task_io_socket_send (
            sock_fd, &send_token, sizeof (send_token), MSG_WAITALL, NULL, NULL);
        if (len <= 0)
            return;
    }

    /* recv message token */
    len = hev_task_io_socket_recv (sock_fd, &message, sizeof (message),
                                   MSG_WAITALL, NULL, NULL);
    if (len <= 0)
        return;

    if (message.cmd != HEV_FSH_CMD_TOKEN) {
        fprintf (stderr, "Can't login to server!\n");
        return;
    }

    len = hev_task_io_socket_recv (sock_fd, &recv_token, sizeof (recv_token),
                                   MSG_WAITALL, NULL, NULL);
    if (len <= 0)
        return;

    memcpy (token, recv_token.token, sizeof (HevFshToken));
    hev_fsh_protocol_token_to_string (token, token_str);
    if (0 == memcmp (&send_token, &recv_token, sizeof (HevFshMessageToken))) {
        token_src = "client";
    } else {
        token_src = "server";
    }
    printf ("Token: %s (from %s)\n", token_str, token_src);

    for (;;) {
        sleep_ms = hev_task_sleep (sleep_ms);

        len = recv (sock_fd, &message, sizeof (message), MSG_PEEK);
        if (len == -1 && errno == EAGAIN) {
            /* timeout */
            if (0 == sleep_ms && wait_keep_alive) {
                printf ("Connection lost!\n");
                return;
            }
            /* keep alive */
            message.ver = 2;
            message.cmd = HEV_FSH_CMD_KEEP_ALIVE;
            len = hev_task_io_socket_send (sock_fd, &message, sizeof (message),
                                           MSG_WAITALL, NULL, NULL);
            if (len <= 0)
                return;
            wait_keep_alive = 1;
            sleep_ms = KEEP_ALIVE_INTERVAL;
            continue;
        }

        /* recv message connect */
        len = hev_task_io_socket_recv (sock_fd, &message, sizeof (message),
                                       MSG_WAITALL, NULL, NULL);
        if (len <= 0)
            return;

        switch (message.cmd) {
        case HEV_FSH_CMD_KEEP_ALIVE:
            wait_keep_alive = 0;
            sleep_ms = KEEP_ALIVE_INTERVAL;
            continue;
        case HEV_FSH_CMD_CONNECT:
            break;
        default:
            return;
        }

        len = hev_task_io_socket_recv (
            sock_fd, &recv_token, sizeof (recv_token), MSG_WAITALL, NULL, NULL);
        if (len <= 0)
            return;

        if (memcmp (recv_token.token, token, sizeof (HevFshToken)) != 0)
            return;

        hev_fsh_client_port_accept_new (&self->base.address,
                                        sizeof (struct sockaddr_in),
                                        self->srv_addr, self->srv_port,
                                        &recv_token.token);
        sleep_ms = KEEP_ALIVE_INTERVAL;
    }
}