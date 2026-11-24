/*
 * QTraceGuard - QNX 8.0 Autonomous-Control Resource Manager Demo Protocol
 * SPDX-License-Identifier: MIT
 *
 * Models a classic QNX safety-critical microkernel pipeline:
 *   /dev/qtg_sensor   (LiDAR/IMU frame fusion resource manager)
 *   /dev/qtg_planner  (Trajectory & brake planner resource manager)
 *   /dev/qtg_actuator (Drive-by-wire torque/brake actuator resource manager)
 *
 * Supports 6 reproducible test scenarios for both QNX OS 8.0 (QEMU / RPi5) and
 * portable POSIX CI simulation:
 *   1. healthy         - Deterministic DAG IPC + proper priority inheritance + SCHED_SPORADIC
 *   2. deadlock        - 3-node synchronous MsgSend cycle (sensor -> planner -> actuator -> sensor)
 *   3. prio-inversion  - _NTO_CHF_FIXED_PRIORITY disables inheritance; mid-prio task preempts server
 *   4. aid-cap         - QNX 8.0 PROCMGR_AID_PRIORITY ceiling (63) caps prio-120 safety client
 *   5. pulse-storm     - High-rate MsgSendPulse saturating a blocked resource manager channel
 *   6. aps-legacy      - Legacy QNX 7.1 APS trace workload for QNX 8.0 SCHED_SPORADIC migration
 */

#ifndef QTG_RESMGR_DEMO_H
#define QTG_RESMGR_DEMO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QTG_MSG_TYPE_SENSOR_SAMPLE  0x1001U
#define QTG_MSG_TYPE_PLAN_COMPUTE   0x1002U
#define QTG_MSG_TYPE_ACTUATOR_CMD   0x1003U
#define QTG_MSG_TYPE_STATUS_QUERY   0x1004U
#define QTG_MSG_TYPE_SHUTDOWN       0x10FFU

#define QTG_DEVCTL_CMD_GET_HEALTH   0x0001U
#define QTG_DEVCTL_CMD_INJECT_FAULT 0x0002U

typedef struct qtg_ipc_request {
    uint16_t type;
    uint16_t flags;
    uint32_t seq;
    uint64_t emit_ns;
    int32_t  sensor_distance_mm;
    int32_t  target_torque_mNm;
} qtg_ipc_request_t;

typedef struct qtg_ipc_reply {
    uint16_t type;
    uint16_t status;
    uint32_t seq;
    uint64_t handle_ns;
    int32_t  applied_torque_mNm;
    int32_t  reserved;
} qtg_ipc_reply_t;

typedef enum qtg_scenario {
    QTG_SCEN_HEALTHY = 0,
    QTG_SCEN_DEADLOCK = 1,
    QTG_SCEN_PRIO_INVERSION = 2,
    QTG_SCEN_AID_CAP = 3,
    QTG_SCEN_PULSE_STORM = 4,
    QTG_SCEN_APS_LEGACY = 5
} qtg_scenario_t;

int qtg_run_demo_scenario(qtg_scenario_t scen, const char *kev_out_path, uint32_t iterations);
int qtg_parse_scenario_name(const char *name, qtg_scenario_t *out);
const char *qtg_scenario_name(qtg_scenario_t scen);

#ifdef __cplusplus
}
#endif

#endif /* QTG_RESMGR_DEMO_H */
