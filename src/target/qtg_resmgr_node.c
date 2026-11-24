/*
 * QTraceGuard - QNX OS 8.0 Resource Manager & Deterministic Scenario Workload
 * SPDX-License-Identifier: MIT
 *
 * Demonstrates real QNX OS 8.0 Resource Manager idioms (`iofunc_attr_init`,
 * `iofunc_func_init`, `resmgr_attach`, `dispatch_create`, `procmgr_ability`,
 * `_NTO_CHF_UNBLOCK`, `SCHED_SPORADIC`, `TraceEvent`) and executes 6 reproducible
 * real-time fault/health scenarios across BOTH:
 *   - Live multi-process QNX OS 8.0 (`fork`+`execv` distinct PIDs: `qtg_sensor_rm`,
 *     `qtg_planner_rm`, `qtg_actuator_rm`, `qtg_telemetry_bg` + real `resmgr_attach`
 *     `/dev/qtg_sensor` + real `setuid(1000)` `PROCMGR_AID_PRIORITY` ceiling-63 trap)
 *   - Portable POSIX CI simulation (Linux/macOS host verification)
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__QNX__) || defined(__QNXNTO__)
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#endif

#include "qtg_resmgr_demo.h"
#include "qtg_rtos_shim.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(QTG_BUILD_RESMGR_BIN) && (defined(__QNX__) || defined(__QNXNTO__))
static void qtg_sleep_ms(uint32_t ms) {
    struct timespec ts = { (time_t)(ms / 1000U), (long)((ms % 1000U) * 1000000U) };
    (void)nanosleep(&ts, NULL);
}
#endif

#if defined(__QNX__) || defined(__QNXNTO__)
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/resmgr.h>
#include <sys/neutrino.h>
#include <sys/trace.h>
#include <devctl.h>
#define QTG_DCMD_GET_HEALTH __DIOF(_DCMD_MISC, 1, uint32_t)
#endif

typedef struct pipeline_ctx {
    qtg_scenario_t scen;
    uint32_t       iterations;
    uint32_t       sensor_pid;
    uint32_t       planner_pid;
    uint32_t       actuator_pid;
    uint32_t       bg_pid;
    int            sensor_chid;
    int            planner_chid;
    int            actuator_chid;
} pipeline_ctx_t;

static void *actuator_thread(void *arg) {
    pipeline_ctx_t *ctx = (pipeline_ctx_t *)arg;
#if defined(__QNX__) || defined(__QNXNTO__)
    qtg_shim_register_thread(ctx->actuator_pid, 1U, "brake_act", 12U, 2U);
#endif
    int sensor_coid = -1;
    if (ctx->scen == QTG_SCEN_DEADLOCK) {
        sensor_coid = qtg_shim_connect_attach(ctx->sensor_pid, ctx->sensor_chid);
    }

    for (uint32_t i = 0; i < ctx->iterations; i++) {
        qtg_ipc_request_t req;
        qtg_shim_msg_info_t info;
        memset(&req, 0, sizeof(req));
        int64_t rcvid = qtg_shim_msg_receive(ctx->actuator_pid, 1U, ctx->actuator_chid,
                                             &req, sizeof(req), &info, 250U);
        if (rcvid < 0) break;
        if (rcvid == 0) continue; /* pulse */
        if (req.type == QTG_MSG_TYPE_SHUTDOWN) {
            qtg_shim_msg_reply(ctx->actuator_pid, 1U, rcvid, 0, NULL, 0);
            break;
        }

        if (ctx->scen == QTG_SCEN_DEADLOCK) {
            /* BUG INJECTION: actuator calls back synchronously into sensor -> 3-way deadlock cycle! */
            qtg_ipc_request_t cb = { .type = QTG_MSG_TYPE_STATUS_QUERY, .seq = req.seq };
            qtg_ipc_reply_t cb_rep;
            (void)qtg_shim_msg_send(ctx->actuator_pid, 1U, sensor_coid,
                                    &cb, sizeof(cb), &cb_rep, sizeof(cb_rep), 75U);
#if defined(__QNX__) || defined(__QNXNTO__)
            (void)qtg_shim_msg_error(ctx->actuator_pid, 1U, rcvid, ETIMEDOUT);
#endif
            break;
        } else if (ctx->scen == QTG_SCEN_PRIO_INVERSION) {
            /* _NTO_CHF_FIXED_PRIORITY prevented actuator (prio 12) from inheriting planner/sensor prio 50.
             * Medium-priority telemetry logger (prio 30) preempts actuator for 420 us! */
            qtg_shim_simulate_preempt(ctx->actuator_pid, 1U, ctx->bg_pid, 1U, 30U, 0U, 420U);
        } else if (ctx->scen == QTG_SCEN_AID_CAP) {
            /* QNX 8.0 PROCMGR_AID_PRIORITY ceiling (63) capped actuator while serving prio-120 client.
             * Medium-priority task at prio 80 preempts actuator for 380 us! */
            qtg_shim_simulate_preempt(ctx->actuator_pid, 1U, ctx->bg_pid, 1U, 80U, 0U, 380U);
        } else {
            qtg_shim_simulate_work(ctx->actuator_pid, 1U, 2U, 22U);
        }

        qtg_ipc_reply_t rep = {
            .type = QTG_MSG_TYPE_ACTUATOR_CMD,
            .status = 0,
            .seq = req.seq,
            .handle_ns = qtg_shim_now_ns(),
            .applied_torque_mNm = req.target_torque_mNm
        };
        qtg_shim_msg_reply(ctx->actuator_pid, 1U, rcvid, 0, &rep, sizeof(rep));
    }

    if (sensor_coid >= 0) qtg_shim_connect_detach(sensor_coid);
    return NULL;
}

static void *planner_thread(void *arg) {
    pipeline_ctx_t *ctx = (pipeline_ctx_t *)arg;
#if defined(__QNX__) || defined(__QNXNTO__)
    uint32_t plan_prio = (ctx->scen == QTG_SCEN_AID_CAP) ? 115U : 45U;
    qtg_shim_register_thread(ctx->planner_pid, 1U, "traj_planner", plan_prio, 1U);
#endif
    int act_coid = qtg_shim_connect_attach(ctx->actuator_pid, ctx->actuator_chid);

    for (uint32_t i = 0; i < ctx->iterations; i++) {
        qtg_ipc_request_t req;
        qtg_shim_msg_info_t info;
        memset(&req, 0, sizeof(req));
        int64_t rcvid = qtg_shim_msg_receive(ctx->planner_pid, 1U, ctx->planner_chid,
                                             &req, sizeof(req), &info, 250U);
        if (rcvid < 0) break;
        if (rcvid == 0) continue;
        if (req.type == QTG_MSG_TYPE_SHUTDOWN) {
            qtg_shim_msg_reply(ctx->planner_pid, 1U, rcvid, 0, NULL, 0);
            break;
        }

        qtg_shim_simulate_work(ctx->planner_pid, 1U, 1U, 18U);
        qtg_ipc_request_t act_req = {
            .type = QTG_MSG_TYPE_ACTUATOR_CMD,
            .seq = req.seq,
            .sensor_distance_mm = req.sensor_distance_mm,
            .target_torque_mNm = (req.sensor_distance_mm < 5000) ? -450 : 120
        };
        qtg_ipc_reply_t act_rep;
        int rc = qtg_shim_msg_send(ctx->planner_pid, 1U, act_coid,
                                   &act_req, sizeof(act_req), &act_rep, sizeof(act_rep),
                                   (ctx->scen == QTG_SCEN_DEADLOCK) ? 45U : 200U);
        if (rc < 0) {
#if defined(__QNX__) || defined(__QNXNTO__)
            (void)qtg_shim_msg_error(ctx->planner_pid, 1U, rcvid, ETIMEDOUT);
#endif
            break;
        }

        qtg_ipc_reply_t rep = {
            .type = QTG_MSG_TYPE_PLAN_COMPUTE,
            .status = 0,
            .seq = req.seq,
            .handle_ns = qtg_shim_now_ns(),
            .applied_torque_mNm = act_rep.applied_torque_mNm
        };
        qtg_shim_msg_reply(ctx->planner_pid, 1U, rcvid, 0, &rep, sizeof(rep));
    }

    qtg_shim_connect_detach(act_coid);
    return NULL;
}

int qtg_run_demo_scenario(qtg_scenario_t scen, const char *kev_out_path, uint32_t iterations) {
    if (iterations == 0U) iterations = 12U;
    if (scen == QTG_SCEN_DEADLOCK) iterations = 1U;

    if (qtg_shim_init(kev_out_path, 4U) != 0) {
        return -1;
    }

    pipeline_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scen = scen;
    ctx.iterations = iterations;

    bool act_has_aid = (scen != QTG_SCEN_AID_CAP);
    uint32_t sensor_prio = (scen == QTG_SCEN_AID_CAP) ? 120U : 50U;
    uint32_t planner_prio = (scen == QTG_SCEN_AID_CAP) ? 115U : 45U;
    uint32_t act_base_prio = 12U;
    uint32_t act_flags = QTG_SHIM_CHF_UNBLOCK;
    if (scen == QTG_SCEN_PRIO_INVERSION) {
        act_flags |= QTG_SHIM_CHF_FIXED_PRIORITY; /* Bug: disables priority inheritance */
    }

    ctx.sensor_pid   = qtg_shim_register_proc(101U, "qtg_sensor_rm",   true);
    ctx.planner_pid  = qtg_shim_register_proc(102U, "qtg_planner_rm",  true);
    ctx.actuator_pid = qtg_shim_register_proc(103U, "qtg_actuator_rm", act_has_aid);
    ctx.bg_pid       = qtg_shim_register_proc(104U, "qtg_telemetry_bg", true);

    qtg_shim_register_thread(ctx.sensor_pid,   1U, "lidar_fusion", sensor_prio, 0U);
#if !defined(__QNX__) && !defined(__QNXNTO__)
    qtg_shim_register_thread(ctx.planner_pid,  1U, "traj_planner", planner_prio, 1U);
    qtg_shim_register_thread(ctx.actuator_pid, 1U, "brake_act",    act_base_prio, 2U);
    qtg_shim_register_thread(ctx.bg_pid,       1U, "diag_logger",
                             (scen == QTG_SCEN_AID_CAP) ? 80U : 30U, 0U);
#else
    (void)planner_prio; (void)act_base_prio;
#endif

#if defined(__QNX__) || defined(__QNXNTO__)
    uint32_t sensor_flags = (scen == QTG_SCEN_DEADLOCK) ? QTG_SHIM_CHF_NONE : QTG_SHIM_CHF_UNBLOCK;
#else
    uint32_t sensor_flags = QTG_SHIM_CHF_UNBLOCK;
#endif
    ctx.sensor_chid   = qtg_shim_channel_create(ctx.sensor_pid, 1U, sensor_flags, "/dev/qtg_sensor");
    ctx.planner_chid  = qtg_shim_channel_create(ctx.planner_pid, 1U, QTG_SHIM_CHF_UNBLOCK, "/dev/qtg_planner");
    ctx.actuator_chid = qtg_shim_channel_create(ctx.actuator_pid, 1U, act_flags, "/dev/qtg_actuator");

    if (scen == QTG_SCEN_HEALTHY) {
        qtg_shim_configure_sporadic(ctx.bg_pid, 1U, 30U, 10U, 100U, 15U, 4U);
    }

    if (scen == QTG_SCEN_APS_LEGACY) {
        qtg_shim_trace_aps_legacy_record(0U, 0U, "System",       25U,  0U, 100U, false);
        qtg_shim_trace_aps_legacy_record(0U, 1U, "ADAS_Control", 50U, 15U, 100U, true);
        qtg_shim_trace_aps_legacy_record(0U, 2U, "Infotainment", 25U,  0U, 100U, false);
    }

    if (scen == QTG_SCEN_PULSE_STORM) {
        int coid = qtg_shim_connect_attach(ctx.actuator_pid, ctx.actuator_chid);
        for (uint32_t i = 0; i < 40U; i++) {
            qtg_shim_msg_send_pulse(ctx.sensor_pid, 1U, coid, 40, QTG_SHIM_PULSE_CODE_TICK, i);
        }
        for (uint32_t i = 0; i < 4U; i++) {
            qtg_shim_pulse_t p;
            (void)qtg_shim_msg_receive(ctx.actuator_pid, 1U, ctx.actuator_chid, &p, sizeof(p), NULL, 20U);
        }
        qtg_shim_connect_detach(coid);
        qtg_shim_shutdown();
        return 0;
    }

    pthread_t th_act, th_plan;
    if (pthread_create(&th_act, NULL, actuator_thread, &ctx) != 0) {
        qtg_shim_shutdown();
        return -1;
    }
    if (pthread_create(&th_plan, NULL, planner_thread, &ctx) != 0) {
        pthread_join(th_act, NULL);
        qtg_shim_shutdown();
        return -1;
    }

    int plan_coid = qtg_shim_connect_attach(ctx.planner_pid, ctx.planner_chid);
    for (uint32_t i = 0; i < iterations; i++) {
        qtg_shim_trace_marker(0U, QTG_USER_MARKER, (uint32_t)scen, i);
        qtg_ipc_request_t req = {
            .type = QTG_MSG_TYPE_PLAN_COMPUTE,
            .seq = i + 1U,
            .emit_ns = qtg_shim_now_ns(),
            .sensor_distance_mm = 3200 + (int32_t)(i * 150U)
        };
        qtg_ipc_reply_t rep;
        (void)qtg_shim_msg_send(ctx.sensor_pid, 1U, plan_coid,
                                &req, sizeof(req), &rep, sizeof(rep),
                                (scen == QTG_SCEN_DEADLOCK) ? 35U : 200U);
    }
    qtg_shim_connect_detach(plan_coid);

    pthread_join(th_plan, NULL);
    pthread_join(th_act, NULL);
    qtg_shim_shutdown();
    return 0;
}

int qtg_parse_scenario_name(const char *name, qtg_scenario_t *out) {
    if (!name || !out) return -1;
    if (strcmp(name, "healthy") == 0)         { *out = QTG_SCEN_HEALTHY; return 0; }
    if (strcmp(name, "deadlock") == 0)        { *out = QTG_SCEN_DEADLOCK; return 0; }
    if (strcmp(name, "prio-inversion") == 0)  { *out = QTG_SCEN_PRIO_INVERSION; return 0; }
    if (strcmp(name, "aid-cap") == 0)         { *out = QTG_SCEN_AID_CAP; return 0; }
    if (strcmp(name, "pulse-storm") == 0)     { *out = QTG_SCEN_PULSE_STORM; return 0; }
    if (strcmp(name, "aps-legacy") == 0)      { *out = QTG_SCEN_APS_LEGACY; return 0; }
    return -1;
}

const char *qtg_scenario_name(qtg_scenario_t scen) {
    switch (scen) {
        case QTG_SCEN_HEALTHY:        return "healthy";
        case QTG_SCEN_DEADLOCK:       return "deadlock";
        case QTG_SCEN_PRIO_INVERSION: return "prio-inversion";
        case QTG_SCEN_AID_CAP:        return "aid-cap";
        case QTG_SCEN_PULSE_STORM:    return "pulse-storm";
        case QTG_SCEN_APS_LEGACY:     return "aps-legacy";
        default:                      return "unknown";
    }
}

#if defined(QTG_BUILD_RESMGR_BIN)

#if defined(__QNX__) || defined(__QNXNTO__)
/* Genuine QNX OS 8.0 Resource Manager daemon exposing /dev/qtg_sensor */
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t      g_io_funcs;
static iofunc_attr_t          g_attr;
static uint32_t               g_read_counter = 0U;

static int qtg_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if (ocb->offset > 0) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    char payload[128];
    uint32_t seq = ++g_read_counter;
    int full_len = snprintf(payload, sizeof(payload),
                            "qtg_sensor seq=%u distance_mm=%u qnx8=ok\n",
                            seq, 3200U + (seq % 500U));
    if (full_len < 0) full_len = 0;
    size_t req_bytes = (size_t)_IO_READ_GET_NBYTES(msg);
    size_t to_send = ((size_t)full_len < req_bytes) ? (size_t)full_len : req_bytes;
    TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, QTG_USER_MARKER, 0x52454144U, seq);
    MsgReply(ctp->rcvid, (int64_t)to_send, payload, to_send);
    ocb->offset += (int)to_send;
    return _RESMGR_NOREPLY;
}

static int qtg_io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) return status;
    uint32_t *val = (uint32_t *)_DEVCTL_DATA(msg->i);
    if (msg->i.dcmd == QTG_DCMD_GET_HEALTH) {
        *val = g_read_counter;
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.nbytes = sizeof(uint32_t);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(uint32_t));
    }
    return ENOSYS;
}

static int run_qnx8_resmgr_daemon(const char *dev_path) {
    dispatch_t *dpp = dispatch_create();
    if (!dpp) { perror("dispatch_create"); return 1; }

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 2;
    rattr.msg_max_size = 2048;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs,
                     _RESMGR_IO_NFUNCS, &g_io_funcs);
    g_io_funcs.read   = qtg_io_read;
    g_io_funcs.devctl = qtg_io_devctl;
    iofunc_attr_init(&g_attr, S_IFCHR | 0666, NULL, NULL);

    if (resmgr_attach(dpp, &rattr, dev_path, _FTYPE_ANY, 0,
                      &g_connect_funcs, &g_io_funcs, &g_attr) == -1) {
        perror("resmgr_attach");
        return 1;
    }

    /* Drop root while retaining PROCMGR_AID_PRIORITY per QNX 8.0 least-privilege best practice */
    (void)qtg_shim_drop_privileges_keep_priority(true);

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    if (!ctp) { perror("dispatch_context_alloc"); return 1; }

    printf("[qtg-resmgr-node] QNX 8.0 Resource Manager mounted at %s\n", dev_path);
    fflush(stdout);
    while (1) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            perror("dispatch_block");
            return 1;
        }
        dispatch_handler(ctp);
    }
    return 0;
}

/* ===================== GENUINE MULTI-PROCESS QNX OS 8.0 PIPELINE =====================
 * Spawns 3-4 distinct QNX OS 8.0 processes (`qtg_sensor_rm`, `qtg_planner_rm`,
 * `qtg_actuator_rm`, and `qtg_telemetry_bg`) via fork()+execv() so procnto-smp-instr
 * emits distinct PIDs and real PROCCREATE_NAME records in tracelogger .kev streams,
 * exercises real resmgr_attach("/dev/qtg_sensor"), and reproduces genuine non-root
 * setuid(1000) PROCMGR_AID_PRIORITY ceiling-63 inheritance capping! */

typedef struct qtg_mp_reg {
    uint32_t role; /* 1=actuator, 2=planner, 3=bg */
    uint32_t pid;
    int32_t  chid;
} qtg_mp_reg_t;

typedef struct qtg_mp_cfg {
    uint32_t sensor_pid;
    int32_t  sensor_chid;
    uint32_t actuator_pid;
    int32_t  actuator_chid;
    uint32_t bg_pid;
    int32_t  bg_chid;
} qtg_mp_cfg_t;

static int run_mp_bg_process(qtg_scenario_t scen, uint32_t parent_pid, int boot_chid) {
    uint32_t bg_prio = (scen == QTG_SCEN_AID_CAP) ? 80U : 30U;
    qtg_shim_register_thread((uint32_t)getpid(), 1U, "diag_logger", bg_prio, 0U);
    /* Pin preemptor to CPU 0 so it preempts the server on CPU 0 cleanly */
    (void)ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1U);
    /* Fixed-priority trigger channel keeps bg_prio (30/80) even when triggered by non-root pulse */
    int ch = ChannelCreate(_NTO_CHF_FIXED_PRIORITY);
    int bc = qtg_shim_connect_attach(parent_pid, boot_chid);
    qtg_mp_reg_t reg = { .role = 3U, .pid = (uint32_t)getpid(), .chid = ch };
    qtg_mp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    (void)qtg_shim_msg_send((uint32_t)getpid(), 1U, bc, &reg, sizeof(reg), &cfg, sizeof(cfg), 1000U);
    qtg_shim_connect_detach(bc);

    /* Wait for trigger pulses from actuator to fire deterministic 420us / 380us preemption */
    for (uint32_t k = 0; k < 64U; k++) {
        struct _pulse p;
        memset(&p, 0, sizeof(p));
        int64_t rcvid = qtg_shim_msg_receive((uint32_t)getpid(), 1U, ch, &p, sizeof(p), NULL, 400U);
        if (rcvid < 0) break;
        if (rcvid == 0) {
            if (p.code == QTG_SHIM_PULSE_CODE_PING) {
                uint32_t dur_us = (uint32_t)p.value.sival_int;
                if (dur_us == 0U || dur_us > 2000U) dur_us = 400U;
                uint64_t deadline = qtg_shim_now_ns() + (uint64_t)dur_us * 1000ULL;
                while (qtg_shim_now_ns() < deadline) {
                    /* hold CPU 0 at prio 30 or 80 while victim is READY on CPU 0 */
                }
            } else if (p.code == QTG_SHIM_PULSE_CODE_TICK) {
                break; /* shutdown pulse */
            }
        } else {
            (void)qtg_shim_msg_reply((uint32_t)getpid(), 1U, rcvid, 0, NULL, 0);
            break;
        }
    }
    return 0;
}

static int run_mp_actuator_process(qtg_scenario_t scen, uint32_t iterations,
                                   uint32_t parent_pid, int boot_chid) {
    qtg_shim_register_thread((uint32_t)getpid(), 1U, "brake_act", 12U, 0U);
    if (scen == QTG_SCEN_PRIO_INVERSION || scen == QTG_SCEN_AID_CAP) {
        (void)ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1U);
    }
    if (scen == QTG_SCEN_AID_CAP) {
        /* GENUINE QNX OS 8.0 non-root privilege drop WITHOUT PROCMGR_AID_PRIORITY:
         * procnto-smp-instr itself caps inherited priority at exactly 63! */
        (void)qtg_shim_drop_privileges_keep_priority(false);
    }
    uint32_t ch_flags = QTG_SHIM_CHF_UNBLOCK;
    if (scen == QTG_SCEN_PRIO_INVERSION) ch_flags |= QTG_SHIM_CHF_FIXED_PRIORITY;
    int ch = qtg_shim_channel_create((uint32_t)getpid(), 1U, ch_flags, "/dev/qtg_actuator");

    int bc = qtg_shim_connect_attach(parent_pid, boot_chid);
    qtg_mp_reg_t reg = { .role = 1U, .pid = (uint32_t)getpid(), .chid = ch };
    qtg_mp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    (void)qtg_shim_msg_send((uint32_t)getpid(), 1U, bc, &reg, sizeof(reg), &cfg, sizeof(cfg), 1000U);
    qtg_shim_connect_detach(bc);

    if (scen == QTG_SCEN_PULSE_STORM) {
        /* Drain only 4 of the 40 pulses sent by qtg_sensor_rm -> real kernel backlog */
        qtg_sleep_ms(25U);
        for (uint32_t i = 0; i < 4U; i++) {
            struct _pulse p;
            (void)qtg_shim_msg_receive((uint32_t)getpid(), 1U, ch, &p, sizeof(p), NULL, 100U);
        }
        return 0;
    }

    int sensor_coid = -1;
    int bg_coid = -1;
    if (scen == QTG_SCEN_DEADLOCK) {
        sensor_coid = qtg_shim_connect_attach(cfg.sensor_pid, cfg.sensor_chid);
    }
    if ((scen == QTG_SCEN_PRIO_INVERSION || scen == QTG_SCEN_AID_CAP) && cfg.bg_pid != 0U) {
        bg_coid = qtg_shim_connect_attach(cfg.bg_pid, cfg.bg_chid);
    }

    for (uint32_t i = 0; i < iterations; i++) {
        qtg_ipc_request_t req;
        qtg_shim_msg_info_t info;
        memset(&req, 0, sizeof(req));
        int64_t rcvid = qtg_shim_msg_receive((uint32_t)getpid(), 1U, ch, &req, sizeof(req), &info, 400U);
        if (rcvid < 0) break;
        if (rcvid == 0) continue;
        if (req.type == QTG_MSG_TYPE_SHUTDOWN) {
            (void)qtg_shim_msg_reply((uint32_t)getpid(), 1U, rcvid, 0, NULL, 0);
            break;
        }
        if (scen == QTG_SCEN_DEADLOCK) {
            qtg_ipc_request_t cb = { .type = QTG_MSG_TYPE_STATUS_QUERY, .seq = req.seq };
            qtg_ipc_reply_t cb_rep;
            (void)qtg_shim_msg_send((uint32_t)getpid(), 1U, sensor_coid,
                                    &cb, sizeof(cb), &cb_rep, sizeof(cb_rep), 75U);
            (void)qtg_shim_msg_error((uint32_t)getpid(), 1U, rcvid, ETIMEDOUT);
            break;
        } else if ((scen == QTG_SCEN_PRIO_INVERSION || scen == QTG_SCEN_AID_CAP) && bg_coid >= 0) {
            uint32_t dur_us = (scen == QTG_SCEN_AID_CAP) ? 380U : 420U;
            /* Fire pulse with priority=-1 (valid even as non-root uid=1000!) to fixed-prio bg on CPU 0 */
            (void)qtg_shim_msg_send_pulse((uint32_t)getpid(), 1U, bg_coid, -1,
                                          QTG_SHIM_PULSE_CODE_PING, dur_us);
            uint64_t end = qtg_shim_now_ns() + (uint64_t)dur_us * 1000ULL + 60000ULL;
            while (qtg_shim_now_ns() < end) {
                /* stay runnable on CPU 0 while preempted by qtg_telemetry_bg */
            }
        } else {
            qtg_shim_simulate_work((uint32_t)getpid(), 1U, 2U, 22U);
        }
        qtg_ipc_reply_t rep = {
            .type = QTG_MSG_TYPE_ACTUATOR_CMD,
            .status = 0,
            .seq = req.seq,
            .handle_ns = qtg_shim_now_ns(),
            .applied_torque_mNm = req.target_torque_mNm
        };
        (void)qtg_shim_msg_reply((uint32_t)getpid(), 1U, rcvid, 0, &rep, sizeof(rep));
    }

    if (bg_coid >= 0) {
        (void)qtg_shim_msg_send_pulse((uint32_t)getpid(), 1U, bg_coid, -1, QTG_SHIM_PULSE_CODE_TICK, 0U);
        qtg_shim_connect_detach(bg_coid);
    }
    if (sensor_coid >= 0) qtg_shim_connect_detach(sensor_coid);
    return 0;
}

static int run_mp_planner_process(qtg_scenario_t scen, uint32_t iterations,
                                  uint32_t parent_pid, int boot_chid) {
    uint32_t plan_prio = (scen == QTG_SCEN_AID_CAP) ? 115U : 45U;
    qtg_shim_register_thread((uint32_t)getpid(), 1U, "traj_planner", plan_prio, 1U);
    int ch = qtg_shim_channel_create((uint32_t)getpid(), 1U, QTG_SHIM_CHF_UNBLOCK, "/dev/qtg_planner");

    int bc = qtg_shim_connect_attach(parent_pid, boot_chid);
    qtg_mp_reg_t reg = { .role = 2U, .pid = (uint32_t)getpid(), .chid = ch };
    qtg_mp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    (void)qtg_shim_msg_send((uint32_t)getpid(), 1U, bc, &reg, sizeof(reg), &cfg, sizeof(cfg), 1000U);
    qtg_shim_connect_detach(bc);

    int act_coid = qtg_shim_connect_attach(cfg.actuator_pid, cfg.actuator_chid);
    for (uint32_t i = 0; i < iterations; i++) {
        qtg_ipc_request_t req;
        qtg_shim_msg_info_t info;
        memset(&req, 0, sizeof(req));
        int64_t rcvid = qtg_shim_msg_receive((uint32_t)getpid(), 1U, ch, &req, sizeof(req), &info, 400U);
        if (rcvid < 0) break;
        if (rcvid == 0) continue;
        if (req.type == QTG_MSG_TYPE_SHUTDOWN) {
            (void)qtg_shim_msg_reply((uint32_t)getpid(), 1U, rcvid, 0, NULL, 0);
            break;
        }
        qtg_shim_simulate_work((uint32_t)getpid(), 1U, 1U, 18U);
        qtg_ipc_request_t act_req = {
            .type = QTG_MSG_TYPE_ACTUATOR_CMD,
            .seq = req.seq,
            .sensor_distance_mm = req.sensor_distance_mm,
            .target_torque_mNm = (req.sensor_distance_mm < 5000) ? -450 : 120
        };
        qtg_ipc_reply_t act_rep;
        int rc = qtg_shim_msg_send((uint32_t)getpid(), 1U, act_coid,
                                   &act_req, sizeof(act_req), &act_rep, sizeof(act_rep),
                                   (scen == QTG_SCEN_DEADLOCK) ? 45U : 250U);
        if (rc < 0) {
            (void)qtg_shim_msg_error((uint32_t)getpid(), 1U, rcvid, ETIMEDOUT);
            break;
        }
        qtg_ipc_reply_t rep = {
            .type = QTG_MSG_TYPE_PLAN_COMPUTE,
            .status = 0,
            .seq = req.seq,
            .handle_ns = qtg_shim_now_ns(),
            .applied_torque_mNm = act_rep.applied_torque_mNm
        };
        (void)qtg_shim_msg_reply((uint32_t)getpid(), 1U, rcvid, 0, &rep, sizeof(rep));
    }
    qtg_shim_connect_detach(act_coid);
    return 0;
}

static pid_t spawn_mp_child(const char *self_bin, const char *alias_name,
                            const char *role, qtg_scenario_t scen,
                            uint32_t iters, uint32_t parent_pid, int boot_chid) {
    char link_path[128];
    snprintf(link_path, sizeof(link_path), "/tmp/%s", alias_name);
    (void)unlink(link_path);
    const char *exec_path = (symlink(self_bin, link_path) == 0) ? link_path : self_bin;

    pid_t pid = fork();
    if (pid == 0) {
        char s_iters[24], s_ppid[24], s_bch[24];
        snprintf(s_iters, sizeof(s_iters), "%u", iters);
        snprintf(s_ppid, sizeof(s_ppid), "%u", parent_pid);
        snprintf(s_bch, sizeof(s_bch), "%d", boot_chid);
        char *child_argv[12];
        int k = 0;
        child_argv[k++] = (char *)alias_name;
        child_argv[k++] = (char *)"--role";
        child_argv[k++] = (char *)role;
        child_argv[k++] = (char *)"--scenario";
        child_argv[k++] = (char *)qtg_scenario_name(scen);
        child_argv[k++] = (char *)"--iters";
        child_argv[k++] = s_iters;
        child_argv[k++] = (char *)"--parent-pid";
        child_argv[k++] = s_ppid;
        child_argv[k++] = (char *)"--boot-chid";
        child_argv[k++] = s_bch;
        child_argv[k++] = NULL;
        execv(exec_path, child_argv);
        _exit(127);
    }
    return pid;
}

static int run_qnx8_multiprocess_scenario(const char *self_bin, qtg_scenario_t scen, uint32_t iterations) {
    if (iterations == 0U) iterations = 12U;
    if (scen == QTG_SCEN_DEADLOCK) iterations = 1U;
    (void)qtg_shim_init(NULL, 4U);

    int boot_chid = ChannelCreate(0);
    uint32_t sensor_flags = (scen == QTG_SCEN_DEADLOCK) ? QTG_SHIM_CHF_NONE : QTG_SHIM_CHF_UNBLOCK;
    int sensor_chid = qtg_shim_channel_create((uint32_t)getpid(), 1U, sensor_flags, "/dev/qtg_sensor");

    /* In healthy mode, start real /dev/qtg_sensor resmgr and verify open/read before IPC bench */
    pid_t rm_pid = -1;
    if (scen == QTG_SCEN_HEALTHY) {
        rm_pid = fork();
        if (rm_pid == 0) {
            _exit(run_qnx8_resmgr_daemon("/dev/qtg_sensor"));
        }
    }

    bool need_bg = (scen == QTG_SCEN_PRIO_INVERSION || scen == QTG_SCEN_AID_CAP);
    bool need_plan = (scen != QTG_SCEN_PULSE_STORM);

    qtg_mp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sensor_pid = (uint32_t)getpid();
    cfg.sensor_chid = sensor_chid;
    int plan_chid = -1;

    /* Spawn sequentially and reply immediately to each bootstrap MsgReceive so no child
     * ever sits REPLY-blocked waiting on another child's execv()! */
    pid_t bg_pid = -1, act_pid = -1, plan_pid = -1;
    if (need_bg) {
        bg_pid = spawn_mp_child(self_bin, "qtg_telemetry_bg", "bg", scen, iterations, (uint32_t)getpid(), boot_chid);
        qtg_mp_reg_t reg;
        int64_t rcvid = qtg_shim_msg_receive((uint32_t)getpid(), 1U, boot_chid, &reg, sizeof(reg), NULL, 2000U);
        if (rcvid > 0) {
            cfg.bg_pid = reg.pid; cfg.bg_chid = reg.chid;
            (void)qtg_shim_msg_reply((uint32_t)getpid(), 1U, rcvid, 0, &cfg, sizeof(cfg));
        }
    }
    act_pid = spawn_mp_child(self_bin, "qtg_actuator_rm", "actuator", scen, iterations, (uint32_t)getpid(), boot_chid);
    {
        qtg_mp_reg_t reg;
        int64_t rcvid = qtg_shim_msg_receive((uint32_t)getpid(), 1U, boot_chid, &reg, sizeof(reg), NULL, 2000U);
        if (rcvid > 0) {
            cfg.actuator_pid = reg.pid; cfg.actuator_chid = reg.chid;
            (void)qtg_shim_msg_reply((uint32_t)getpid(), 1U, rcvid, 0, &cfg, sizeof(cfg));
        }
    }
    if (need_plan) {
        plan_pid = spawn_mp_child(self_bin, "qtg_planner_rm", "planner", scen, iterations, (uint32_t)getpid(), boot_chid);
        qtg_mp_reg_t reg;
        int64_t rcvid = qtg_shim_msg_receive((uint32_t)getpid(), 1U, boot_chid, &reg, sizeof(reg), NULL, 2000U);
        if (rcvid > 0) {
            plan_chid = reg.chid;
            (void)qtg_shim_msg_reply((uint32_t)getpid(), 1U, rcvid, 0, &cfg, sizeof(cfg));
        }
    }

    /* Raise sensor to safety priority AFTER children finish execv() and bootstrap */
    uint32_t sensor_prio = (scen == QTG_SCEN_AID_CAP) ? 120U : 50U;
    qtg_shim_register_thread((uint32_t)getpid(), 1U, "lidar_fusion", sensor_prio, 0U);
    qtg_sleep_ms(5U);

    /* Exercise real /dev/qtg_sensor iofunc_read via POSIX open/read/close */
    if (rm_pid > 0) {
        for (int tries = 0; tries < 10; tries++) {
            int fd = open("/dev/qtg_sensor", O_RDONLY);
            if (fd >= 0) {
                char rbuf[64];
                (void)read(fd, rbuf, sizeof(rbuf));
                close(fd);
                break;
            }
            qtg_sleep_ms(5U);
        }
    }

    if (scen == QTG_SCEN_PULSE_STORM && cfg.actuator_pid != 0U) {
        int coid = qtg_shim_connect_attach(cfg.actuator_pid, cfg.actuator_chid);
        for (uint32_t i = 0; i < 40U; i++) {
            (void)qtg_shim_msg_send_pulse((uint32_t)getpid(), 1U, coid, 40, QTG_SHIM_PULSE_CODE_TICK, i);
        }
        qtg_shim_connect_detach(coid);
    } else if (plan_pid > 0 && plan_chid >= 0) {
        int plan_coid = qtg_shim_connect_attach((uint32_t)plan_pid, plan_chid);
        for (uint32_t i = 0; i < iterations; i++) {
            qtg_shim_trace_marker(0U, QTG_USER_MARKER, (uint32_t)scen, i);
            qtg_ipc_request_t req = {
                .type = QTG_MSG_TYPE_PLAN_COMPUTE,
                .seq = i + 1U,
                .emit_ns = qtg_shim_now_ns(),
                .sensor_distance_mm = 3200 + (int32_t)(i * 150U)
            };
            qtg_ipc_reply_t rep;
            (void)qtg_shim_msg_send((uint32_t)getpid(), 1U, plan_coid,
                                    &req, sizeof(req), &rep, sizeof(rep),
                                    (scen == QTG_SCEN_DEADLOCK) ? 35U : 250U);
        }
        qtg_shim_connect_detach(plan_coid);
    }

    if (rm_pid > 0) {
        (void)kill(rm_pid, SIGTERM);
        (void)waitpid(rm_pid, NULL, 0);
    }
    if (plan_pid > 0) (void)waitpid(plan_pid, NULL, 0);
    if (act_pid > 0) (void)waitpid(act_pid, NULL, 0);
    if (bg_pid > 0) (void)waitpid(bg_pid, NULL, 0);
    qtg_shim_shutdown();
    return 0;
}
#endif

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s --scenario <healthy|deadlock|prio-inversion|aid-cap|pulse-storm|aps-legacy>\n"
        "          [--out trace.kev] [--iters N]\n"
#if defined(__QNX__) || defined(__QNXNTO__)
        "       %s --resmgr-daemon [/dev/qtg_sensor]\n"
#endif
        , argv0
#if defined(__QNX__) || defined(__QNXNTO__)
        , argv0
#endif
    );
}

int main(int argc, char **argv) {
    qtg_scenario_t scen = QTG_SCEN_HEALTHY;
    const char *out_path = NULL;
    uint32_t iters = 12U;
#if defined(__QNX__) || defined(__QNXNTO__)
    const char *role = NULL;
    uint32_t parent_pid = 0U;
    int boot_chid = -1;
    bool single_proc = false;
#endif

    for (int i = 1; i < argc; i++) {
#if defined(__QNX__) || defined(__QNXNTO__)
        if (strcmp(argv[i], "--resmgr-daemon") == 0) {
            const char *path = (i + 1 < argc) ? argv[i + 1] : "/dev/qtg_sensor";
            return run_qnx8_resmgr_daemon(path);
        } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            role = argv[++i];
        } else if (strcmp(argv[i], "--parent-pid") == 0 && i + 1 < argc) {
            parent_pid = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--boot-chid") == 0 && i + 1 < argc) {
            boot_chid = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--single-process") == 0) {
            single_proc = true;
        } else
#endif
        if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            if (qtg_parse_scenario_name(argv[++i], &scen) != 0) {
                fprintf(stderr, "Unknown scenario: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

#if defined(__QNX__) || defined(__QNXNTO__)
    if (role) {
        if (strcmp(role, "actuator") == 0) return run_mp_actuator_process(scen, iters, parent_pid, boot_chid);
        if (strcmp(role, "planner") == 0)  return run_mp_planner_process(scen, iters, parent_pid, boot_chid);
        if (strcmp(role, "bg") == 0)       return run_mp_bg_process(scen, parent_pid, boot_chid);
        return 1;
    }
    if (!single_proc && scen != QTG_SCEN_APS_LEGACY) {
        printf("[qtg-resmgr-node] Running multi-process QNX 8.0 scenario '%s' (iters=%u)\n",
               qtg_scenario_name(scen), iters);
        return run_qnx8_multiprocess_scenario(argv[0], scen, iters);
    }
#endif

    printf("[qtg-resmgr-node] Running scenario '%s' (iters=%u, out=%s)\n",
           qtg_scenario_name(scen), iters, out_path ? out_path : "<qnx-tracelogger>");
    if (qtg_run_demo_scenario(scen, out_path, iters) != 0) {
        fprintf(stderr, "[qtg-resmgr-node] Scenario failed.\n");
        return 1;
    }
    printf("[qtg-resmgr-node] Scenario '%s' complete.\n", qtg_scenario_name(scen));
    return 0;
}
#endif
