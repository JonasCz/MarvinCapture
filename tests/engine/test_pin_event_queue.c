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

/* The event queue when it overflows: a log event goes first, and what is
 * left still comes out in the order it went in. Uses the process-wide
 * queue (s == NULL), which shares the code with the per-session one. */

#include "pin_session.h"
#include "pin_session_priv.h"
#include <stdio.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void drain(void)
{
    pin_event_t e;
    while (pin_session_poll_event(NULL, &e))
        ;
}

/* Full queue with one log event in the middle; one more push must drop the
 * log event and keep every other event in order. */
static void test_drops_log_keeps_order(void)
{
    drain();
    int n = 0;
    for (int i = 0; i < PIN_EVQ_CAP; i++) {
        if (i == 10)
            pin_session_push_event(NULL, PIN_EVT_LOG, -1, "log");
        else
            pin_session_push_event(NULL, PIN_EVT_STATE, n++, NULL);
    }
    pin_session_push_event(NULL, PIN_EVT_STATE, n++, NULL);

    pin_event_t e;
    int got = 0, expect = 0, in_order = 1, saw_log = 0;
    while (pin_session_poll_event(NULL, &e)) {
        got++;
        if (e.kind == PIN_EVT_LOG) {
            saw_log = 1;
            continue;
        }
        if (e.a != expect)
            in_order = 0;
        expect++;
    }
    CHECK(got == PIN_EVQ_CAP, "queue holds exactly its capacity");
    CHECK(!saw_log, "the log event was the one dropped");
    CHECK(in_order, "the other events keep their order");
    CHECK(expect == n, "no other event was lost");
}

/* Full queue without log events: the oldest one goes. */
static void test_drops_oldest_without_log(void)
{
    drain();
    for (int i = 0; i <= PIN_EVQ_CAP; i++)
        pin_session_push_event(NULL, PIN_EVT_STATE, i, NULL);

    pin_event_t e;
    int expect = 1, in_order = 1, got = 0;
    while (pin_session_poll_event(NULL, &e)) {
        got++;
        if (e.a != expect++)
            in_order = 0;
    }
    CHECK(got == PIN_EVQ_CAP, "queue holds exactly its capacity");
    CHECK(in_order, "oldest dropped, the rest in order");
}

int main(void)
{
    test_drops_log_keeps_order();
    test_drops_oldest_without_log();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
