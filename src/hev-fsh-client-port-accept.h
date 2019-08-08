/*
 ============================================================================
 Name        : hev-fsh-client-port-accept.h
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2018 - 2019 everyone.
 Description : Fsh client port accept
 ============================================================================
 */

#ifndef __HEV_FSH_CLIENT_PORT_ACCEPT_H__
#define __HEV_FSH_CLIENT_PORT_ACCEPT_H__

#include "hev-fsh-client-accept.h"

typedef struct _HevFshClientPortAccept HevFshClientPortAccept;

HevFshClientBase *hev_fsh_client_port_accept_new (HevFshConfig *config,
                                                  HevFshToken token);

#endif /* __HEV_FSH_CLIENT_PORT_ACCEPT_H__ */
