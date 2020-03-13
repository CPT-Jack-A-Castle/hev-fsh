/*
 ============================================================================
 Name        : hev-fsh-server-session.h
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2017 - 2019 everyone.
 Description : Fsh server session
 ============================================================================
 */

#ifndef __HEV_FSH_SERVER_SESSION_H__
#define __HEV_FSH_SERVER_SESSION_H__

#include "hev-fsh-session.h"

typedef struct _HevFshServerSession HevFshServerSession;

HevFshServerSession *hev_fsh_server_session_new (int client_fd,
                                                 HevFshSessionNotify notify,
                                                 void *notify_data);

void hev_fsh_server_session_run (HevFshServerSession *self);

#endif /* __HEV_FSH_SERVER_SESSION_H__ */
