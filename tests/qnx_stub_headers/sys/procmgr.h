/* Clean-room compile-check stub ONLY. SPDX-License-Identifier: MIT */
#ifndef QTG_STUB_SYS_PROCMGR_H
#define QTG_STUB_SYS_PROCMGR_H
#include <sys/types.h>
#define PROCMGR_ADN_NONROOT  0x00020000U
#define PROCMGR_AOP_ALLOW    0x00000001U
#define PROCMGR_AOP_DENY     0x00000002U
#define PROCMGR_AOP_LOCK     0x00000004U
#define PROCMGR_AID_PRIORITY 0x00000010U
#define PROCMGR_AID_PATHSPACE 0x00000011U
#define PROCMGR_AID_EOL      0U
int procmgr_ability(pid_t pid, ...);
#endif
