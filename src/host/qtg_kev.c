/*
 * QTraceGuard - Clean-room QNX Kernel Event (.kev) binary format codec
 * SPDX-License-Identifier: MIT
 */

#include "qtg_kev.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint32_t qtg_bswap32(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) << 8)  |
           ((v & 0x00FF0000U) >> 8)  |
           ((v & 0xFF000000U) >> 24);
}

static bool qtg_host_is_little_endian(void) {
    const uint16_t probe = 0x0102U;
    return (*((const uint8_t *)&probe) == 0x02U);
}

static uint32_t qtg_read_u32(const uint8_t *p, bool file_le) {
    uint32_t v = 0;
    memcpy(&v, p, sizeof(v));
    if (file_le != qtg_host_is_little_endian()) {
        v = qtg_bswap32(v);
    }
    return v;
}

static void qtg_write_u32(uint8_t *p, uint32_t v, bool file_le) {
    if (file_le != qtg_host_is_little_endian()) {
        v = qtg_bswap32(v);
    }
    memcpy(p, &v, sizeof(v));
}

uint64_t qtg_cycles_to_ns(uint64_t cycles, uint64_t cycles_per_sec) {
    if (cycles_per_sec == 0ULL) {
        return cycles;
    }
    uint64_t secs = cycles / cycles_per_sec;
    uint64_t rem  = cycles % cycles_per_sec;
    /* Guard against uint64_t overflow on hostile/tiny cycles_per_sec headers */
    if (secs >= (UINT64_MAX / 1000000000ULL)) {
        return UINT64_MAX;
    }
    uint64_t ns_rem = 0;
    if (rem <= (UINT64_MAX / 1000000000ULL)) {
        ns_rem = (rem * 1000000000ULL) / cycles_per_sec;
    } else {
        double frac = (double)rem / (double)cycles_per_sec;
        ns_rem = (uint64_t)(frac * 1e9);
    }
    uint64_t base = secs * 1000000000ULL;
    return (ns_rem > UINT64_MAX - base) ? UINT64_MAX : (base + ns_rem);
}

void qtg_trace_init(qtg_trace_t *tr) {
    if (!tr) return;
    memset(tr, 0, sizeof(*tr));
    qtg_kev_header_defaults(&tr->header);
}

void qtg_trace_free(qtg_trace_t *tr) {
    if (!tr) return;
    free(tr->events);
    tr->events = NULL;
    tr->count = 0;
    tr->capacity = 0;
}

int qtg_trace_push(qtg_trace_t *tr, const qtg_event_t *ev) {
    if (!tr || !ev) return -1;
    if (tr->count == tr->capacity) {
        if (tr->capacity > (SIZE_MAX / 2U) || (tr->capacity > 0 && tr->capacity > (SIZE_MAX / sizeof(qtg_event_t)) / 2U)) {
            return -1;
        }
        size_t new_cap = (tr->capacity == 0) ? 256U : (tr->capacity * 2U);
        if (new_cap > 4194304U) {
            return -1; /* Hard cap: 4M events (~1.2 GB) protects against hostile trace bombs */
        }
        qtg_event_t *next = (qtg_event_t *)realloc(tr->events, new_cap * sizeof(qtg_event_t));
        if (!next) return -1;
        tr->events = next;
        tr->capacity = new_cap;
    }
    tr->events[tr->count++] = *ev;
    return 0;
}

void qtg_kev_header_defaults(qtg_kev_header_t *hdr) {
    if (!hdr) return;
    memset(hdr, 0, sizeof(*hdr));
    snprintf(hdr->file_name, sizeof(hdr->file_name), "/dev/shmem/tracebuffer.kev");
    snprintf(hdr->date, sizeof(hdr->date), "2026-11-23T00:00:00Z");
    snprintf(hdr->sysname, sizeof(hdr->sysname), "QNX");
    snprintf(hdr->nodename, sizeof(hdr->nodename), "qnx8-qemu-target");
    snprintf(hdr->sys_release, sizeof(hdr->sys_release), "8.0.0");
    snprintf(hdr->machine, sizeof(hdr->machine), "x86_64");
    snprintf(hdr->tracelogger_args, sizeof(hdr->tracelogger_args),
             "tracelogger -w -n 4 -f /dev/shmem/tracebuffer.kev");
    hdr->ver_major = 1U;
    hdr->ver_minor = 1U;
    hdr->little_endian = true;
    hdr->cycles_per_sec = 1000000000ULL; /* 1 GHz => 1 cycle == 1 ns */
    hdr->cpu_num = 4U;
    hdr->syspage_len = 0U;
}

static void qtg_trim_copy(char *dst, size_t dst_len, const char *src, size_t src_len) {
    while (src_len > 0 && (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n')) {
        src++;
        src_len--;
    }
    while (src_len > 0 && (src[src_len - 1] == ' ' || src[src_len - 1] == '\t' ||
                           src[src_len - 1] == '\r' || src[src_len - 1] == '\n' ||
                           src[src_len - 1] == '\0')) {
        src_len--;
    }
    if (src_len >= dst_len) {
        src_len = dst_len - 1U;
    }
    if (src_len > 0) {
        memcpy(dst, src, src_len);
    }
    dst[src_len] = '\0';
}

static void qtg_apply_header_kv(qtg_kev_header_t *hdr, const char *key, const char *val) {
    size_t vlen = strlen(val);
    if (strcmp(key, "TRACE_FILE_NAME") == 0) {
        qtg_trim_copy(hdr->file_name, sizeof(hdr->file_name), val, vlen);
    } else if (strcmp(key, "TRACE_DATE") == 0) {
        qtg_trim_copy(hdr->date, sizeof(hdr->date), val, vlen);
    } else if (strcmp(key, "TRACE_VER_MAJOR") == 0) {
        hdr->ver_major = (uint32_t)strtoul(val, NULL, 10);
    } else if (strcmp(key, "TRACE_VER_MINOR") == 0) {
        hdr->ver_minor = (uint32_t)strtoul(val, NULL, 10);
    } else if (strcmp(key, "TRACE_LITTLE_ENDIAN") == 0) {
        hdr->little_endian = (strcmp(val, "TRUE") == 0 || strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
    } else if (strcmp(key, "TRACE_BIG_ENDIAN") == 0) {
        if (strcmp(val, "TRUE") == 0 || strcmp(val, "true") == 0 || strcmp(val, "1") == 0) {
            hdr->little_endian = false;
        }
    } else if (strcmp(key, "TRACE_CYCLES_PER_SEC") == 0) {
        uint64_t cps = strtoull(val, NULL, 10);
        if (cps > 0ULL) hdr->cycles_per_sec = cps;
    } else if (strcmp(key, "TRACE_CPU_NUM") == 0) {
        uint32_t cpus = (uint32_t)strtoul(val, NULL, 10);
        if (cpus > 0U && cpus <= 64U) hdr->cpu_num = cpus;
    } else if (strcmp(key, "TRACE_SYSNAME") == 0) {
        qtg_trim_copy(hdr->sysname, sizeof(hdr->sysname), val, vlen);
    } else if (strcmp(key, "TRACE_NODENAME") == 0) {
        qtg_trim_copy(hdr->nodename, sizeof(hdr->nodename), val, vlen);
    } else if (strcmp(key, "TRACE_SYS_RELEASE") == 0) {
        qtg_trim_copy(hdr->sys_release, sizeof(hdr->sys_release), val, vlen);
    } else if (strcmp(key, "TRACE_MACHINE") == 0) {
        qtg_trim_copy(hdr->machine, sizeof(hdr->machine), val, vlen);
    } else if (strcmp(key, "TRACE_SYSPAGE_LEN") == 0) {
        errno = 0;
        unsigned long long sp = strtoull(val, NULL, 10);
        hdr->syspage_len = (errno == 0 && sp <= 64ULL * 1024ULL * 1024ULL) ? (uint32_t)sp : 0xFFFFFFFFU;
    } else if (strcmp(key, "TRACE_TRACELOGGER_ARGS") == 0) {
        qtg_trim_copy(hdr->tracelogger_args, sizeof(hdr->tracelogger_args), val, vlen);
    }
}

/* Locate substring inside binary buffer safely */
static const uint8_t *qtg_memfind(const uint8_t *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

typedef struct qtg_combine_acc {
    bool        active;
    qtg_event_t ev;
} qtg_combine_acc_t;

int qtg_cmp_event_ts_fn(const void *a, const void *b) {
    const qtg_event_t *ea = (const qtg_event_t *)a;
    const qtg_event_t *eb = (const qtg_event_t *)b;
    if (ea->timestamp_cycles < eb->timestamp_cycles) return -1;
    if (ea->timestamp_cycles > eb->timestamp_cycles) return 1;
    if (ea->seq < eb->seq) return -1;
    if (ea->seq > eb->seq) return 1;
    return 0;
}

int qtg_kev_read_buffer(const uint8_t *buf, size_t len, qtg_trace_t *out_trace, char *errbuf, size_t errlen) {
    if (!buf || !out_trace) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "invalid null argument");
        return -1;
    }
    qtg_trace_free(out_trace);
    qtg_trace_init(out_trace);

    size_t scan_limit = (len < 65536U) ? len : 65536U;
    const uint8_t *hdr_begin = qtg_memfind(buf, scan_limit, "TRACE_HEADER_BEGIN");
    const uint8_t *hdr_end   = NULL;
    size_t end_marker_len = 16U;
    if (hdr_begin) {
        size_t rem_scan = scan_limit - (size_t)(hdr_begin - buf);
        hdr_end = qtg_memfind(hdr_begin, rem_scan, "TRACE_HEADER_END");
        if (!hdr_end) {
            hdr_end = qtg_memfind(hdr_begin, rem_scan, "HEADER_END");
            end_marker_len = 10U;
        }
    }
    if (!hdr_begin || !hdr_end || hdr_end < hdr_begin) {
        if (errbuf && errlen) {
            snprintf(errbuf, errlen, "missing TRACE_HEADER_BEGIN .. TRACE_HEADER_END markers");
        }
        return -1;
    }

    /* Parse all TRACE_<KEY>::<VAL> entries between hdr_begin and hdr_end.
     * Supports both newline-delimited headers and NUL-delimited QNX kevfile headers. */
    const uint8_t *p = hdr_begin;
    while (p < hdr_end) {
        while (p < hdr_end && (*p == '\0' || *p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')) {
            p++;
        }
        if (p >= hdr_end) break;

        const uint8_t *rec_start = p;
        while (p < hdr_end && *p != '\n' && *p != '\0') {
            p++;
        }
        size_t rec_len = (size_t)(p - rec_start);

        /* Also handle concatenated "::TRACE_" segments inside a single record */
        const uint8_t *seg = rec_start;
        const uint8_t *rec_end = rec_start + rec_len;
        while (seg < rec_end) {
            const uint8_t *sep = qtg_memfind(seg, (size_t)(rec_end - seg), "::");
            if (!sep) break;
            char key[64];
            qtg_trim_copy(key, sizeof(key), (const char *)seg, (size_t)(sep - seg));
            const uint8_t *val_start = sep + 2;
            const uint8_t *next_trace = qtg_memfind(val_start, (size_t)(rec_end - val_start), "TRACE_");
            const uint8_t *val_end = next_trace ? next_trace : rec_end;
            /* If value itself is a filename containing TRACE_, only split if followed by "::" */
            if (next_trace) {
                const uint8_t *check_sep = qtg_memfind(next_trace, (size_t)(rec_end - next_trace), "::");
                if (!check_sep || (check_sep - next_trace) > 32) {
                    val_end = rec_end;
                    next_trace = NULL;
                }
            }
            char val[256];
            qtg_trim_copy(val, sizeof(val), (const char *)val_start, (size_t)(val_end - val_start));
            if (key[0] != '\0' && strcmp(key, "TRACE_HEADER_BEGIN") != 0) {
                qtg_apply_header_kv(&out_trace->header, key, val);
            }
            if (!next_trace) break;
            seg = next_trace;
        }
    }

    /* Advance past HEADER_END marker, optional "::", then either exact QNX syspage
     * (no NUL-skipping: real QNX tracelogger writes raw syspage immediately after "::"
     * and raw 16-byte events immediately after syspage_len) or synthetic NUL padding. */
    size_t data_off = (size_t)((hdr_end - buf) + end_marker_len);
    if (data_off + 2U <= len && buf[data_off] == ':' && buf[data_off + 1U] == ':') {
        data_off += 2U;
    }
    if (out_trace->header.syspage_len > 0U) {
        if (out_trace->header.syspage_len > len - data_off) {
            if (errbuf && errlen) {
                snprintf(errbuf, errlen, "truncated syspage (%u bytes)", out_trace->header.syspage_len);
            }
            return -1;
        }
        data_off += out_trace->header.syspage_len;
    } else {
        if (data_off < len && buf[data_off] == '\r') data_off++;
        if (data_off < len && buf[data_off] == '\n') data_off++;
        /* Align to 16-byte boundary while bytes are NUL padding (synthetic writer) */
        while ((data_off % 16U) != 0U && data_off < len && buf[data_off] == 0U) {
            data_off++;
        }
    }

    bool file_le = out_trace->header.little_endian;
    /* Per-CPU 64-bit timestamp tracking prevents SMP buffer out-of-order wrap corruption */
    uint32_t ts_hi[64];
    uint32_t last_ts_lo[64];
    bool     have_ts[64];
    bool     have_ctrl_time[64];
    memset(ts_hi, 0, sizeof(ts_hi));
    memset(last_ts_lo, 0, sizeof(last_ts_lo));
    memset(have_ts, 0, sizeof(have_ts));
    memset(have_ctrl_time, 0, sizeof(have_ctrl_time));
    uint64_t seq_counter = 0ULL;

    qtg_combine_acc_t combine[64];
    memset(combine, 0, sizeof(combine));

    while (data_off + 16U <= len) {
        uint32_t raw_hdr = qtg_read_u32(buf + data_off,      file_le);
        uint32_t d0      = qtg_read_u32(buf + data_off + 4U, file_le);
        uint32_t d1      = qtg_read_u32(buf + data_off + 8U, file_le);
        uint32_t d2      = qtg_read_u32(buf + data_off + 12U, file_le);
        data_off += 16U;
        out_trace->raw_slots_read++;

        uint32_t st  = QTG_HDR_GET_STRUCT(raw_hdr);
        uint32_t cpu = QTG_HDR_GET_CPU(raw_hdr);
        uint32_t cls = QTG_HDR_GET_CLASS(raw_hdr);
        uint32_t ev  = QTG_HDR_GET_EVENT(raw_hdr);
        uint32_t cidx = cpu & 63U;

        if (cls == QTG_CLASS_EMPTY && ev == 0U && st == QTG_STRUCT_S) {
            continue; /* Empty padding slot */
        }

        if (cls == QTG_CLASS_CONTROL && ev == QTG_EV_CTRL_TIME) {
            /* CONTROL_TIME updates upper 32 bits of 64-bit cycle counter for this CPU */
            ts_hi[cidx] = d1;
            last_ts_lo[cidx] = d0;
            have_ts[cidx] = true;
            have_ctrl_time[cidx] = true;
            /* Seed CPUs that have not yet emitted their own CONTROL_TIME */
            for (uint32_t k = 0; k < 64U; k++) {
                if (!have_ctrl_time[k] && !have_ts[k]) {
                    ts_hi[k] = d1;
                }
            }
            continue;
        }
        if (cls == QTG_CLASS_CONTROL && ev == QTG_EV_CTRL_BUFFER) {
            /* Buffer sequence metadata (QNX 8.0 CONTROL_BUFFER) */
            continue;
        }
        if (cls == QTG_CLASS_CONTROL && ev == QTG_EV_CTRL_BUFFER_END) {
            /* End-of-buffer sentinel (<hdr, ts, 0xe6e6e6e6, 0xe6e6e6e6>) */
            continue;
        }
        if (cls == QTG_CLASS_CONTROL && ev == QTG_EV_CTRL_DROPPED) {
            out_trace->dropped_buffer_events += (d1 > 0U) ? d1 : 1U;
            continue;
        }

        /* Reconstruct 64-bit cycle timestamp with per-CPU 32-bit wraparound protection */
        if (have_ts[cidx] && d0 < last_ts_lo[cidx] && (last_ts_lo[cidx] - d0) > 0x80000000U) {
            ts_hi[cidx]++;
        }
        last_ts_lo[cidx] = d0;
        have_ts[cidx] = true;
        uint64_t cycles = (((uint64_t)ts_hi[cidx]) << 32) | (uint64_t)d0;
        uint64_t ts_ns  = qtg_cycles_to_ns(cycles, out_trace->header.cycles_per_sec);

        if (st == QTG_STRUCT_S) {
            qtg_event_t item;
            memset(&item, 0, sizeof(item));
            item.timestamp_cycles = cycles;
            item.timestamp_ns = ts_ns;
            item.seq = seq_counter++;
            item.cpu = cpu;
            item.event_class = cls;
            item.event_id = ev;
            item.word_count = 2U;
            item.words[0] = d1;
            item.words[1] = d2;
            item.is_combine = false;
            if (qtg_trace_push(out_trace, &item) != 0) {
                if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory");
                return -1;
            }
        } else if (st == QTG_STRUCT_CB) {
            qtg_combine_acc_t *acc = &combine[cpu & 63U];
            memset(acc, 0, sizeof(*acc));
            acc->active = true;
            acc->ev.timestamp_cycles = cycles;
            acc->ev.timestamp_ns = ts_ns;
            acc->ev.seq = seq_counter++;
            acc->ev.cpu = cpu;
            acc->ev.event_class = cls;
            acc->ev.event_id = ev;
            acc->ev.is_combine = true;
            acc->ev.words[0] = d1;
            acc->ev.words[1] = d2;
            acc->ev.word_count = 2U;
        } else if (st == QTG_STRUCT_CC) {
            qtg_combine_acc_t *acc = &combine[cpu & 63U];
            if (acc->active) {
                if (acc->ev.word_count < QTG_MAX_EVENT_WORDS) acc->ev.words[acc->ev.word_count++] = d1;
                if (acc->ev.word_count < QTG_MAX_EVENT_WORDS) acc->ev.words[acc->ev.word_count++] = d2;
            }
        } else if (st == QTG_STRUCT_CE) {
            qtg_combine_acc_t *acc = &combine[cpu & 63U];
            if (acc->active) {
                if (acc->ev.word_count < QTG_MAX_EVENT_WORDS) acc->ev.words[acc->ev.word_count++] = d1;
                if (acc->ev.word_count < QTG_MAX_EVENT_WORDS) acc->ev.words[acc->ev.word_count++] = d2;
                if (qtg_trace_push(out_trace, &acc->ev) != 0) {
                    if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory");
                    return -1;
                }
                out_trace->combine_events_reassembled++;
                acc->active = false;
            }
        }
    }

    /* Stable chronological sort across SMP per-CPU tracebuf_t buffers */
    if (out_trace->count > 1U) {
        /* Only sort if out-of-order timestamps exist (O(n) fast check) */
        bool unsorted = false;
        for (size_t i = 1U; i < out_trace->count; i++) {
            if (out_trace->events[i].timestamp_cycles < out_trace->events[i - 1U].timestamp_cycles) {
                unsorted = true;
                break;
            }
        }
        if (unsorted) {
            /* qsort comparator defined above */
            extern int qtg_cmp_event_ts_fn(const void *a, const void *b);
            qsort(out_trace->events, out_trace->count, sizeof(qtg_event_t), qtg_cmp_event_ts_fn);
        }
    }

    return 0;
}

int qtg_kev_read_file(const char *path, qtg_trace_t *out_trace, char *errbuf, size_t errlen) {
    if (!path || !out_trace) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "invalid null path");
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "cannot open '%s': %s", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        if (errbuf && errlen) snprintf(errbuf, errlen, "fseek failed");
        return -1;
    }
    long sz = ftell(fp);
    if (sz < 0 || (unsigned long)sz > 64UL * 1024UL * 1024UL) {
        fclose(fp);
        if (errbuf && errlen) snprintf(errbuf, errlen, "file size invalid or exceeds 64 MiB safety cap");
        return -1;
    }
    rewind(fp);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1U);
    if (!buf) {
        fclose(fp);
        if (errbuf && errlen) snprintf(errbuf, errlen, "malloc failed");
        return -1;
    }
    size_t nread = fread(buf, 1U, (size_t)sz, fp);
    fclose(fp);
    buf[nread] = 0U;

    int rc = qtg_kev_read_buffer(buf, nread, out_trace, errbuf, errlen);
    free(buf);
    return rc;
}

/* Parse standard QNX `traceprinter` plain-text output into qtg_trace_t.
 * Requires a token boundary before `key` so "pid:" never matches "ppid:"/"cpid:"/"spid:". */
static bool qtg_parse_kv_u32(const char *line, const char *key, uint32_t *out) {
    size_t klen = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        if (p == line || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) {
            const char *vp = p + klen;
            while (*vp == ' ' || *vp == ':' || *vp == '=') vp++;
            char *endp = NULL;
            unsigned long val = strtoul(vp, &endp, 0);
            if (endp != vp) {
                *out = (uint32_t)val;
                return true;
            }
        }
        p += 1;
    }
    return false;
}

static int qtg_traceprinter_parse_line(const char *line, qtg_trace_t *out_trace, uint64_t *seq_counter) {
    /* Parse header metadata lines (KEY:: VALUE) */
    const char *sep = strstr(line, "::");
    if (sep && strncmp(line, "TRACE_", 6) == 0) {
        char key[64], val[256];
        qtg_trim_copy(key, sizeof(key), line, (size_t)(sep - line));
        qtg_trim_copy(val, sizeof(val), sep + 2, strlen(sep + 2));
        qtg_apply_header_kv(&out_trace->header, key, val);
        return 0;
    }
    /* Event lines look like: t:0x00001234 CPU:00 COMM :SND_MESSAGE ... */
    const char *tp = strstr(line, "t:0x");
    if (!tp) return 0;
    uint64_t cycles = strtoull(tp + 2, NULL, 16);
    uint32_t cpu = 0;
    qtg_parse_kv_u32(line, "CPU:", &cpu);

    qtg_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.timestamp_cycles = cycles;
    ev.timestamp_ns = qtg_cycles_to_ns(cycles, out_trace->header.cycles_per_sec);
    ev.seq = (*seq_counter)++;
    ev.cpu = cpu & 63U;

    uint32_t pid = 0, tid = 0, prio = 0, rcvid = 0, chid = 0, scoid = 0, spid = 0, stid = 0;
    if (strstr(line, "SND_MESSAGE")) {
        ev.event_class = QTG_CLASS_COMM;
        ev.event_id = QTG_COMM_SMSG;
        qtg_parse_kv_u32(line, "rcvid:", &rcvid);
        qtg_parse_kv_u32(line, "chid:", &chid);
        qtg_parse_kv_u32(line, "pid:", &spid);
        qtg_parse_kv_u32(line, "cpid:", &pid);
        qtg_parse_kv_u32(line, "ctid:", &tid);
        qtg_parse_kv_u32(line, "stid:", &stid);
        qtg_parse_kv_u32(line, "prio:", &prio);
        ev.words[0] = (chid != 0) ? chid : rcvid;
        ev.words[1] = spid;
        ev.words[2] = pid;
        ev.words[3] = tid;
        ev.words[4] = stid;
        ev.words[5] = prio;
        ev.word_count = (pid != 0) ? 6U : 2U;
    } else if (strstr(line, "REC_MESSAGE")) {
        ev.event_class = QTG_CLASS_COMM;
        ev.event_id = QTG_COMM_RMSG;
        qtg_parse_kv_u32(line, "rcvid:", &rcvid);
        qtg_parse_kv_u32(line, "pid:", &pid);
        qtg_parse_kv_u32(line, "spid:", &spid);
        qtg_parse_kv_u32(line, "stid:", &stid);
        qtg_parse_kv_u32(line, "ctid:", &tid);
        qtg_parse_kv_u32(line, "chid:", &chid);
        ev.words[0] = rcvid;
        ev.words[1] = pid;
        ev.words[2] = spid;
        ev.words[3] = stid;
        ev.words[4] = tid;
        ev.words[5] = chid;
        ev.word_count = (spid != 0) ? 6U : 2U;
    } else if (strstr(line, "REPLY_MESSAGE")) {
        ev.event_class = QTG_CLASS_COMM;
        ev.event_id = QTG_COMM_REPLY;
        qtg_parse_kv_u32(line, "tid:", &tid);
        qtg_parse_kv_u32(line, "pid:", &pid);
        qtg_parse_kv_u32(line, "spid:", &spid);
        qtg_parse_kv_u32(line, "stid:", &stid);
        qtg_parse_kv_u32(line, "chid:", &chid);
        ev.words[0] = tid;
        ev.words[1] = pid;
        ev.words[2] = spid;
        ev.words[3] = stid;
        ev.words[4] = chid;
        ev.word_count = (spid != 0) ? 5U : 2U;
    } else if (strstr(line, "SND_PULSE")) {
        ev.event_class = QTG_CLASS_COMM;
        ev.event_id = QTG_COMM_SPULSE;
        qtg_parse_kv_u32(line, "scoid:", &scoid);
        qtg_parse_kv_u32(line, "chid:", &chid);
        qtg_parse_kv_u32(line, "pid:", &pid);
        ev.words[0] = (chid != 0) ? chid : scoid;
        ev.words[1] = pid;
        ev.word_count = 2U;
    } else if (strstr(line, "REC_PULSE")) {
        ev.event_class = QTG_CLASS_COMM;
        ev.event_id = QTG_COMM_RPULSE;
        qtg_parse_kv_u32(line, "scoid:", &scoid);
        qtg_parse_kv_u32(line, "chid:", &chid);
        qtg_parse_kv_u32(line, "pid:", &pid);
        ev.words[0] = (chid != 0) ? chid : scoid;
        ev.words[1] = pid;
        ev.word_count = 2U;
    } else if (strstr(line, "THRUNNING") || strstr(line, "THREADY") ||
               strstr(line, "THSEND") || strstr(line, "THRECEIVE") ||
               strstr(line, "THREPLY") || strstr(line, "THMUTEX")) {
        ev.event_class = QTG_CLASS_PR_TH;
        if (strstr(line, "THRUNNING")) ev.event_id = QTG_TH_RUNNING;
        else if (strstr(line, "THREADY")) ev.event_id = QTG_TH_READY;
        else if (strstr(line, "THSEND")) ev.event_id = QTG_TH_SEND;
        else if (strstr(line, "THRECEIVE")) ev.event_id = QTG_TH_RECEIVE;
        else if (strstr(line, "THREPLY")) ev.event_id = QTG_TH_REPLY;
        else ev.event_id = QTG_TH_MUTEX;
        qtg_parse_kv_u32(line, "pid:", &pid);
        qtg_parse_kv_u32(line, "tid:", &tid);
        bool has_prio = qtg_parse_kv_u32(line, "priority:", &prio) ||
                        qtg_parse_kv_u32(line, "prio:", &prio);
        ev.words[0] = pid;
        ev.words[1] = tid;
        if (has_prio) {
            ev.words[2] = prio;
            ev.word_count = 3U;
        } else {
            ev.word_count = 2U;
        }
    } else if (strstr(line, "PROCCREATE_NAME")) {
        ev.event_class = QTG_CLASS_PR_TH;
        ev.event_id = QTG_PR_TH_CREATE_P_NAME;
        uint32_t ppid = 0;
        qtg_parse_kv_u32(line, "ppid:", &ppid);
        qtg_parse_kv_u32(line, "pid:", &pid);
        ev.words[0] = ppid;
        ev.words[1] = pid;
        ev.word_count = 2U;
        const char *np = strstr(line, "name:");
        if (np) {
            np += 5;
            while (*np == ' ') np++;
            char nbuf[64];
            qtg_trim_copy(nbuf, sizeof(nbuf), np, strlen(np));
            size_t nlen = strlen(nbuf);
            size_t nwords = (nlen + 4U) / 4U;
            if (2U + nwords > QTG_MAX_EVENT_WORDS) nwords = QTG_MAX_EVENT_WORDS - 2U;
            memset(&ev.words[2], 0, nwords * sizeof(uint32_t));
            memcpy(&ev.words[2], nbuf, nlen);
            ev.word_count = (uint32_t)(2U + nwords);
        }
    } else {
        return 0;
    }
    return qtg_trace_push(out_trace, &ev);
}

static void qtg_trace_sort_chronological(qtg_trace_t *tr) {
    if (!tr || tr->count <= 1U) return;
    bool unsorted = false;
    for (size_t i = 1U; i < tr->count; i++) {
        if (tr->events[i].timestamp_cycles < tr->events[i - 1U].timestamp_cycles) {
            unsorted = true;
            break;
        }
    }
    if (unsorted) {
        qsort(tr->events, tr->count, sizeof(qtg_event_t), qtg_cmp_event_ts_fn);
    }
}

int qtg_traceprinter_read_buffer(const char *buf, size_t len, qtg_trace_t *out_trace, char *errbuf, size_t errlen) {
    if (!buf || !out_trace) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "invalid null argument");
        return -1;
    }
    qtg_trace_free(out_trace);
    qtg_trace_init(out_trace);

    uint64_t seq_counter = 0ULL;
    size_t pos = 0U;
    while (pos < len) {
        size_t start = pos;
        while (pos < len && buf[pos] != '\n') pos++;
        size_t llen = pos - start;
        if (pos < len && buf[pos] == '\n') pos++;
        char line[1024];
        if (llen >= sizeof(line)) llen = sizeof(line) - 1U;
        memcpy(line, buf + start, llen);
        line[llen] = '\0';
        if (qtg_traceprinter_parse_line(line, out_trace, &seq_counter) != 0) {
            if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory");
            return -1;
        }
    }
    qtg_trace_sort_chronological(out_trace);
    return 0;
}

int qtg_traceprinter_read_file(const char *path, qtg_trace_t *out_trace, char *errbuf, size_t errlen) {
    if (!path || !out_trace) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "cannot open '%s': %s", path, strerror(errno));
        return -1;
    }
    qtg_trace_free(out_trace);
    qtg_trace_init(out_trace);

    uint64_t seq_counter = 0ULL;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (qtg_traceprinter_parse_line(line, out_trace, &seq_counter) != 0) {
            fclose(fp);
            if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory");
            return -1;
        }
    }
    fclose(fp);
    qtg_trace_sort_chronological(out_trace);
    return 0;
}

static int qtg_writer_emit_slot(qtg_kev_writer_t *w, uint32_t hdr, uint32_t d0, uint32_t d1, uint32_t d2) {
    uint8_t slot[16];
    qtg_write_u32(slot + 0U,  hdr, w->header.little_endian);
    qtg_write_u32(slot + 4U,  d0,  w->header.little_endian);
    qtg_write_u32(slot + 8U,  d1,  w->header.little_endian);
    qtg_write_u32(slot + 12U, d2,  w->header.little_endian);
    if (fwrite(slot, 1U, sizeof(slot), w->fp) != sizeof(slot)) {
        return -1;
    }
    w->slots_written++;
    return 0;
}

static int qtg_writer_sync_time(qtg_kev_writer_t *w, uint32_t cpu, uint64_t cycles) {
    uint32_t hi = (uint32_t)(cycles >> 32);
    uint32_t lo = (uint32_t)(cycles & 0xFFFFFFFFULL);
    if (!w->has_ts || hi != w->last_ts_hi) {
        uint32_t chdr = QTG_HDR_MAKE(QTG_STRUCT_S, cpu, QTG_CLASS_CONTROL, QTG_EV_CTRL_TIME);
        if (qtg_writer_emit_slot(w, chdr, lo, hi, 0U) != 0) return -1;
        w->last_ts_hi = hi;
        w->last_ts_lo = lo;
        w->has_ts = true;
    } else {
        w->last_ts_lo = lo;
    }
    return 0;
}

int qtg_kev_writer_open(qtg_kev_writer_t *w, const char *path, const qtg_kev_header_t *hdr) {
    if (!w || !path) return -1;
    memset(w, 0, sizeof(*w));
    if (hdr) {
        w->header = *hdr;
    } else {
        qtg_kev_header_defaults(&w->header);
    }
    /* Synthetic writer writes no raw syspage blob; keep syspage_len=0 for roundtrip safety */
    w->header.syspage_len = 0U;
    w->fp = fopen(path, "wb");
    if (!w->fp) return -1;

    char hbuf[2048];
    int n = snprintf(hbuf, sizeof(hbuf),
        "TRACE_HEADER_BEGIN::\n"
        "TRACE_FILE_NAME:: %s\n"
        "TRACE_DATE:: %s\n"
        "TRACE_VER_MAJOR:: %u\n"
        "TRACE_VER_MINOR:: %u\n"
        "TRACE_LITTLE_ENDIAN:: %s\n"
        "TRACE_ENCODING:: 16 byte events\n"
        "TRACE_CYCLES_PER_SEC:: %llu\n"
        "TRACE_CPU_NUM:: %u\n"
        "TRACE_SYSNAME:: %s\n"
        "TRACE_NODENAME:: %s\n"
        "TRACE_SYS_RELEASE:: %s\n"
        "TRACE_MACHINE:: %s\n"
        "TRACE_SYSPAGE_LEN:: %u\n"
        "TRACE_TRACELOGGER_ARGS:: %s\n"
        "TRACE_HEADER_END::\n",
        w->header.file_name,
        w->header.date,
        w->header.ver_major,
        w->header.ver_minor,
        w->header.little_endian ? "TRUE" : "FALSE",
        (unsigned long long)w->header.cycles_per_sec,
        w->header.cpu_num,
        w->header.sysname,
        w->header.nodename,
        w->header.sys_release,
        w->header.machine,
        w->header.syspage_len,
        w->header.tracelogger_args);
    if (n <= 0 || (size_t)n >= sizeof(hbuf)) {
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }
    if (fwrite(hbuf, 1U, (size_t)n, w->fp) != (size_t)n) {
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }
    /* Pad to 16-byte alignment */
    size_t pad = (16U - ((size_t)n % 16U)) % 16U;
    uint8_t zeros[16] = {0};
    if (pad > 0 && fwrite(zeros, 1U, pad, w->fp) != pad) {
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }
    return 0;
}

int qtg_kev_writer_write_simple(qtg_kev_writer_t *w, uint32_t cpu, uint32_t cls, uint32_t ev,
                                uint64_t cycles, uint32_t w1, uint32_t w2) {
    if (!w || !w->fp) return -1;
    if (qtg_writer_sync_time(w, cpu, cycles) != 0) return -1;
    uint32_t hdr = QTG_HDR_MAKE(QTG_STRUCT_S, cpu, cls, ev);
    return qtg_writer_emit_slot(w, hdr, (uint32_t)(cycles & 0xFFFFFFFFULL), w1, w2);
}

int qtg_kev_writer_write_combine(qtg_kev_writer_t *w, uint32_t cpu, uint32_t cls, uint32_t ev,
                                 uint64_t cycles, const uint32_t *words, size_t word_count) {
    if (!w || !w->fp) return -1;
    if (word_count <= 2U) {
        uint32_t a = (word_count > 0U && words) ? words[0] : 0U;
        uint32_t b = (word_count > 1U && words) ? words[1] : 0U;
        return qtg_kev_writer_write_simple(w, cpu, cls, ev, cycles, a, b);
    }
    if (qtg_writer_sync_time(w, cpu, cycles) != 0) return -1;
    uint32_t lo = (uint32_t)(cycles & 0xFFFFFFFFULL);
    size_t num_slots = (word_count + 1U) / 2U;
    for (size_t s = 0; s < num_slots; s++) {
        uint32_t st = (s == 0U) ? QTG_STRUCT_CB :
                      (s + 1U == num_slots) ? QTG_STRUCT_CE : QTG_STRUCT_CC;
        uint32_t a = words[s * 2U];
        uint32_t b = (s * 2U + 1U < word_count) ? words[s * 2U + 1U] : 0U;
        uint32_t hdr = QTG_HDR_MAKE(st, cpu, cls, ev);
        if (qtg_writer_emit_slot(w, hdr, lo, a, b) != 0) return -1;
    }
    return 0;
}

int qtg_kev_writer_write_name(qtg_kev_writer_t *w, uint32_t cpu, uint32_t cls, uint32_t ev,
                              uint64_t cycles, uint32_t id1, uint32_t id2, const char *name) {
    uint32_t words[20];
    memset(words, 0, sizeof(words));
    words[0] = id1;
    words[1] = id2;
    size_t nlen = name ? strlen(name) : 0U;
    if (nlen > 63U) nlen = 63U;
    /* Pack ASCII string little-endian into words[2..] */
    for (size_t i = 0; i < nlen; i++) {
        words[2U + (i / 4U)] |= ((uint32_t)(uint8_t)name[i]) << ((i % 4U) * 8U);
    }
    size_t str_words = (nlen + 4U) / 4U;
    if (str_words < 1U) str_words = 1U;
    return qtg_kev_writer_write_combine(w, cpu, cls, ev, cycles, words, 2U + str_words);
}

int qtg_kev_writer_close(qtg_kev_writer_t *w) {
    if (!w) return -1;
    if (w->fp) {
        int rc = fclose(w->fp);
        w->fp = NULL;
        return rc;
    }
    return 0;
}

void qtg_extract_ascii_from_words(const uint32_t *words, size_t word_count, char *out, size_t out_len) {
    if (!out || out_len == 0U) return;
    size_t pos = 0;
    for (size_t i = 0; i < word_count && pos + 1U < out_len; i++) {
        uint32_t w = words[i];
        for (int b = 0; b < 4 && pos + 1U < out_len; b++) {
            char ch = (char)((w >> (b * 8)) & 0xFFU);
            if (ch == '\0') {
                out[pos] = '\0';
                return;
            }
            if (isprint((unsigned char)ch)) {
                out[pos++] = ch;
            }
        }
    }
    out[pos] = '\0';
}

const char *qtg_class_name(uint32_t cls) {
    switch (cls) {
        case QTG_CLASS_EMPTY:   return "EMPTY";
        case QTG_CLASS_CONTROL: return "CONTROL";
        case QTG_CLASS_KERCALL: return "KERCALL";
        case QTG_CLASS_INT:     return "INT";
        case QTG_CLASS_PR_TH:   return "PR_TH";
        case QTG_CLASS_SYSTEM:  return "SYSTEM";
        case QTG_CLASS_USER:    return "USER";
        case QTG_CLASS_COMM:    return "COMM";
        default:                return "UNKNOWN";
    }
}

const char *qtg_event_name(uint32_t cls, uint32_t ev) {
    if (cls == QTG_CLASS_COMM) {
        switch (ev) {
            case QTG_COMM_SMSG:      return "SND_MESSAGE";
            case QTG_COMM_SPULSE:    return "SND_PULSE";
            case QTG_COMM_RMSG:      return "REC_MESSAGE";
            case QTG_COMM_RPULSE:    return "REC_PULSE";
            case QTG_COMM_SIGNAL:    return "SIGNAL";
            case QTG_COMM_REPLY:     return "REPLY_MESSAGE";
            case QTG_COMM_ERROR:     return "COMM_ERROR";
            case QTG_COMM_SPULSE_UN: return "SND_PULSE_UNBLOCK";
            default:                 return "COMM_OTHER";
        }
    }
    if (cls == QTG_CLASS_PR_TH) {
        switch (ev) {
            case QTG_TH_DEAD:            return "THDEAD";
            case QTG_TH_RUNNING:         return "THRUNNING";
            case QTG_TH_READY:           return "THREADY";
            case QTG_TH_STOPPED:         return "THSTOPPED";
            case QTG_TH_SEND:            return "THSEND";
            case QTG_TH_RECEIVE:         return "THRECEIVE";
            case QTG_TH_REPLY:           return "THREPLY";
            case QTG_TH_MUTEX:           return "THMUTEX";
            case QTG_TH_CONDVAR:         return "THCONDVAR";
            case QTG_PR_TH_CREATE_T:     return "THCREATE";
            case QTG_PR_TH_DESTROY_T:    return "THDESTROY";
            case QTG_PR_TH_NAME_T:       return "THNAME";
            case QTG_PR_TH_CREATE_P:     return "PROCCREATE";
            case QTG_PR_TH_CREATE_P_NAME:return "PROCCREATE_NAME";
            default:                     return "PR_TH_OTHER";
        }
    }
    if (cls == QTG_CLASS_SYSTEM) {
        switch (ev) {
            case QTG_SYS_APS_NAME:      return "APS_NAME";
            case QTG_SYS_APS_BUDGETS:   return "APS_BUDGETS";
            case QTG_SYS_APS_BNKR:      return "APS_BANKRUPTCY";
            case QTG_SYS_CH_FLAGS:      return "CHANNEL_FLAGS";
            case QTG_SYS_SPORADIC_REPL: return "SPORADIC_REPL";
            case QTG_SYS_ABILITY_CAP:   return "ABILITY_PRIORITY_CAP";
            case QTG_SYS_RESMGR_ATTACH: return "RESMGR_ATTACH";
            default:                    return "SYS_OTHER";
        }
    }
    if (cls == QTG_CLASS_CONTROL) {
        if (ev == QTG_EV_CTRL_TIME) return "CONTROL_TIME";
        if (ev == QTG_EV_CTRL_BUFFER) return "CONTROL_BUFFER";
    }
    if (cls == QTG_CLASS_USER) {
        return "USER_MARKER";
    }
    return "EVENT";
}
