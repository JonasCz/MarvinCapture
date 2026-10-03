/*
 * Pinnacle Studio 500-USB open driver
 * Copyright (C) 2026 Jonas Cz.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * pindeck — AV/C deck control over the Pinnacle's 1394 link.
 *
 * Brings the device up exactly like pincli (camera found, its output plug
 * connected, isochronous receive running), then runs the steps given on the
 * command line:
 *
 *   play | pause | stop | ff | rew | state | timecode | subunits |
 *   wait:<sec> | raw:<hex bytes> | reg:<ohci offset>
 *
 * Each deck step is an AV/C command written to the camera's FCP_COMMAND
 * register; pinnacle_1394.c sends it and waits for the response. EP 0x88 is
 * drained throughout and its byte rate printed once a second during wait:,
 * which is the independent proof that the tape really moves: a stopped deck
 * sends nothing.
 *
 * See docs/deck-control.md.
 */

#include "pinnacle_1394.h"
#include "pinnacle_device.h"
#include "pinnacle_enum.h"
#include "pinnacle_lock.h"
#include "pinnacle_stream.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static void on_sigint(int s) { (void)s; g_stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double g_t0;
static pinnacle_device_t g_dev;
static pinnacle_1394_t g_link;

/* --- EP 0x88: drained on its own thread, bytes counted ------------------ */

static volatile int g_ep88_stop;
static unsigned long long g_ep88_bytes;

static void *ep88_thread(void *arg)
{
    FILE *raw = arg;
    uint8_t *buf = malloc(65536);
    while (!g_ep88_stop && buf) {
        int n = 0;
        int rc = libusb_bulk_transfer(g_dev.handle, PINNACLE_EP_DV_IN, buf, 65536, &n, 100);
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "ep88 read error %s\n", libusb_error_name(rc));
            break;
        }
        if (n > 0) {
            __atomic_add_fetch(&g_ep88_bytes, (unsigned long long)n, __ATOMIC_RELAXED);
            if (raw)
                fwrite(buf, 1, (size_t)n, raw);
        }
    }
    free(buf);
    return NULL;
}

/* --- AV/C ---------------------------------------------------------------- */

static const char *ctype_name(uint8_t c)
{
    switch (c) {
    case 0x08: return "NOT_IMPLEMENTED";
    case 0x09: return "ACCEPTED";
    case 0x0a: return "REJECTED";
    case 0x0b: return "IN_TRANSITION";
    case 0x0c: return "STABLE";
    case 0x0d: return "CHANGED";
    case 0x0f: return "INTERIM";
    default:   return "?";
    }
}

static const char *transport_name(uint8_t op, uint8_t mode)
{
    if (op == 0xc3) {
        switch (mode) {
        case 0x75: return "PLAY forward";
        case 0x7d: return "PLAY forward-pause";
        case 0x65: return "PLAY reverse";
        case 0x6d: return "PLAY reverse-pause";
        default:   return "PLAY (other mode)";
        }
    }
    if (op == 0xc4) {
        switch (mode) {
        case 0x60: return "WIND stop";
        case 0x65: return "WIND rewind";
        case 0x75: return "WIND fast-forward";
        case 0x45: return "WIND high-speed rewind";
        default:   return "WIND (other mode)";
        }
    }
    if (op == 0xc2)
        return "RECORD";
    if (op == 0xc1)
        return "LOAD MEDIUM";
    return "?";
}

/* Sends one command and prints the exchange. The Canon HDV answers
 * transport commands (and TRANSPORT STATE) with NOT_IMPLEMENTED while its
 * mechanism is changing mode, e.g. for a few seconds after PLAY while the
 * tape threads, so those are retried. */
static int avc(const uint8_t *cmd, unsigned len, uint8_t *resp)
{
    int transport = len >= 3 && cmd[1] == 0x20 && (cmd[2] == 0xc3 || cmd[2] == 0xc4 || cmd[2] == 0xd0);
    int rl = -1;
    for (int attempt = 0; attempt < 5 && !g_stop; attempt++) {
        printf("  -> sent");
        for (unsigned i = 0; i < len; i++)
            printf(" %02x", cmd[i]);
        printf("\n");
        rl = p1394_avc(&g_link, g_dev.camera_node, cmd, len, resp, 512, 3000);
        if (rl < 0) {
            printf("  <- no response\n");
            return -1;
        }
        printf("  <- %-15s", ctype_name(resp[0]));
        for (int i = 0; i < rl; i++)
            printf(" %02x", resp[i]);
        printf("\n");
        if (!(transport && resp[0] == 0x08))
            break;
        printf("     (camera busy, retrying)\n");
        usleep(700000);
    }
    return rl;
}

static void report_rate(double seconds)
{
    double end = now_s() + seconds;
    while (now_s() < end && !g_stop) {
        unsigned long long b0 = __atomic_load_n(&g_ep88_bytes, __ATOMIC_RELAXED);
        double t = now_s();
        double slice = end - t < 1.0 ? end - t : 1.0;
        while (now_s() < t + slice && !g_stop) {
            /* answer anything the camera sends meanwhile */
            p1394_pump(&g_link, 20);
            p1394_answer_owed(&g_link);
        }
        unsigned long long b1 = __atomic_load_n(&g_ep88_bytes, __ATOMIC_RELAXED);
        printf("  [%7.3f] EP 0x88: %8.1f KB/s\n", now_s() - g_t0, (b1 - b0) / 1024.0 / slice);
        fflush(stdout);
    }
}

static int parse_hex(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == ',' || *s == '.' || *s == '-')
            s++;
        if (!s[0] || !s[1])
            break;
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            return -1;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-b bitstream (default firmware/fpga-ohci.bin)] [-r raw.bin] [-v|-vv] step...\n"
            "steps: play pause stop ff rew state timecode subunits wait:<sec> raw:<hex> reg:<offset>\n"
            "       e.g.  %s state play wait:5 stop state\n", argv0, argv0);
}

int main(int argc, char **argv)
{
    const char *bitstream = "firmware/fpga-ohci.bin";
    const char *raw_path = NULL;
    int verbose = 0;
    int first = 1;

    while (first < argc && argv[first][0] == '-') {
        if (!strcmp(argv[first], "-b") && first + 1 < argc) {
            bitstream = argv[++first];
        } else if (!strcmp(argv[first], "-r") && first + 1 < argc) {
            raw_path = argv[++first];
        } else if (!strcmp(argv[first], "-v")) {
            verbose++;
        } else if (!strcmp(argv[first], "-vv")) {
            verbose += 2;
        } else {
            usage(argv[0]);
            return 2;
        }
        first++;
    }
    if (first >= argc) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, on_sigint);
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_t0 = now_s();

    /* Cross-process ownership (pinnacle_lock.h): refuse to fight another
     * pincli/pindeck/pinanalog -- or MarvinCaptureCLI -- over this device. */
    pinnacle_lock_t *lock = NULL;
    pinnacle_status_t st = pinnacle_open(&g_dev);
    if (st == PINNACLE_OK)
        pinnacle_tuning_from_env(&g_dev.tuning);
    if (st == PINNACLE_OK) {
        char device_id[PINNACLE_ENUM_ID_MAX];
        pinnacle_enum_build_id(libusb_get_device(g_dev.handle), device_id, sizeof(device_id));
        st = pinnacle_lock_acquire(device_id, &lock);
    }
    if (st == PINNACLE_OK)
        st = pinnacle_init_hardware(&g_dev, bitstream);
    if (st == PINNACLE_OK)
        pinnacle_lock_update(lock, PINNACLE_LOCK_READY, g_dev.guid_hi, g_dev.guid_lo);
    if (st == PINNACLE_OK)
        st = pinnacle_stream_start(&g_dev);
    if (st == PINNACLE_OK)
        pinnacle_lock_update(lock, PINNACLE_LOCK_CAPTURING, g_dev.guid_hi, g_dev.guid_lo);
    if (st != PINNACLE_OK) {
        fprintf(stderr, "pindeck: bring-up failed: %s\n", pinnacle_strerror(st));
        pinnacle_lock_release(lock);
        pinnacle_close(&g_dev);
        return 1;
    }
    if (!g_dev.camera_node) {
        fprintf(stderr, "pindeck: no camera on the 1394 bus\n");
        pinnacle_stream_stop(&g_dev);
        pinnacle_lock_release(lock);
        pinnacle_close(&g_dev);
        return 1;
    }
    printf("device up (%.1f s), camera is node %u\n", now_s() - g_t0, g_dev.camera_node & 0x3f);

    p1394_init(&g_link, &g_dev);
    g_link.verbose = verbose;
    FILE *raw = raw_path ? fopen(raw_path, "wb") : NULL;
    pthread_t t88;
    pthread_create(&t88, NULL, ep88_thread, raw);

    for (int i = first; i < argc && !g_stop; i++) {
        const char *s = argv[i];
        uint8_t cmd[64], resp[512];
        int clen = 0;
        printf("[%7.3f] step %s\n", now_s() - g_t0, s);

        if (!strncmp(s, "reg:", 4)) {
            uint32_t v = 0;
            unsigned off = (unsigned)strtoul(s + 4, NULL, 16);
            if (p1394_reg_read(&g_link, (uint16_t)off, &v) == 0)
                printf("  reg 0x%03x = 0x%08x\n", off, v);
            else
                printf("  reg 0x%03x: no reply\n", off);
            continue;
        }
        if (!strncmp(s, "wait:", 5)) {
            report_rate(atof(s + 5));
            continue;
        }
        static const struct { const char *name; uint8_t len, b[8]; } named[] = {
            { "play",     4, { 0x00, 0x20, 0xc3, 0x75 } },       /* PLAY forward */
            { "pause",    4, { 0x00, 0x20, 0xc3, 0x7d } },       /* PLAY forward-pause */
            { "stop",     4, { 0x00, 0x20, 0xc4, 0x60 } },       /* WIND stop */
            { "ff",       4, { 0x00, 0x20, 0xc4, 0x75 } },       /* WIND fast-forward */
            { "rew",      4, { 0x00, 0x20, 0xc4, 0x65 } },       /* WIND rewind */
            { "state",    4, { 0x01, 0x20, 0xd0, 0x7f } },       /* TRANSPORT STATE status */
            { "timecode", 8, { 0x01, 0x20, 0x51, 0x71, 0xff, 0xff, 0xff, 0xff } },
            { "subunits", 8, { 0x01, 0xff, 0x31, 0x07, 0xff, 0xff, 0xff, 0xff } },
        };
        for (unsigned k = 0; k < sizeof(named) / sizeof(named[0]); k++)
            if (!strcmp(s, named[k].name)) {
                memcpy(cmd, named[k].b, named[k].len);
                clen = named[k].len;
            }
        if (!strncmp(s, "raw:", 4))
            clen = parse_hex(s + 4, cmd, sizeof(cmd));
        if (clen <= 0) {
            fprintf(stderr, "unknown step %s\n", s);
            continue;
        }

        int rl = avc(cmd, (unsigned)clen, resp);
        if (rl >= 4 && resp[1] == 0x20) {
            if (cmd[2] == 0xd0)          /* TRANSPORT STATE answers with the transport opcode */
                printf("     transport: %s\n", transport_name(resp[2], resp[3]));
            else if (resp[2] == 0xc3 || resp[2] == 0xc4)
                printf("     %s\n", transport_name(resp[2], resp[3]));
            else if (resp[2] == 0x51 && rl >= 8 && resp[0] == 0x0c)
                printf("     time code %02x:%02x:%02x:%02x\n", resp[7], resp[6], resp[5], resp[4]);
        }
    }

    g_ep88_stop = 1;
    pthread_join(t88, NULL);
    if (raw)
        fclose(raw);
    /* stops isochronous receive (draining EP 0x88 itself) and releases the
     * camera's plug; the tape keeps doing whatever it was last told */
    pinnacle_stream_stop(&g_dev);
    pinnacle_lock_release(lock);
    pinnacle_close(&g_dev);
    printf("done\n");
    return 0;
}
