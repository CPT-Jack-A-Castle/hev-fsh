/*
 ============================================================================
 Name        : hev-fsh-server-session.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2017 everyone.
 Description : Fsh server session
 ============================================================================
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "hev-fsh-server-session.h"
#include "hev-fsh-protocol.h"
#include "hev-fsh-task-io.h"
#include "hev-memory-allocator.h"
#include "hev-task.h"

#define SESSION_HP	(10)
#define TASK_STACK_SIZE	(3 * 4096)

static void hev_fsh_server_session_task_entry (void *data);

enum
{
	TYPE_FORWARD = 1,
	TYPE_CONNECT,
};

enum
{
	STEP_NULL,
	STEP_READ_MESSAGE,

	STEP_DO_LOGIN,
	STEP_WRITE_MESSAGE_TOKEN,

	STEP_DO_CONNECT,
	STEP_WRITE_MESSAGE_CONNECT,
	STEP_DO_SPLICE,

	STEP_DO_ACCEPT,

	STEP_DO_KEEP_ALIVE,

	STEP_CLOSE_SESSION,
};

struct _HevFshServerSession
{
	HevFshServerSessionBase base;

	int client_fd;
	int remote_fd;
	int ref_count;

	int type;
	HevFshToken token;

	HevFshServerSessionCloseNotify notify;
	void *notify_data;
};

HevFshServerSession *
hev_fsh_server_session_new (int client_fd,
			HevFshServerSessionCloseNotify notify, void *notify_data)
{
	HevFshServerSession *self;
	HevTask *task;

	self = hev_malloc0 (sizeof (HevFshServerSession));
	if (!self)
		return NULL;

	self->base.hp = SESSION_HP;

	self->ref_count = 1;
	self->remote_fd = -1;
	self->client_fd = client_fd;
	self->notify = notify;
	self->notify_data = notify_data;

	task = hev_task_new (TASK_STACK_SIZE);
	if (!task) {
		hev_free (self);
		return NULL;
	}

	self->base.task = task;
	hev_task_set_priority (task, 1);

	return self;
}

HevFshServerSession *
hev_fsh_server_session_ref (HevFshServerSession *self)
{
	self->ref_count ++;

	return self;
}

void
hev_fsh_server_session_unref (HevFshServerSession *self)
{
	self->ref_count --;
	if (self->ref_count)
		return;

	hev_free (self);
}

void
hev_fsh_server_session_run (HevFshServerSession *self)
{
	hev_task_run (self->base.task, hev_fsh_server_session_task_entry, self);
}

static int
fsh_task_io_yielder (HevTaskYieldType type, void *data)
{
	HevFshServerSession *self = data;

	self->base.hp = SESSION_HP;

	hev_task_yield (type);

	return (self->base.hp > 0) ? 0 : -1;
}

static int
fsh_server_read_message (HevFshServerSession *self)
{
	HevFshMessage message;
	ssize_t len;

	len = hev_fsh_task_io_socket_recv (self->client_fd, &message, sizeof (message),
				MSG_WAITALL, fsh_task_io_yielder, self);
	if (len <= 0)
		return STEP_CLOSE_SESSION;

	if (message.ver != 1)
		return STEP_CLOSE_SESSION;

	switch (message.cmd) {
	case HEV_FSH_CMD_LOGIN:
		return STEP_DO_LOGIN;
	case HEV_FSH_CMD_CONNECT:
		return STEP_DO_CONNECT;
	case HEV_FSH_CMD_ACCEPT:
		return STEP_DO_ACCEPT;
	case HEV_FSH_CMD_KEEP_ALIVE:
		return STEP_DO_KEEP_ALIVE;
	}

	return STEP_CLOSE_SESSION;
}

static int
fsh_server_do_login (HevFshServerSession *self)
{
	char token_str[40];
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof (addr);

	hev_fsh_protocol_token_generate (self->token);
	hev_fsh_protocol_token_to_string (self->token, token_str);

	memset (&addr, 0, sizeof (addr));
	getpeername (self->client_fd, (struct sockaddr *) &addr, &addr_len);
	printf ("[LOGIN]   Client: %s:%d Token: %s\n", inet_ntoa (addr.sin_addr),
				ntohs (addr.sin_port), token_str);
	fflush (stdout);

	return STEP_WRITE_MESSAGE_TOKEN;
}

static int
fsh_server_write_message_token (HevFshServerSession *self)
{
	HevFshMessage message;
	HevFshMessageToken message_token;
	struct msghdr mh;
	struct iovec iov[2];

	message.ver = 1;
	message.cmd = HEV_FSH_CMD_TOKEN;
	memcpy (message_token.token, self->token, sizeof (HevFshToken));

	memset (&mh, 0, sizeof (mh));
	mh.msg_iov = iov;
	mh.msg_iovlen = 2;

	iov[0].iov_base = &message;
	iov[0].iov_len = sizeof (message);
	iov[1].iov_base = &message_token;
	iov[1].iov_len = sizeof (message_token);

	if (hev_fsh_task_io_socket_sendmsg (self->client_fd, &mh, MSG_WAITALL,
					fsh_task_io_yielder, self) <= 0)
		return STEP_CLOSE_SESSION;

	self->type = TYPE_FORWARD;

	return STEP_READ_MESSAGE;
}

static int
fsh_server_do_connect (HevFshServerSession *self)
{
	HevFshMessageToken message_token;
	ssize_t len;

	len = hev_fsh_task_io_socket_recv (self->client_fd, &message_token, sizeof (message_token),
				MSG_WAITALL, fsh_task_io_yielder, self);
	if (len <= 0)
		return STEP_CLOSE_SESSION;

	memcpy (self->token, message_token.token, sizeof (HevFshToken));

	return STEP_WRITE_MESSAGE_CONNECT;
}

static int
fsh_server_write_message_connect (HevFshServerSession *self)
{
	HevFshServerSessionBase *session;
	HevFshServerSession *fs_session;
	HevFshMessage message;
	HevFshMessageToken message_token;
	struct msghdr mh;
	struct iovec iov[2];
	char token_str[40];
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof (addr);

	for (session=self->base.prev; session; session=session->prev) {
		HevFshServerSession *fs_session = (HevFshServerSession *) session;

		if (fs_session->type != TYPE_FORWARD)
			continue;
		if (memcmp (self->token, fs_session->token, sizeof (HevFshToken)) == 0)
			break;
	}
	if (!session) {
		for (session=self->base.next; session; session=session->next) {
			HevFshServerSession *fs_session = (HevFshServerSession *) session;

			if (fs_session->type != TYPE_FORWARD)
				continue;
			if (memcmp (self->token, fs_session->token, sizeof (HevFshToken)) == 0)
				break;
		}
	}

	hev_fsh_protocol_token_to_string (self->token, token_str);

	memset (&addr, 0, sizeof (addr));
	getpeername (self->client_fd, (struct sockaddr *) &addr, &addr_len);
	printf ("[CONNECT] Client: %s:%d Token: %s\n", inet_ntoa (addr.sin_addr),
				ntohs (addr.sin_port), token_str);
	fflush (stdout);

	if (!session)
		return STEP_CLOSE_SESSION;

	message.ver = 1;
	message.cmd = HEV_FSH_CMD_CONNECT;
	memcpy (message_token.token, self->token, sizeof (HevFshToken));

	memset (&mh, 0, sizeof (mh));
	mh.msg_iov = iov;
	mh.msg_iovlen = 2;

	iov[0].iov_base = &message;
	iov[0].iov_len = sizeof (message);
	iov[1].iov_base = &message_token;
	iov[1].iov_len = sizeof (message_token);

	fs_session = (HevFshServerSession *) session;
	if (hev_fsh_task_io_socket_sendmsg (fs_session->client_fd, &mh, MSG_WAITALL,
					fsh_task_io_yielder, self) <= 0)
		return STEP_CLOSE_SESSION;

	self->type = TYPE_CONNECT;

	return STEP_DO_SPLICE;
}

static int
fsh_server_do_accept (HevFshServerSession *self)
{
	HevTask *task = hev_task_self ();
	HevFshServerSessionBase *session;
	HevFshServerSession *fs_session;
	HevFshMessageToken message_token;
	ssize_t len;

	len = hev_fsh_task_io_socket_recv (self->client_fd, &message_token, sizeof (message_token),
				MSG_WAITALL, fsh_task_io_yielder, self);
	if (len <= 0)
		return STEP_CLOSE_SESSION;

	for (session=self->base.prev; session; session=session->prev) {
		HevFshServerSession *fs_session = (HevFshServerSession *) session;

		if (fs_session->type != TYPE_CONNECT)
			continue;
		if (memcmp (message_token.token, fs_session->token, sizeof (HevFshToken)) == 0)
			break;
	}
	if (!session) {
		for (session=self->base.next; session; session=session->next) {
			HevFshServerSession *fs_session = (HevFshServerSession *) session;

			if (fs_session->type != TYPE_CONNECT)
				continue;
			if (memcmp (message_token.token, fs_session->token, sizeof (HevFshToken)) == 0)
				break;
		}
	}

	if (!session)
		return STEP_CLOSE_SESSION;

	fs_session = (HevFshServerSession *) session;
	fs_session->remote_fd = self->client_fd;
	hev_task_del_fd (task, self->client_fd);
	hev_task_add_fd (session->task, fs_session->remote_fd, EPOLLIN | EPOLLOUT);
	hev_task_wakeup (session->task);

	self->client_fd = -1;
	return STEP_CLOSE_SESSION;
}

static int
fsh_server_do_keep_alive (HevFshServerSession *self)
{
	return STEP_READ_MESSAGE;
}

static int
fsh_server_do_splice (HevFshServerSession *self)
{
	/* wait for remote fd */
	while (self->base.hp > 0) {
		if (self->remote_fd >= 0)
			break;
		hev_task_yield (HEV_TASK_WAITIO);
	}

	hev_fsh_task_io_splice (self->client_fd, self->client_fd, self->remote_fd, self->remote_fd,
				2048, fsh_task_io_yielder, self);

	return STEP_CLOSE_SESSION;
}

static int
fsh_server_close_session (HevFshServerSession *self)
{
	if (self->remote_fd >= 0)
		close (self->remote_fd);
	if (self->client_fd >= 0)
		close (self->client_fd);

	self->notify (self, self->notify_data);
	hev_fsh_server_session_unref (self);

	return STEP_NULL;
}

static void
hev_fsh_server_session_task_entry (void *data)
{
	HevTask *task = hev_task_self ();
	HevFshServerSession *self = data;
	int step = STEP_READ_MESSAGE;

	hev_task_add_fd (task, self->client_fd, EPOLLIN | EPOLLOUT);

	while (step) {
		switch (step) {
		case STEP_READ_MESSAGE:
			step = fsh_server_read_message (self);
			break;

		case STEP_DO_LOGIN:
			step = fsh_server_do_login (self);
			break;
		case STEP_WRITE_MESSAGE_TOKEN:
			step = fsh_server_write_message_token (self);
			break;

		case STEP_DO_CONNECT:
			step = fsh_server_do_connect (self);
			break;
		case STEP_WRITE_MESSAGE_CONNECT:
			step = fsh_server_write_message_connect (self);
			break;
		case STEP_DO_SPLICE:
			step = fsh_server_do_splice (self);
			break;

		case STEP_DO_ACCEPT:
			step = fsh_server_do_accept (self);
			break;

		case STEP_DO_KEEP_ALIVE:
			step = fsh_server_do_keep_alive (self);
			break;

		case STEP_CLOSE_SESSION:
			step = fsh_server_close_session (self);
			break;
		default:
			step = STEP_NULL;
			break;
		}
	}
}

