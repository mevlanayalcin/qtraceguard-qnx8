/*
 * QTraceGuard - LibFuzzer Harness for Binary .kev, traceprinter, Analyzer & JSON/SARIF
 * SPDX-License-Identifier: MIT
 *
 * Build & run with Clang LibFuzzer + ASan + UBSan:
 *   make fuzz
 */

#include "qtg_analyzer.h"
#include "qtg_kev.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size == 0U || size > (1U << 20)) {
        return 0;
    }

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};

    int rc = qtg_kev_read_buffer(data, size, &tr, err, sizeof(err));
    if (rc != 0 && size <= 65536U) {
        /* Exercise traceprinter plain-text parser on non-binary inputs */
        rc = qtg_traceprinter_read_buffer((const char *)data, size, &tr, err, sizeof(err));
    }

    if (rc == 0) {
        qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
        if (rep) {
            qtg_sla_policy_t pol;
            qtg_sla_policy_defaults(&pol);
            (void)qtg_analyze_trace(&tr, &pol, rep);
            /* Exercise JSON and SARIF 2.1.0 serializers/escapers */
            (void)qtg_report_write_json(rep, "/dev/null");
            (void)qtg_report_write_sarif(rep, "fuzz.kev", "/dev/null");
            free(rep);
        }
    }

    qtg_trace_free(&tr);
    return 0;
}
