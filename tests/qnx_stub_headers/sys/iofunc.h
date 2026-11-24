/* Clean-room compile-check stub ONLY. SPDX-License-Identifier: MIT */
#ifndef QTG_STUB_SYS_IOFUNC_H
#define QTG_STUB_SYS_IOFUNC_H
#include <sys/stat.h>
#include <stdint.h>
#include <sys/neutrino.h>
#ifndef S_IFNAM
#define S_IFNAM 0x5000U
#endif
typedef struct { int offset; } iofunc_ocb_t;
#define RESMGR_OCB_T iofunc_ocb_t
typedef struct { int mode; } iofunc_attr_t;
typedef struct { rcvid_t rcvid; int nbytes; } resmgr_context_t;
typedef struct { struct { uint32_t nbytes; } i; } io_read_t;
#define _IO_READ_GET_NBYTES(msg) ((msg)->i.nbytes)
typedef struct { struct { uint32_t dcmd; } i; struct { uint32_t nbytes; } o; } io_devctl_t;
typedef struct { int dummy; } resmgr_connect_funcs_t;
typedef struct {
    int (*read)(resmgr_context_t *, io_read_t *, RESMGR_OCB_T *);
    int (*devctl)(resmgr_context_t *, io_devctl_t *, RESMGR_OCB_T *);
} resmgr_io_funcs_t;
typedef struct { unsigned nparts_max; unsigned msg_max_size; } resmgr_attr_t;
typedef struct dispatch dispatch_t;
typedef struct dispatch_context dispatch_context_t;
#define _RESMGR_CONNECT_NFUNCS 1
#define _RESMGR_IO_NFUNCS      1
#define _FTYPE_ANY             0
#define _RESMGR_NOREPLY        (-1)
#define _RESMGR_DEFAULT        (-2)
#define _RESMGR_NPARTS(n)      (n)
#define _IO_SET_READ_NBYTES(ctp, n) do { (ctp)->nbytes = (n); } while (0)
#define _RESMGR_PTR(ctp, p, len)    ((int)((ctp)->rcvid == 0 && (p) != NULL && (len) > 0))
void iofunc_func_init(unsigned nconnect, resmgr_connect_funcs_t *c, unsigned nio, resmgr_io_funcs_t *io);
void iofunc_attr_init(iofunc_attr_t *attr, mode_t mode, iofunc_attr_t *dattr, void *info);
int  iofunc_read_verify(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb, int *nonblock);
int  iofunc_devctl_default(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb);
dispatch_t *dispatch_create(void);
dispatch_context_t *dispatch_context_alloc(dispatch_t *dpp);
dispatch_context_t *dispatch_block(dispatch_context_t *ctp);
int dispatch_handler(dispatch_context_t *ctp);
int resmgr_attach(dispatch_t *dpp, resmgr_attr_t *attr, const char *path, int file_type,
                  unsigned flags, const resmgr_connect_funcs_t *c, const resmgr_io_funcs_t *io,
                  iofunc_attr_t *handle);
#endif
