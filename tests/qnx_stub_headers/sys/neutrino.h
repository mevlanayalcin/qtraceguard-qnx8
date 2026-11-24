/* Clean-room compile-check stub ONLY for Linux CI verification of #ifdef __QNX__ code paths.
 * NOT derived from or containing any proprietary QNX header. SPDX-License-Identifier: MIT */
#ifndef QTG_STUB_SYS_NEUTRINO_H
#define QTG_STUB_SYS_NEUTRINO_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#ifndef EOK
#define EOK 0
#endif
typedef int64_t rcvid_t;
#define _NTO_CHF_FIXED_PRIORITY 0x0040U
#define _NTO_CHF_UNBLOCK        0x0001U
#define _NTO_CHF_PRIVATE        0x0800U
#define _NTO_SIDE_CHANNEL       0x40000000
#define _NTO_TIMEOUT_SEND       (1U << 4)
#define _NTO_TIMEOUT_RECEIVE    (1U << 5)
#define _NTO_TIMEOUT_REPLY      (1U << 6)
#define _NTO_TCTL_RUNMASK       4
#define _PULSE_CODE_UNBLOCK     (-32)
#define SIGEV_UNBLOCK_INIT(e)   do { (void)(e); } while (0)
#ifndef SCHED_SPORADIC
#define SCHED_SPORADIC 4
#endif
struct _pulse { uint16_t type; uint16_t subtype; int8_t code; uint8_t zero[3]; union { int sival_int; void *sival_ptr; } value; int32_t scoid; };
struct _msg_info { uint32_t nd; uint32_t srcnd; pid_t pid; int32_t tid; int32_t chid; int32_t scoid; int32_t coid; int32_t msglen; int32_t srcmsglen; int32_t dstmsglen; int16_t priority; int16_t flags; };
int ChannelCreate(unsigned flags);
int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags);
int ConnectDetach(int coid);
long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes);
rcvid_t MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info);
int MsgReply(rcvid_t rcvid, int64_t status, const void *msg, size_t bytes);
int MsgError(rcvid_t rcvid, int err);
int MsgSendPulse(int coid, int priority, int code, int value);
int ThreadCtl(int cmd, void *data);
int TimerTimeout(clockid_t id, int flags, const struct sigevent *notify, const uint64_t *ntime, uint64_t *otime);
int SchedCtl(int cmd, void *data, size_t length);
int pthread_setname_np(pthread_t thread, const char *name);
/* Extend sched_param fields used by QNX SCHED_SPORADIC in stub check */
#define sched_param qtg_qnx_sched_param
struct qtg_qnx_sched_param {
    int sched_priority;
    int sched_ss_low_priority;
    struct timespec sched_ss_repl_period;
    struct timespec sched_ss_init_budget;
    int sched_ss_max_repl;
};
int qtg_stub_pthread_setschedparam(pthread_t t, int p, const struct qtg_qnx_sched_param *sp);
int qtg_stub_pthread_getschedparam(pthread_t t, int *p, struct qtg_qnx_sched_param *sp);
int qtg_stub_pthread_attr_setschedparam(pthread_attr_t *a, const struct qtg_qnx_sched_param *sp);
#define pthread_setschedparam qtg_stub_pthread_setschedparam
#define pthread_getschedparam qtg_stub_pthread_getschedparam
#define pthread_attr_setschedparam qtg_stub_pthread_attr_setschedparam
#endif
