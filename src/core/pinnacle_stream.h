/*
 * DV streaming control and raw capture for the Pinnacle 500-USB.
 *
 * pinnacle_stream_read_loop() delivers raw bytes off EP 0x88 to a callback;
 * it does not know about DV framing. DIF reassembly lives in
 * dv_reassembler.h so this module stays reusable for the analog path and
 * for a future GUI preview that wants the same raw feed.
 */

#ifndef PINNACLE_STREAM_H
#define PINNACLE_STREAM_H

#include "pinnacle_device.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

pinnacle_status_t pinnacle_stream_start(pinnacle_device_t *dev);
pinnacle_status_t pinnacle_stream_stop(pinnacle_device_t *dev);

/* Called with each chunk read from EP 0x88, in order. Return 0 to keep
 * reading, non-zero to stop the loop. */
typedef int (*pinnacle_data_cb)(const uint8_t *data, size_t len, void *user);

/* Reads from EP 0x88 until the callback returns non-zero or *stop_flag
 * becomes non-zero (checked between transfers; safe to set from a signal
 * handler as a sig_atomic_t). Returns PINNACLE_OK on a clean stop. */
pinnacle_status_t pinnacle_stream_read_loop(pinnacle_device_t *dev,
                                             pinnacle_data_cb cb, void *user,
                                             volatile int *stop_flag);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_STREAM_H */
