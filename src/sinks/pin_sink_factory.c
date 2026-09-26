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

#include "pin_sink.h"
#include "sinks_internal.h"

pin_sink_t *pin_sink_create(pin_format_t format)
{
    switch (format) {
    case PIN_FMT_ANALOG_AVI:
        return sink_avi_create();
    case PIN_FMT_ANALOG_FFV1_MKV:
        return sink_ffv1_create();
    case PIN_FMT_DV_RAW:
    case PIN_FMT_HDV_TS:
        return sink_raw_create();
    case PIN_FMT_DV_AVI:
    case PIN_FMT_DV_MOV:
    case PIN_FMT_HDV_MOV:
    case PIN_FMT_HDV_MKV:
        return sink_rewrap_create(format);
    default:
        return NULL;
    }
}
