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
 * .dv / .ts passthrough: byte-identical to what pincli has always written
 * (dv_output_file() before this refactor). No metadata is possible in a raw
 * elementary/transport stream, hence supports_title = 0.
 */

#include "pin_sink.h"
#include "../engine/dv_subcode.h"
#include "../core/pin_log.h"
#include "pin_stdout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *f;
    int to_stdout;            /* path "-": pin_stdout_write(), no file */
    pin_kind_t kind;
    pin_sink_status_t st;
} raw_priv_t;

static int is_valid_dv_unit_len(size_t len)
{
    return len == (size_t)DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE ||
           len == (size_t)DV_SEQ_COUNT_PAL * DV_SEQ_SIZE;
}

static pin_status_t raw_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    raw_priv_t *p = s->priv;
    p->kind = params->kind;
    if (pin_path_is_stdout(path)) {
        pin_stdout_prepare();
        p->to_stdout = 1;
        return PIN_OK;
    }
    p->f = fopen(path, "wb");
    if (!p->f) {
        p->st.last_error = PIN_ERR_IO;
        return PIN_ERR_IO;
    }
    setvbuf(p->f, NULL, _IOFBF, 4 << 20);
    return PIN_OK;
}

static pin_status_t raw_write_unit(pin_sink_t *s, const uint8_t *data, size_t len)
{
    raw_priv_t *p = s->priv;
    if (!p->f && !p->to_stdout)
        return PIN_ERR_STATE;

    /* HDV pictures vary in size (a GOP's worth of TS packets); only DV
     * frames have a fixed, checkable size. See dv_reassembler.h: a unit can
     * exceed 10/12 sequences after a bogus resync, and this is exactly the
     * check the header asks every sink to make. */
    if (p->kind == PIN_KIND_DV && !is_valid_dv_unit_len(len)) {
        p->st.units_damaged++;
        pin_logf(PIN_LOG_WARN, "sink_raw: dropping a %zu-byte DV unit (not 10 or 12 whole "
                                "DIF sequences)\n", len);
        return PIN_OK;
    }

    if (p->to_stdout) {
        int rc = pin_stdout_write(data, len);
        if (rc != PIN_STDOUT_OK) {
            p->st.pipe_closed = rc == PIN_STDOUT_CLOSED;
            p->st.last_error = PIN_ERR_IO;
            return PIN_ERR_IO;
        }
    } else if (fwrite(data, 1, len, p->f) != len) {
        p->st.last_error = PIN_ERR_IO;
        return PIN_ERR_IO;
    }
    p->st.bytes_written += len;
    p->st.units_written++;
    return PIN_OK;
}

static pin_status_t raw_close(pin_sink_t *s)
{
    raw_priv_t *p = s->priv;
    pin_status_t rc = PIN_OK;
    if (p->f) {
        if (fclose(p->f) != 0)
            rc = PIN_ERR_IO;
        p->f = NULL;
    }
    free(p);
    free(s);
    return rc;
}

static void raw_get_status(pin_sink_t *s, pin_sink_status_t *out)
{
    raw_priv_t *p = s->priv;
    *out = p->st;
}

pin_sink_t *sink_raw_create(void)
{
    pin_sink_t *s = calloc(1, sizeof(*s));
    raw_priv_t *p = calloc(1, sizeof(*p));
    if (!s || !p) {
        free(s);
        free(p);
        return NULL;
    }
    s->priv = p;
    s->open = raw_open;
    s->write_unit = raw_write_unit;
    s->close = raw_close;
    s->get_status = raw_get_status;
    s->supports_title = 0;
    return s;
}
