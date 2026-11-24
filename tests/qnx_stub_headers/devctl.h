/* Clean-room compile-check stub ONLY. SPDX-License-Identifier: MIT */
#ifndef QTG_STUB_DEVCTL_H
#define QTG_STUB_DEVCTL_H
#define _DCMD_MISC 1U
#define __DIOF(cls, cmd, type) (((uint32_t)(cls) << 8) | ((uint32_t)(cmd) & 0xFFU) | ((uint32_t)sizeof(type) << 16))
#define _DEVCTL_DATA(msg) ((void *)(&(msg)))
#endif
