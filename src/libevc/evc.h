/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libevc - user-space driver for the Elgato Video Capture (USB 0fd9:0037),
 * a Conexant CX231xx "Polaris" analogue capture device.
 *
 * The register sequences follow the Linux cx231xx / cx25840 drivers (GPL-2.0-or-later),
 * cross-checked against the behaviour of the vendor's Windows driver. See docs/.
 */
#ifndef EVC_H
#define EVC_H

#include <stddef.h>
#include <stdint.h>

#define EVC_VID 0x0fd9
#define EVC_PID 0x0037

#define EVC_WIDTH 720
#define EVC_AUDIO_RATE 48000
#define EVC_AUDIO_CHANNELS 2

typedef enum { EVC_STD_NTSC = 0, EVC_STD_PAL = 1 } evc_std;
typedef enum { EVC_INPUT_COMPOSITE = 0, EVC_INPUT_SVIDEO = 1 } evc_input;

typedef struct evc evc;

/* One woven interlaced frame: 720 x height, packed UYVY ('2vuy'), top field on even rows. */
typedef void (*evc_video_cb)(void *user, const uint8_t *uyvy, size_t len, int width, int height,
                             uint64_t host_time_ns, int complete);
/* Interleaved signed 16-bit little-endian stereo PCM at 48 kHz. */
typedef void (*evc_audio_cb)(void *user, const int16_t *pcm, size_t frames, uint64_t host_time_ns);

typedef struct {
    int video_present;   /* decoder sees a video signal */
    int hlock;           /* horizontal lock */
    int detected_fmt;    /* decoder auto-detect code (AFD_FMT_STAT), 0 = none */
    uint32_t gen_stat;   /* raw GEN_STAT register */
} evc_signal;

typedef struct {
    uint64_t frames, incomplete_frames, iso_packets, iso_errors, bytes, audio_bytes;
} evc_stats;

/* verbose: -1 silent (the host logs for itself), 0 errors only, 1 progress, 2 every register access */
int evc_open(evc **out, int verbose);
void evc_close(evc *e);

/* Full device bring-up for the given standard and input. */
int evc_init(evc *e, evc_std std, evc_input input);
int evc_set_std(evc *e, evc_std std);
int evc_set_input(evc *e, evc_input input);
int evc_get_signal(evc *e, evc_signal *sig);
/* Waits briefly for the decoder to recognise the incoming signal, switches to the matching
 * standard and returns it. Keeps the current standard when there is no signal. Not while streaming. */
evc_std evc_autodetect_std(evc *e);

/* Picture controls, 0..255 (hue: -128..127). evc_init() sets the neutral values 128 / 128 / 128 / 0. */
int evc_set_brightness(evc *e, int v);
int evc_set_contrast(evc *e, int v);
int evc_set_saturation(evc *e, int v);
int evc_set_hue(evc *e, int v);

int evc_start(evc *e, evc_video_cb vcb, evc_audio_cb acb, void *user);
/* Returns -1 if some transfers could not be reclaimed. The handle is then unusable and
 * evc_close() deliberately leaks it, so the device stays claimed until the process exits. */
int evc_stop(evc *e);
/* Pump USB events for up to timeout_ms; call in a loop from one thread while streaming. */
int evc_poll(evc *e, int timeout_ms);
void evc_get_stats(evc *e, evc_stats *st);
int evc_height(evc *e);
/* 0 once streaming has died (device unplugged, or transfers failing). Stop, close and reopen. */
int evc_alive(evc *e);

/* Raw access, for diagnostics. */
int evc_reg_read(evc *e, uint16_t reg, uint32_t *val);               /* USB bridge register */
/* len is 1..4 bytes, saddr_len 0..2 */
int evc_i2c_read(evc *e, uint8_t addr8, uint16_t saddr, int saddr_len, uint32_t *val, int len);
int evc_i2c_write(evc *e, uint8_t addr8, uint16_t saddr, int saddr_len, uint32_t val, int len);

#endif
