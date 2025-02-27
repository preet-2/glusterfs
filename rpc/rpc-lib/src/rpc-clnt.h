/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#ifndef __RPC_CLNT_H
#define __RPC_CLNT_H

#include <glusterfs/stack.h>
#include "rpc-transport.h"
#include <glusterfs/timer.h>
#include "xdr-common.h"
#include "glusterfs3.h"

typedef enum {
    RPC_CLNT_CONNECT,
    RPC_CLNT_DISCONNECT,
    RPC_CLNT_PING,
    RPC_CLNT_MSG,
    RPC_CLNT_DESTROY
} rpc_clnt_event_t;

typedef enum {
    RPC_STATUS_INITIALIZED,
    RPC_STATUS_CONNECTED,
    RPC_STATUS_DISCONNECTED
} rpc_clnt_status_t;

#define SFRAME_GET_PROGNUM(sframe) (sframe->rpcreq->prog->prognum)
#define SFRAME_GET_PROGVER(sframe) (sframe->rpcreq->prog->progver)
#define SFRAME_GET_PROCNUM(sframe) (sframe->rpcreq->procnum)

struct rpc_req;
struct rpc_clnt;
struct rpc_clnt_config;
struct rpc_clnt_program;

typedef int (*rpc_clnt_notify_t)(struct rpc_clnt *rpc, void *mydata,
                                 rpc_clnt_event_t fn, void *data);

typedef int (*fop_cbk_fn_t)(struct rpc_req *req, struct iovec *iov, int count,
                            void *myframe);

typedef int (*clnt_fn_t)(call_frame_t *fr, xlator_t *xl, void *args);

struct saved_frame {
    union {
        struct list_head list;
        struct {
            struct saved_frame *frame_next;
            struct saved_frame *frame_prev;
        };
    };
    void *capital_this;
    void *frame;
    struct rpc_req *rpcreq;
    time_t saved_at;
    rpc_transport_rsp_t rsp;
};

struct saved_frames {
    int64_t count;
    struct saved_frame sf;
    struct saved_frame lk_sf;
};

/* Initialized by procnum */
typedef struct rpc_clnt_procedure {
    char *procname;
    clnt_fn_t fn;
} rpc_clnt_procedure_t;

typedef struct rpc_clnt_program {
    char *progname;
    int prognum;
    int progver;
    rpc_clnt_procedure_t *proctable;
    char **procnames;
    int numproc;
} rpc_clnt_prog_t;

typedef int (*rpcclnt_cb_fn)(struct rpc_clnt *rpc, void *mydata, void *data);

/* The descriptor for each procedure/actor that runs
 * over the RPC service.
 */
typedef struct rpcclnt_actor_desc {
    char procname[32];
    rpcclnt_cb_fn actor;
    int procnum;
} rpcclnt_cb_actor_t;

/* Describes a program and its version along with the function pointers
 * required to handle the procedures/actors of each program/version.
 * Never changed ever by any thread so no need for a lock.
 */
typedef struct rpcclnt_cb_program {
    char progname[32];
    int prognum;
    int progver;
    rpcclnt_cb_actor_t *actors; /* All procedure handlers */
    /* Program specific state handed to actors */
    void *private;

    /* list member to link to list of registered services with rpc_clnt */
    struct list_head program;

    /* Needed for passing back in cb_actor */
    void *mydata;
    int numactors; /* Num actors in actor array */

} rpcclnt_cb_program_t;

typedef struct rpc_auth_data {
    int flavour;
    int datalen;
    char authdata[GF_MAX_AUTH_BYTES];
} rpc_auth_data_t;

struct rpc_clnt_config {
    time_t rpc_timeout;
    int remote_port;
    char *remote_host;
    time_t ping_timeout;
};

#define rpc_auth_flavour(au) ((au).flavour)

struct rpc_clnt_connection {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    rpc_transport_t *trans;
    struct rpc_clnt_config config;
    gf_timer_t *reconnect;
    gf_timer_t *timer;
    gf_timer_t *ping_timer;
    struct rpc_clnt *rpc_clnt;
    struct saved_frames *saved_frames;
    time_t last_sent;
    time_t last_received;
    uint64_t pingcnt;
    uint64_t msgcnt;
    uint64_t cleanup_gen;
    char *name;
    int32_t ping_started;
    time_t frame_timeout;
    time_t ping_timeout;
    rpc_clnt_status_t status;
};
typedef struct rpc_clnt_connection rpc_clnt_connection_t;

struct rpc_req {
    rpc_clnt_connection_t *conn;
    struct iovec req[2];
    struct iobref *req_iobref;
    struct iovec rsp[2];
    int rspcnt;
    int reqcnt;
    struct iobref *rsp_iobref;
    rpc_clnt_prog_t *prog;
    rpc_auth_data_t verf;
    fop_cbk_fn_t cbkfn;
    void *conn_private;
    int procnum;
    int rpc_status;
    uint32_t xid;
};

typedef struct rpc_clnt {
    pthread_mutex_t lock;
    rpc_clnt_notify_t notifyfn;
    rpc_clnt_connection_t conn;
    void *mydata;
    gf_atomic_t xid;

    /* list of cb programs registered with rpc-clnt */
    struct list_head programs;

    /* Memory pool for rpc_req_t */
    struct mem_pool *reqpool;

    struct mem_pool *saved_frames_pool;

    glusterfs_ctx_t *ctx;
    gf_atomic_t refcount;
    xlator_t *owner;
    int auth_value;
    char disabled;
} rpc_clnt_t;

struct rpc_clnt *
rpc_clnt_new(dict_t *options, xlator_t *owner, char *name,
             uint32_t reqpool_size);

int
rpc_clnt_start(struct rpc_clnt *rpc);

int
rpc_clnt_cleanup_and_start(struct rpc_clnt *rpc);

int
rpc_clnt_register_notify(struct rpc_clnt *rpc, rpc_clnt_notify_t fn,
                         void *mydata);

/* Some preconditions related to vectors holding responses.
 * @rsphdr: should contain pointer to buffer which can hold response header
 *          and length of the program header. In case of procedures whose
 *          respnose size is not bounded (eg., glusterfs lookup), the length
 *          should be equal to size of buffer.
 * @rsp_payload: should contain pointer and length of the bu
 *
 * 1. Both @rsp_hdr and @rsp_payload are optional.
 * 2. The user of rpc_clnt_submit, if wants response hdr and payload in its own
 *    buffers, then it has to populate @rsphdr and @rsp_payload.
 * 3. when @rsp_payload is not NULL, @rsphdr should
 *    also be filled with pointer to buffer to hold header and length
 *    of the header.
 */

int
rpc_clnt_submit(struct rpc_clnt *rpc, rpc_clnt_prog_t *prog, int procnum,
                fop_cbk_fn_t cbkfn, struct iovec *proghdr, int proghdrcount,
                struct iovec *progpayload, int progpayloadcount,
                struct iobref *iobref, void *frame, struct iovec *rsphdr,
                int rsphdr_count, struct iovec *rsp_payload,
                int rsp_payload_count, struct iobref *rsp_iobref);

struct rpc_clnt *
rpc_clnt_ref(struct rpc_clnt *rpc);

struct rpc_clnt *
rpc_clnt_unref(struct rpc_clnt *rpc);

int
rpc_clnt_connection_cleanup(rpc_clnt_connection_t *conn);
int
rpc_clnt_reconnect_cleanup(rpc_clnt_connection_t *conn);
rpc_clnt_status_t
rpc_clnt_connection_status(rpc_clnt_connection_t *conn);

void
rpc_clnt_reconnect(void *trans_ptr);

void
rpc_clnt_reconfig(struct rpc_clnt *rpc, struct rpc_clnt_config *config);

/* All users of RPC services should use this API to register their
 * procedure handlers.
 */
int
rpcclnt_cbk_program_register(struct rpc_clnt *svc,
                             rpcclnt_cb_program_t *program, void *mydata);

int
rpc_clnt_disable(struct rpc_clnt *rpc);

int
rpc_clnt_mgmt_pmap_signout(glusterfs_ctx_t *ctx, char *brick_name);

#endif /* !_RPC_CLNT_H */
