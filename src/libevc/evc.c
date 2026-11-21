// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * libevc - user-space driver for the Elgato Video Capture (USB 0fd9:0037).
 *
 * Register sequences are a port of the Linux cx231xx and cx25840 drivers
 * (Copyright Conexant Systems, Hans Verkuil, and others; GPL-2.0-or-later),
 * restricted to what the Elgato V2 board uses: external A/V, no tuner.
 * Where the vendor's Windows driver configures this board differently from
 * Linux, the vendor's values are used; each such place is marked "vendor:".
 * docs/04-device-protocol.md lists them.
 */
#include "evc.h"

#include <libusb.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- USB bridge (vendor requests on EP0) ---- */
#define VRT_SET_I2C0 0x00
#define VRT_GET_I2C0 0x04
#define VRT_SET_REGISTER 0x0c
#define VRT_GET_REGISTER 0x0d

#define BOARD_CFG_STAT 0x00
#define TS_MODE_REG 0x04
#define EP_MODE_SET 0x18
#define PWR_CTL_EN 0x74
#define PWR_MODE_MASK 0x17f
#define PWR_AV_EN 0x08
#define PWR_ISO_EN 0x40
#define AVMODE_EXTERNAL_AV 0x30
#define ENABLE_EP3 0x04 /* audio */
#define ENABLE_EP4 0x08 /* video */

#define I2C_SPEED_100K 2
#define I2C_SYNC 1

/* ---- I2C slaves inside the chip (8-bit addresses) ---- */
#define AFE 0x60 /* analogue front end ("Colibri") */
#define VID 0x88 /* video/audio decoder ("Merlin", cx25840-like) */
#define I2S 0x98 /* audio ADC block ("Flatiron") */

/* AFE registers */
#define SUP_BLK_TUNE1 0x00
#define SUP_BLK_TUNE2 0x01
#define SUP_BLK_TUNE3 0x02
#define SUP_BLK_PLL2 0x05
#define SUP_BLK_PWRDN 0x08
#define ADC_COM_QUANT 0x0b
#define ADC_CAL_ATEST(ch) (0x22 + 0x20 * (ch))
#define ADC_PWRDN_CLAMP(ch) (0x23 + 0x20 * (ch))
#define ADC_DCSERVO_DEM(ch) (0x26 + 0x20 * (ch))
#define ADC_FB_FRCRST(ch) (0x27 + 0x20 * (ch))
#define ADC_INPUT(ch) (0x28 + 0x20 * (ch))
#define ADC_NTF_PRECLMP_EN(ch) (0x29 + 0x20 * (ch))
#define ADC_QGAIN_RES_TRM(ch) (0x2a + 0x20 * (ch))

/* I2S block */
#define CH_PWR_CTRL1 0x0e
#define CH_PWR_CTRL2 0x0f

/* Video decoder block */
#define AFE_CTRL 0x104
#define PIN_CTRL 0x120
#define AUD_IO_CTRL 0x124
#define DIF_MISC_CTRL 0x33c
#define DIF_SRC_PHASE_INC 0x340
#define MODE_CTRL 0x400
#define OUT_CTRL1 0x404
#define GEN_STAT 0x40c
#define HORIZ_TIM_CTRL 0x470
#define VERT_TIM_CTRL 0x474
#define DFE_CTRL1 0x488
#define DFE_CTRL3 0x490
#define DL_CTL 0x800
#define GENERAL_CTL 0x810
#define PATH1_CTL1 0x8d0
#define PATH1_VOL_CTL 0x8d4
#define PATH1_SC_CTL 0x8dc
#define BAND_OUT_SEL 0x910
#define AC97_CTL 0x91c
#define OUT_MODE_VIP11 2

/* vendor: AFE reference count for baseband input (Linux uses 0x23c) */
#define AFE_REF_COUNT 0x210

#define VIDEO_IFACE 3
#define VIDEO_EP 0x84
#define VIDEO_ALT 3
#define AUDIO_IFACE 2
#define AUDIO_EP 0x83
#define AUDIO_ALT 1

#define N_VIDEO_XFERS 8
#define VIDEO_PKTS 64
#define N_AUDIO_XFERS 4
#define AUDIO_PKTS 64
#define LINE_BYTES (EVC_WIDTH * 2)
#define MAX_HEIGHT 576

struct evc {
    libusb_context *ctx;
    libusb_device_handle *h;
    int verbose;
    int claimed_video, claimed_audio;
    uint16_t bcd_device;
    int io_err;            /* sticky: set by the first failed control transfer, see io_failed() */
    evc_std std;
    evc_input input;

    /* streaming */
    int streaming, active_xfers;
    int stream_failed;     /* a transfer could not be resubmitted, or too many failed in a row */
    int bad_video, bad_audio; /* consecutive transfers that did not complete, per endpoint */
    int leaked;            /* transfers were still in flight at stop: nothing may be freed */
    int16_t pcm[AUDIO_PKTS * 64];
    struct libusb_transfer *vx[N_VIDEO_XFERS], *ax[N_AUDIO_XFERS];
    evc_video_cb vcb;
    evc_audio_cb acb;
    void *user;
    evc_stats st;

    /* BT.656-style stream parser */
    int height, lines_per_field;
    int sync;              /* how many bytes of FF 00 00 matched so far */
    int field;             /* 1 or 2 while inside active video, else 0 */
    int line;              /* lines completed in the current field */
    int line_pos;          /* bytes copied into the current line, -1 = not in a line */
    int field1_lines, field2_lines;
    int damaged;           /* data was lost since field 1 of this frame began */
    uint8_t *frame;
    uint8_t line_buf[LINE_BYTES];
    mach_timebase_info_data_t tb;
    FILE *dump; /* EVC_DUMP_STREAM=path: raw video endpoint bytes, for diagnostics */
};

/* verbose < 0 is silent, for hosts that do their own logging */
static void elog(evc *e, const char *fmt, ...) {
    if (e->verbose < 0) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static uint64_t now_ns(evc *e) {
    return mach_absolute_time() * e->tb.numer / e->tb.denom;
}

static void msleep(int ms) { usleep(ms * 1000); }

/* ---------------------------------------------------------------- bridge registers */

/*
 * Control-transfer errors are sticky. After the first failure reads return 0 and writes are
 * not sent, so a failed read can never feed a stale or uninitialised value into a later
 * write. Each public entry point clears the flag on entry and reports it on exit.
 */
static int io_failed(evc *e, const char *what, unsigned a, unsigned b, int r) {
    if (!e->io_err) elog(e, "evc: %s %02x:%03x failed: %s\n", what, a, b, r < 0 ? libusb_error_name(r) : "short transfer");
    e->io_err = 1;
    return -1;
}

static int reg_read(evc *e, uint16_t reg, uint32_t *val) {
    uint8_t b[4] = {0};
    *val = 0;
    if (e->io_err) return -1;
    int r = libusb_control_transfer(e->h, 0xc0, VRT_GET_REGISTER, 0x0f, reg, b, 4, 1000);
    if (r != 4) return io_failed(e, "bridge read", 0, reg, r);
    *val = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    if (e->verbose > 1) fprintf(stderr, "  reg  R %04x = %08x\n", reg, *val);
    return 0;
}

static int reg_write(evc *e, uint16_t reg, uint32_t val) {
    uint8_t b[4] = {val, val >> 8, val >> 16, val >> 24};
    if (e->io_err) return -1;
    if (e->verbose > 1) fprintf(stderr, "  reg  W %04x = %08x\n", reg, val);
    int r = libusb_control_transfer(e->h, 0x40, VRT_SET_REGISTER, 0x0f, reg, b, 4, 1000);
    if (r != 4) return io_failed(e, "bridge write", 0, reg, r);
    return 0;
}

static int reg_or(evc *e, uint16_t reg, uint32_t bits) {
    uint32_t v;
    if (reg_read(e, reg, &v)) return -1;
    return reg_write(e, reg, v | bits);
}

/* ---------------------------------------------------------------- I2C over USB (master 0) */

static uint16_t i2c_wvalue(uint8_t addr8, int saddr_len) {
    return (uint16_t)((addr8 >> 1) << 9 | I2C_SPEED_100K << 4 | saddr_len << 2 | I2C_SYNC);
}

static int i2c_read(evc *e, uint8_t addr8, uint16_t saddr, int saddr_len, uint32_t *val, int len) {
    uint8_t b[4] = {0};
    *val = 0;
    if (len < 1 || len > 4 || saddr_len < 0 || saddr_len > 2) return -1;
    if (e->io_err) return -1;
    int r = libusb_control_transfer(e->h, 0xc0, VRT_GET_I2C0, i2c_wvalue(addr8, saddr_len), saddr, b, len, 1000);
    if (r != len) return io_failed(e, "i2c read", addr8, saddr, r);
    *val = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    if (e->verbose > 1) fprintf(stderr, "  i2c  R %02x:%03x = %0*x\n", addr8, saddr, len * 2, *val);
    return 0;
}

static int i2c_write(evc *e, uint8_t addr8, uint16_t saddr, int saddr_len, uint32_t val, int len) {
    uint8_t b[4] = {val, val >> 8, val >> 16, val >> 24};
    if (len < 1 || len > 4 || saddr_len < 0 || saddr_len > 2) return -1;
    if (e->io_err) return -1;
    if (e->verbose > 1) fprintf(stderr, "  i2c  W %02x:%03x = %0*x\n", addr8, saddr, len * 2, val);
    int r = libusb_control_transfer(e->h, 0x40, VRT_SET_I2C0, i2c_wvalue(addr8, saddr_len), saddr, b, len, 1000);
    if (r != len) return io_failed(e, "i2c write", addr8, saddr, r);
    return 0;
}

/* Public raw access: each call starts from a clean error state. */
int evc_reg_read(evc *e, uint16_t reg, uint32_t *val) {
    e->io_err = 0;
    return reg_read(e, reg, val);
}
int evc_i2c_read(evc *e, uint8_t addr8, uint16_t saddr, int saddr_len, uint32_t *val, int len) {
    e->io_err = 0;
    return i2c_read(e, addr8, saddr, saddr_len, val, len);
}
int evc_i2c_write(evc *e, uint8_t addr8, uint16_t saddr, int saddr_len, uint32_t val, int len) {
    e->io_err = 0;
    return i2c_write(e, addr8, saddr, saddr_len, val, len);
}

static int afe_w(evc *e, uint16_t a, uint8_t v) { return i2c_write(e, AFE, a, 2, v, 1); }
static int afe_r(evc *e, uint16_t a, uint8_t *v) {
    uint32_t t = 0;
    int r = i2c_read(e, AFE, a, 2, &t, 1);
    *v = (uint8_t)t;
    return r;
}
static int vid_w8(evc *e, uint16_t a, uint8_t v) { return i2c_write(e, VID, a, 2, v, 1); }
static int vid_r8(evc *e, uint16_t a, uint8_t *v) {
    uint32_t t = 0;
    int r = i2c_read(e, VID, a, 2, &t, 1);
    *v = (uint8_t)t;
    return r;
}
static int vid_w32(evc *e, uint16_t a, uint32_t v) { return i2c_write(e, VID, a, 2, v, 4); }
static int vid_r32(evc *e, uint16_t a, uint32_t *v) { return i2c_read(e, VID, a, 2, v, 4); }
static int vid_andor8(evc *e, uint16_t a, uint8_t and_mask, uint8_t or_val) {
    uint8_t v;
    if (vid_r8(e, a, &v)) return -1;
    return vid_w8(e, a, (v & and_mask) | or_val);
}
/* replace the bits in mask with value (value already shifted into place) */
static int vid_rmw32(evc *e, uint16_t a, uint32_t mask, uint32_t value) {
    uint32_t v;
    if (vid_r32(e, a, &v)) return -1;
    return vid_w32(e, a, (v & ~mask) | value);
}

/* ---------------------------------------------------------------- AFE */

static int afe_init_super_block(evc *e, uint32_t ref_count) {
    uint8_t st = 0;
    if (afe_w(e, SUP_BLK_TUNE2, ref_count & 0xff)) return -1;
    afe_r(e, SUP_BLK_TUNE2, &st);
    afe_w(e, SUP_BLK_TUNE1, ((ref_count & 0x300) >> 8) | 0x40);
    afe_w(e, SUP_BLK_PLL2, 0x0f);
    /* enable PLL */
    for (int i = 0; st != 0x18; i++) {
        if (i == 10) {
            elog(e, "evc: AFE PLL did not power up (status %02x)\n", st);
            return -1;
        }
        if (afe_w(e, SUP_BLK_PWRDN, 0x18) || afe_r(e, SUP_BLK_PWRDN, &st)) return -1;
    }
    /* start, then leave, filter tuning */
    afe_w(e, SUP_BLK_TUNE3, 0x40);
    msleep(5);
    if (afe_w(e, SUP_BLK_TUNE3, 0x00)) return -1;

    /* vendor: a tail Linux does not have, chosen by hardware revision */
    if (e->bcd_device < 0x4002) return afe_w(e, 0x0a, 0x72);
    uint8_t in, v;
    afe_w(e, 0x0a, 0x52);
    if (afe_r(e, ADC_INPUT(0), &in)) return -1;
    afe_w(e, ADC_INPUT(0), (in & 0xcf) | ((((in >> 4) + 1) & 3) << 4)); /* park ADC1 on the next input */
    msleep(10);
    afe_w(e, 0x21, 1); /* run a calibration */
    msleep(50);
    afe_w(e, 0x21, 0);
    msleep(50);
    if (afe_r(e, 0x21, &v)) return -1;
    if (v & 2) afe_w(e, 0x0a, 0x72);
    afe_w(e, ADC_INPUT(0), in);
    if (afe_r(e, ADC_FB_FRCRST(0), &v)) return -1;
    afe_w(e, ADC_FB_FRCRST(0), (v & 0xfd) | 1);
    return afe_w(e, SUP_BLK_TUNE1, 0xbc | ((ref_count >> 8) & 3));
}

static int afe_init_channels(evc *e) {
    uint8_t v;
    for (int ch = 0; ch < 3; ch++) afe_w(e, ADC_PWRDN_CLAMP(ch), 0x00); /* power up */
    afe_w(e, ADC_COM_QUANT, 0x02);                                      /* quantizer calibration */
    for (int ch = 0; ch < 3; ch++) afe_w(e, ADC_FB_FRCRST(ch), 0x17);   /* force modulator reset */
    for (int ch = 0; ch < 3; ch++) afe_w(e, ADC_CAL_ATEST(ch), 0x10);   /* start calibration */
    msleep(5);
    for (int ch = 0; ch < 3; ch++) afe_w(e, ADC_FB_FRCRST(ch), 0x07);   /* leave reset */
    for (int ch = 0; ch < 3; ch++) afe_w(e, ADC_NTF_PRECLMP_EN(ch), 0xf0); /* pre-clamp, single-ended */
    for (int ch = 0; ch < 3; ch++) {                                    /* term_en = res_en = 0 */
        if (afe_r(e, ADC_QGAIN_RES_TRM(ch), &v)) return -1;
        afe_w(e, ADC_QGAIN_RES_TRM(ch), v & ~0xf8);
    }
    for (int ch = 0; ch < 3; ch++) afe_w(e, ADC_DCSERVO_DEM(ch), 0x03); /* dynamic element matching off */
    return 0;
}

static int afe_power_external_av(evc *e) {
    uint8_t st = 0;
    for (int i = 0; st != 0x18; i++) {
        if (i == 10) return -1;
        if (afe_w(e, SUP_BLK_PWRDN, 0x18) || afe_r(e, SUP_BLK_PWRDN, &st)) return -1;
    }
    afe_w(e, ADC_PWRDN_CLAMP(0), 0x00);
    afe_w(e, ADC_PWRDN_CLAMP(1), 0x00);
    return afe_w(e, ADC_PWRDN_CLAMP(2), 0x00); /* vendor: all three channels on (Linux: 0x40) */
}

/* mux: byte 0 = pin for ADC channel 1, byte 1 = channel 2, byte 2 = channel 3 (0 = leave) */
static int afe_set_input_mux(evc *e, uint32_t mux) {
    for (int ch = 0; ch < 3; ch++) {
        uint8_t pin = mux >> (8 * ch);
        if (!pin) continue;
        /* vendor: the whole register is written (Linux changes only bits 5:4) */
        if (afe_w(e, ADC_INPUT(ch), (pin - 1) << 4)) return -1;
    }
    return 0;
}

static int afe_adjust_ref_count(evc *e) {
    uint8_t in = 0, ntf = 0;
    uint32_t ref = AFE_REF_COUNT;
    afe_r(e, ADC_INPUT(0), &in);
    afe_r(e, ADC_NTF_PRECLMP_EN(0), &ntf);
    switch ((ntf & 0x3) | ((in & 0x6) << 1)) {
    case 0x9:
    case 0xa: ref = 0x225; break; /* IF inputs; not reachable without a tuner */
    }
    return afe_init_super_block(e, ref);
}

/* ---------------------------------------------------------------- power, DIF, I2S */

static int set_power_external_av(evc *e) {
    uint32_t v;
    if (reg_read(e, PWR_CTL_EN, &v)) return -1;
    v &= ~PWR_MODE_MASK;
    v |= PWR_AV_EN;
    reg_write(e, PWR_CTL_EN, v);
    msleep(10);
    v |= PWR_ISO_EN;
    reg_write(e, PWR_CTL_EN, v);
    msleep(10);
    v |= AVMODE_EXTERNAL_AV;
    if (reg_write(e, PWR_CTL_EN, v)) return -1;
    msleep(10);
    if (afe_power_external_av(e)) return -1;
    return i2c_write(e, I2S, CH_PWR_CTRL2, 1, 0x00, 1);
}

static int dif_baseband(evc *e) {
    uint32_t v;
    vid_w32(e, DIF_SRC_PHASE_INC, 0x0df7df83);
    if (vid_r32(e, DIF_MISC_CTRL, &v)) return -1;
    return vid_w32(e, DIF_MISC_CTRL, v | 0x80000000); /* DIF bypass */
}

static int i2s_init(evc *e) {
    uint32_t v = 0;
    i2c_read(e, I2S, CH_PWR_CTRL1, 1, &v, 1);
    i2c_write(e, I2S, CH_PWR_CTRL1, 1, v | 0x80, 1); /* clock to delta-sigma + decimator */
    return i2c_write(e, I2S, CH_PWR_CTRL2, 1, 0x00, 1); /* power up all channels */
}

/* ---------------------------------------------------------------- decoder (cx25840-style) */

static void decoder_std_timing(evc *e) {
    int pal = e->std == EVC_STD_PAL;
    int hblank = pal ? 132 : 122, hactive = 720, burst = pal ? 93 : 0x5b;
    int vblank = pal ? 36 : 26, vactive = pal ? 580 : 487, vblank656 = pal ? 40 : 26;
    int src_dec = 0x21f, luma_lpf = pal ? 2 : 1, uv_lpf = 1, comb = pal ? 0x20 : 0x66;
    int sc = pal ? 688739 : 556063;

    vid_w8(e, 0x49f, pal ? 0x11 : 0x14);
    vid_w8(e, 0x470, hblank);
    vid_w8(e, 0x471, (((hblank >> 8) & 0x3) | (hactive << 4)) & 0xff);
    vid_w8(e, 0x472, hactive >> 4);
    vid_w8(e, 0x473, burst);
    vid_w8(e, 0x474, vblank);
    vid_w8(e, 0x475, (((vblank >> 8) & 0x3) | (vactive << 4)) & 0xff);
    vid_w8(e, 0x476, vactive >> 4);
    vid_w8(e, 0x477, vblank656);
    vid_w8(e, 0x478, src_dec & 0xff);
    vid_w8(e, 0x479, (src_dec >> 8) & 0xff);
    vid_w8(e, 0x47a, luma_lpf << 6 | ((uv_lpf << 4) & 0x30));
    vid_w8(e, 0x47b, comb);
    vid_w8(e, 0x47c, sc & 0xff);
    vid_w8(e, 0x47d, (sc >> 8) & 0xff);
    vid_w8(e, 0x47e, (sc >> 16) & 0xff);
    vid_w8(e, 0x47f, pal ? 0x01 : 0x00);
}

static void decoder_input_change(evc *e) {
    vid_w8(e, 0x402, 0x04);
    vid_w8(e, 0x49f, e->std == EVC_STD_NTSC ? 0x14 : 0x11);
    vid_andor8(e, 0x401, ~0x60, 0x00);
    vid_andor8(e, 0x401, ~0x60, 0x60);
    vid_andor8(e, GENERAL_CTL, ~0x01, 0x01);
    vid_w8(e, 0x808, e->std == EVC_STD_NTSC ? 0xf6 : 0xff);
    vid_w8(e, 0x80b, 0x00);
    vid_andor8(e, GENERAL_CTL, ~0x01, 0x00);
}

static void decoder_audio_path(evc *e) {
    vid_andor8(e, GENERAL_CTL, ~0x01, 0x01); /* soft reset */
    vid_andor8(e, 0x803, ~0x10, 0x00);       /* stop microcontroller */
    vid_w8(e, 0x8d3, 0x1f);                  /* mute */
    vid_w32(e, PATH1_CTL1, 0x1f063870);
    vid_w32(e, 0x900, 0x08014faa);           /* sample-rate converters for 48 kHz */
    vid_w32(e, 0x904, 0x08014faa);
    vid_w32(e, 0x90c, 0x08014faa);
    vid_andor8(e, 0x803, ~0x10, 0x10);
    vid_andor8(e, GENERAL_CTL, ~0x01, 0x00);
    vid_andor8(e, 0x803, ~0x10, 0x10);
}

/* the cx25840 view of input routing; reg is the value for register 0x103 */
static void decoder_route(evc *e, uint8_t reg, int composite) {
    vid_w8(e, 0x103, reg);
    vid_andor8(e, 0x401, ~0x06, composite ? 0x00 : 0x02);
    vid_andor8(e, 0x102, ~0x04, 0x00);
    vid_andor8(e, 0x102, ~0x02, 0x00);
    decoder_audio_path(e);
    decoder_input_change(e);
    vid_w8(e, 0x124, 0x03); /* audio channel 1 source: parallel 1 */
    vid_w8(e, 0x914, 0xa0); /* I2S in: Sony mode, left sample on WS=1 */
    vid_w8(e, 0x918, 0xa0); /* I2S out: same, master */
    vid_w8(e, 0x919, 0x01);
}

static int decoder_set_std(evc *e) {
    int fmt = e->std == EVC_STD_PAL ? 0x4 : 0x1;
    uint8_t t;
    if (fmt == 4) {
        /* cx25840 datasheet 3.16 step 9: go through NTSC-M with LCOMB off, avoids PAL ghosting */
        vid_andor8(e, 0x400, ~0x0f, 1);
        vid_andor8(e, 0x47b, ~0x06, 0);
    }
    if (vid_andor8(e, 0x400, ~0x0f, fmt)) return -1;
    if (fmt == 4) {
        vid_r8(e, 0x401, &t);
        vid_andor8(e, 0x401, ~0x40, t & 0x40 ? 0x00 : 0x40); /* toggle chroma AGC */
        vid_andor8(e, 0x401, ~0x40, t & 0x40 ? 0x40 : 0x00);
        vid_andor8(e, 0x401, ~0x20, t & 0x20 ? 0x00 : 0x20); /* toggle chroma killer */
        vid_andor8(e, 0x401, ~0x20, t & 0x20 ? 0x20 : 0x00);
    }
    decoder_std_timing(e);
    decoder_input_change(e);
    return 0;
}

static int decoder_initialize(evc *e) {
    vid_andor8(e, 0x102, ~0x01, 0x01); /* internal reset */
    vid_andor8(e, 0x102, ~0x01, 0x00);
    vid_andor8(e, 0x803, ~0x10, 0x00); /* stop microcontroller */
    vid_w8(e, 0x398, 0);               /* DIF */
    vid_w8(e, 0x2, 0x76);              /* default xtal, no division */
    vid_w8(e, 0x1, 0x40);              /* AUX clock regulator down */
    vid_w32(e, 0x33c, 0x00000001);
    vid_w32(e, 0x340, 0x0df7df83);     /* DIF source phase increment */
    vid_w32(e, 0x414, 0x00107d12);     /* luma */
    vid_w32(e, 0x420, 0x3d008282);     /* chroma */
    vid_w8(e, 0x102, 0x10);            /* ADC2 input select */
    vid_w8(e, 0x103, 0x11);            /* VIN1 & VIN5 */
    vid_w8(e, 0x400, 0);               /* format auto-detect */
    vid_w8(e, 0x401, 0xe8);            /* fast subchroma lock, white crush, chroma AGC + killer */
    /* (the optional audio microcontroller firmware is not loaded: only tuner audio needs it) */
    decoder_std_timing(e);
    decoder_route(e, 0xf6, 1);
    vid_andor8(e, 0x803, ~0x10, 0x10); /* start microcontroller */
    vid_w8(e, 0x404, 0x0b);            /* CC raw enable */
    vid_w8(e, 0x42f, 0x66);            /* CC on */
    return vid_w32(e, 0x474, 0x1e1e601a);
}

/* ---------------------------------------------------------------- chip-level input / standard */

static int set_video_input(evc *e) {
    uint32_t v;
    int svideo = e->input == EVC_INPUT_SVIDEO;
    /* composite: pin 2 -> ADC1.  s-video: luma pin 1 -> ADC1, chroma pin 3 -> ADC2.
     * vendor: these are the INF's XBarPin InputMux values; Linux has pin 1 for chroma. */
    uint32_t mux = svideo ? (1 | 3 << 8) : 2;

    if (afe_adjust_ref_count(e)) return -1;
    if (afe_set_input_mux(e, mux)) return -1;

    /*
     * Output framing, needed for either input: VIP 1.1 codes with bit 7 set on active
     * video (0x80 / 0xC0). Linux only does this on its composite path and relies on
     * composite having been selected once; without it S-Video lines arrive tagged
     * 0x00 / 0x40 (observed on hardware).
     */
    if (vid_r32(e, OUT_CTRL1, &v)) return -1;
    vid_w32(e, OUT_CTRL1, v | 1 << 7);
    vid_rmw32(e, OUT_CTRL1, 0x3, OUT_MODE_VIP11);

    if (!svideo) {
        if (vid_r32(e, AFE_CTRL, &v)) return -1;
        v |= 1 << 4;
        v &= ~(1u << 5);
        v &= ~0x1ff8000u;
        v |= 0x1000000; /* FUNC_MODE = baseband */
        vid_w32(e, AFE_CTRL, v);
        dif_baseband(e);
        vid_r32(e, DFE_CTRL1, &v);
        vid_w32(e, DFE_CTRL1, v | 0x08000000 | 0x10000000); /* VBI gate, auto VGA */
        vid_rmw32(e, MODE_CTRL, 0x20, 0x20);                 /* no register auto-config */
        vid_rmw32(e, MODE_CTRL, 0x600, 0 << 9);              /* CVBS */
    } else {
        if (vid_r32(e, AFE_CTRL, &v)) return -1;
        v &= ~0x1ff8000u;
        v |= 0x1000010;
        vid_w32(e, AFE_CTRL, v);
        dif_baseband(e);
        vid_r32(e, DFE_CTRL1, &v);
        vid_w32(e, DFE_CTRL1, v | 0x08000000 | 0x10000000);
        vid_rmw32(e, MODE_CTRL, 0x20, 0x20);
        vid_rmw32(e, MODE_CTRL, 0x600, 1 << 9); /* Y/C */
        vid_r32(e, AFE_CTRL, &v);
        v |= 0x1000;            /* chroma from ADC2 */
        v &= ~(0x80u | 0x100u); /* ch2/ch3 VGA carry video, not audio */
        vid_w32(e, AFE_CTRL, v);
        uint8_t c;
        afe_r(e, ADC_PWRDN_CLAMP(1), &c); /* AFE baseband mode */
        afe_w(e, ADC_PWRDN_CLAMP(1), c & ~0x50);
    }
    vid_rmw32(e, OUT_CTRL1, 0x8, 0x8); /* raw VBI in active area */
    vid_r32(e, OUT_CTRL1, &v);
    if (v & 0x02) vid_w32(e, OUT_CTRL1, v | 1 << 19);

    decoder_route(e, svideo ? 0x01 : 0xf1, !svideo);
    return 0;
}

static int set_audio_line_in(evc *e) {
    uint32_t v;
    i2c_write(e, I2S, CH_PWR_CTRL2, 1, 0x00, 1);
    i2c_write(e, I2S, CH_PWR_CTRL1, 1, 0x80, 1);

    vid_andor8(e, GENERAL_CTL, ~0x01, 0x01); /* soft reset */
    vid_w32(e, AUD_IO_CTRL, 0);              /* channel 1 from the decoder's parallel output */
    vid_r32(e, AC97_CTL, &v);
    vid_w32(e, AC97_CTL, v | 0x01000000);    /* bypass 2x upsampler */
    vid_w32(e, BAND_OUT_SEL, 0);             /* parallel 1 and SRC3 */
    vid_w32(e, DL_CTL, 0x3000001);
    vid_w32(e, PATH1_CTL1, 0x00063073);      /* unmute, AC97 in, independent */
    vid_r32(e, PATH1_VOL_CTL, &v);
    vid_w32(e, PATH1_VOL_CTL, v | 0x7fff0000);
    vid_r32(e, PATH1_SC_CTL, &v);
    vid_w32(e, PATH1_SC_CTL, v | 0x7fff0000);
    return vid_andor8(e, GENERAL_CTL, ~0x01, 0x00);
}

/* per-standard fix-ups after MODE_CTRL has been touched */
static int mode_overrides(evc *e) {
    vid_w32(e, DFE_CTRL3, 0xcd3f0280);
    if (e->std == EVC_STD_NTSC) {
        /* vendor: 486 active lines and 656 blanking 0x1e (Linux: 487 and 0x1c) */
        vid_rmw32(e, VERT_TIM_CTRL, 0x000003ff, 0x18);
        vid_rmw32(e, VERT_TIM_CTRL, 0x003ff000, 0x1e6000);
        vid_rmw32(e, VERT_TIM_CTRL, 0xff000000, 0x1e000000);
        return vid_rmw32(e, HORIZ_TIM_CTRL, 0x000003ff, 0x79);
    }
    /* vendor: only the blanking count is overridden for 625-line standards
     * (Linux: 0x20, and also rewrites active lines and 656 blanking) */
    vid_rmw32(e, VERT_TIM_CTRL, 0x000003ff, 0x24);
    return vid_rmw32(e, HORIZ_TIM_CTRL, 0x000003ff, 0x85);
}

static void set_geometry(evc *e) {
    e->height = e->std == EVC_STD_PAL ? 576 : 480;
    e->lines_per_field = e->height / 2;
}

/* ---------------------------------------------------------------- public control API */

int evc_open(evc **out, int verbose) {
    evc *e = calloc(1, sizeof(*e));
    if (!e) return -1;
    e->verbose = verbose;
    mach_timebase_info(&e->tb);
    if (getenv("EVC_DUMP_STREAM")) e->dump = fopen(getenv("EVC_DUMP_STREAM"), "wb");
    if (libusb_init(&e->ctx)) goto fail;
    e->h = libusb_open_device_with_vid_pid(e->ctx, EVC_VID, EVC_PID);
    if (!e->h) {
        elog(e, "evc: no Elgato Video Capture (%04x:%04x) found, or it could not be opened\n", EVC_VID, EVC_PID);
        goto fail;
    }
    struct libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(libusb_get_device(e->h), &dd) == 0) e->bcd_device = dd.bcdDevice;
    int r = libusb_claim_interface(e->h, VIDEO_IFACE);
    if (r) {
        elog(e, "evc: cannot claim video interface: %s\n", libusb_error_name(r));
        goto fail;
    }
    e->claimed_video = 1;
    r = libusb_claim_interface(e->h, AUDIO_IFACE);
    if (r) elog(e, "evc: cannot claim audio interface (%s); continuing without audio\n", libusb_error_name(r));
    else e->claimed_audio = 1;
    e->frame = malloc(LINE_BYTES * MAX_HEIGHT);
    if (!e->frame) goto fail;
    e->std = EVC_STD_NTSC;
    set_geometry(e);
    *out = e;
    return 0;
fail:
    evc_close(e);
    return -1;
}

void evc_close(evc *e) {
    if (!e) return;
    if (e->streaming) evc_stop(e);
    if (e->leaked) {
        /* evc_stop could not get every transfer back. libusb may still complete them and
         * call into this object, so the only safe thing is to give all of it up. */
        elog(e, "evc: transfers still in flight at close; leaking the device handle\n");
        return;
    }
    if (e->h) {
        if (e->claimed_video) libusb_release_interface(e->h, VIDEO_IFACE);
        if (e->claimed_audio) libusb_release_interface(e->h, AUDIO_IFACE);
        libusb_close(e->h);
    }
    if (e->ctx) libusb_exit(e->ctx);
    if (e->dump) fclose(e->dump);
    free(e->frame);
    free(e);
}

int evc_init(evc *e, evc_std std, evc_input input) {
    uint32_t v;
    e->io_err = 0;
    e->std = std;
    e->input = input;
    set_geometry(e);

    if (reg_read(e, BOARD_CFG_STAT, &v)) return -1;
    if (e->verbose) elog(e, "evc: board config %08x, hardware revision %04x\n", v, e->bcd_device);

    if (set_power_external_av(e)) return -1;
    if (afe_init_super_block(e, AFE_REF_COUNT)) return -1;
    if (afe_init_channels(e)) return -1;
    if (dif_baseband(e)) return -1;
    if (i2s_init(e)) return -1;
    /* control pins: drive everything the board mask (0xFFFFFFC4) leaves clear */
    if (vid_r32(e, PIN_CTRL, &v)) return -1;
    vid_w32(e, PIN_CTRL, v | ~0xffffffc4u);

    if (decoder_initialize(e)) return -1;
    if (set_video_input(e)) return -1;
    if (set_audio_line_in(e)) return -1;
    if (mode_overrides(e)) return -1;
    if (decoder_set_std(e)) return -1;
    mode_overrides(e);
    /* vendor: neutral picture controls (the cx25840 defaults differ slightly). These are
     * the values evc_set_brightness() etc. produce at 128 / 128 / 128 / 0. */
    vid_w8(e, 0x414, 0x00);
    vid_w8(e, 0x415, 0x80);
    vid_w8(e, 0x420, 0x80);
    vid_w8(e, 0x421, 0x80);
    vid_w8(e, 0x422, 0x00);
    /* many helpers above ignore individual results; the sticky flag catches all of them */
    return e->io_err ? -1 : 0;
}

int evc_set_std(evc *e, evc_std std) {
    if (e->streaming) return -1;
    e->io_err = 0;
    e->std = std;
    set_geometry(e);
    decoder_set_std(e);
    mode_overrides(e);
    return e->io_err ? -1 : 0;
}

int evc_set_input(evc *e, evc_input input) {
    e->io_err = 0;
    e->input = input;
    set_video_input(e);
    set_audio_line_in(e);
    mode_overrides(e);
    return e->io_err ? -1 : 0;
}

int evc_get_signal(evc *e, evc_signal *sig) {
    uint32_t v;
    e->io_err = 0;
    if (vid_r32(e, GEN_STAT, &v)) return -1;
    sig->gen_stat = v;
    sig->video_present = !!(v & 0x00200000);
    sig->hlock = !!(v & 0x00010000);
    sig->detected_fmt = (v & 0x00000f00) >> 8;
    return 0;
}

evc_std evc_autodetect_std(evc *e) {
    evc_signal sig;
    int fmt = 0, same = 0;
    /* the decoder reports its own guess in GEN_STAT whatever standard is programmed;
     * wait until it has video and gives the same answer three times running */
    for (int i = 0; i < 15 && same < 3; i++) {
        msleep(100);
        if (evc_get_signal(e, &sig) || !sig.video_present) {
            same = 0;
            continue;
        }
        same = sig.detected_fmt == fmt ? same + 1 : 1;
        fmt = sig.detected_fmt;
    }
    if (same < 3 || fmt == 0) return e->std;
    /* 4 PAL-BDGHI, 6 PAL-N, 7 PAL-Nc, 0xc SECAM are 625-line; the rest are 525-line */
    evc_std want = (fmt == 4 || fmt == 6 || fmt == 7 || fmt == 0xc) ? EVC_STD_PAL : EVC_STD_NTSC;
    if (want != e->std) evc_set_std(e, want);
    return e->std;
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int pic_w(evc *e, uint16_t reg, uint8_t v) {
    e->io_err = 0;
    return vid_w8(e, reg, v);
}
int evc_set_brightness(evc *e, int v) { return pic_w(e, 0x414, (uint8_t)(clamp8(v) - 128)); }
int evc_set_contrast(evc *e, int v) { return pic_w(e, 0x415, clamp8(v)); }
int evc_set_saturation(evc *e, int v) { return pic_w(e, 0x420, clamp8(v)) | pic_w(e, 0x421, clamp8(v)); }
int evc_set_hue(evc *e, int v) { return pic_w(e, 0x422, (uint8_t)(int8_t)(v < -128 ? -128 : v > 127 ? 127 : v)); }

int evc_height(evc *e) { return e->height; }
int evc_alive(evc *e) { return e->streaming && !e->stream_failed; }
void evc_get_stats(evc *e, evc_stats *st) { *st = e->st; }

/* ---------------------------------------------------------------- video stream parser */

static void emit_frame(evc *e, int complete) {
    /* a field that lost a line can still reach its line count from the spare lines the
     * decoder sends, with everything after the gap one row too high */
    if (e->damaged) complete = 0;
    e->st.frames++;
    if (!complete) e->st.incomplete_frames++;
    if (e->vcb) e->vcb(e->user, e->frame, (size_t)LINE_BYTES * e->height, EVC_WIDTH, e->height, now_ns(e), complete);
}

static void begin_field(evc *e, int field) {
    if (field == 1) {
        /* a new frame starts; flush the previous one if its second field never completed */
        if (e->field == 2 && e->field2_lines < e->lines_per_field && e->field2_lines > 0) emit_frame(e, 0);
        e->field1_lines = e->field2_lines = 0;
        e->damaged = 0;
    }
    e->field = field;
    e->line = 0;
}

static void end_line(evc *e) {
    if (e->line < e->lines_per_field) {
        int row = e->line * 2 + (e->field == 2);
        memcpy(e->frame + (size_t)row * LINE_BYTES, e->line_buf, LINE_BYTES);
        e->line++;
        if (e->field == 1) e->field1_lines = e->line;
        else e->field2_lines = e->line;
        if (e->field == 2 && e->line == e->lines_per_field) emit_frame(e, e->field1_lines == e->lines_per_field);
    }
    e->line_pos = -1;
}

/*
 * The video endpoint carries a BT.656-like byte stream with no per-packet header:
 * FF 00 00 XY timing codes, where XY's top nibble is 0x8 / 0xC for the start of an
 * active line in field 1 / field 2, followed by 1440 bytes of UYVY.
 */
static void parse_video(evc *e, const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (e->line_pos >= 0) {
            size_t want = (size_t)(LINE_BYTES - e->line_pos), have = n - i;
            size_t take = want < have ? want : have;
            memcpy(e->line_buf + e->line_pos, p + i, take);
            int was = e->line_pos;
            e->line_pos += (int)take;
            i += take;
            /* an end-of-line code straight after the start code means an empty line: drop it */
            if (was < 4 && e->line_pos >= 4 && e->line_buf[0] == 0xff && e->line_buf[1] == 0 &&
                e->line_buf[2] == 0 && (e->line_buf[3] & 0x90) == 0x90) {
                i -= (size_t)(e->line_pos - 4) <= i ? (size_t)(e->line_pos - 4) : i;
                e->line_pos = -1;
                e->sync = 0;
                continue;
            }
            if (e->line_pos == LINE_BYTES) end_line(e);
            continue;
        }
        uint8_t b = p[i++];
        if (e->sync == 0) {
            if (b == 0xff) e->sync = 1;
            else {
                const uint8_t *q = memchr(p + i, 0xff, n - i);
                i = q ? (size_t)(q - p) : n;
            }
        } else if (e->sync == 1 || e->sync == 2) {
            e->sync = b == 0x00 ? e->sync + 1 : (b == 0xff ? 1 : 0);
        } else {
            e->sync = 0;
            int code = b & 0xf0;
            if (code == 0x80 || code == 0xc0) {
                int field = code == 0x80 ? 1 : 2;
                if (field != e->field) begin_field(e, field);
                e->line_pos = 0;
            } else if (b == 0xff) {
                e->sync = 1;
            }
        }
    }
}

/* ---------------------------------------------------------------- streaming */

#define MAX_BAD_XFERS 50 /* the vendor driver gives up on a pipe after the same number */

/*
 * Common tail of both completion callbacks. The pool of queued transfers must stay full:
 * losing even one leaves the endpoint unserviced for part of every cycle. So anything that
 * takes a transfer out of the pool while streaming marks the stream as failed, which
 * evc_alive() reports and the host answers by reopening the device.
 */
static void requeue(evc *e, struct libusb_transfer *t, int *bad) {
    if (t->status == LIBUSB_TRANSFER_COMPLETED) *bad = 0;
    else if (t->status != LIBUSB_TRANSFER_CANCELLED && ++*bad >= MAX_BAD_XFERS) e->stream_failed = 1;

    if (e->streaming && !e->stream_failed && t->status != LIBUSB_TRANSFER_CANCELLED &&
        t->status != LIBUSB_TRANSFER_NO_DEVICE && libusb_submit_transfer(t) == 0)
        return;
    if (e->streaming) e->stream_failed = 1;
    e->active_xfers--;
}

static void LIBUSB_CALL video_done(struct libusb_transfer *t) {
    evc *e = t->user_data;
    if (!e->streaming) { /* stopping: the host's callbacks may already be gone */
        requeue(e, t, &e->bad_video);
        return;
    }
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[i];
            e->st.iso_packets++;
            if (d->status != LIBUSB_TRANSFER_COMPLETED) {
                /* bytes are missing: abandon the current line and find the next timing code
                 * rather than splice two lines together. The lost line is not counted, so
                 * the rest of the field sits a row high; the frame is flagged instead. */
                e->st.iso_errors++;
                e->line_pos = -1;
                e->sync = 0;
                e->damaged = 1;
                continue;
            }
            if (!d->actual_length) continue;
            e->st.bytes += d->actual_length;
            if (e->dump) fwrite(libusb_get_iso_packet_buffer_simple(t, i), 1, d->actual_length, e->dump);
            parse_video(e, libusb_get_iso_packet_buffer_simple(t, i), d->actual_length);
        }
    } else {
        e->line_pos = -1;
        e->sync = 0;
        e->damaged = 1;
    }
    requeue(e, t, &e->bad_video);
}

static void LIBUSB_CALL audio_done(struct libusb_transfer *t) {
    evc *e = t->user_data;
    if (t->status == LIBUSB_TRANSFER_COMPLETED && e->streaming) {
        size_t n = 0;
        for (int i = 0; i < t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[i];
            if (d->status != LIBUSB_TRANSFER_COMPLETED || !d->actual_length) continue;
            size_t len = d->actual_length & ~3u;
            if (n + len > sizeof(e->pcm)) break;
            memcpy((uint8_t *)e->pcm + n, libusb_get_iso_packet_buffer_simple(t, i), len);
            n += len;
        }
        e->st.audio_bytes += n;
        if (n && e->acb) e->acb(e->user, e->pcm, n / 4, now_ns(e));
    }
    requeue(e, t, &e->bad_audio);
}

static int start_iso(evc *e, struct libusb_transfer **x, int count, int pkts, int iface, int alt, uint8_t ep,
                     libusb_transfer_cb_fn cb) {
    /* the size must come from the selected alternate setting (alt 0 is a bulk placeholder) */
    int mps = libusb_get_max_alt_packet_size(libusb_get_device(e->h), iface, alt, ep);
    if (mps <= 0) {
        elog(e, "evc: endpoint %02x has no isochronous packet size\n", ep);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        x[i] = libusb_alloc_transfer(pkts);
        uint8_t *buf = malloc((size_t)mps * pkts);
        if (!x[i] || !buf) {
            free(buf);
            return -1;
        }
        libusb_fill_iso_transfer(x[i], e->h, ep, buf, mps * pkts, pkts, cb, e, 0);
        x[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
        libusb_set_iso_packet_lengths(x[i], mps);
        int r = libusb_submit_transfer(x[i]);
        if (r) {
            elog(e, "evc: submit on endpoint %02x failed: %s\n", ep, libusb_error_name(r));
            return -1;
        }
        e->active_xfers++;
    }
    if (e->verbose) elog(e, "evc: endpoint %02x streaming, %d bytes/packet\n", ep, mps);
    return 0;
}

int evc_start(evc *e, evc_video_cb vcb, evc_audio_cb acb, void *user) {
    int r;
    if (e->streaming || e->leaked) return -1;
    e->io_err = 0;
    e->stream_failed = e->bad_video = e->bad_audio = 0;
    e->vcb = vcb;
    e->acb = acb;
    e->user = user;
    memset(&e->st, 0, sizeof(e->st));
    e->sync = e->field = e->line = e->field1_lines = e->field2_lines = e->damaged = 0;
    e->line_pos = -1;
    memset(e->frame, 0, LINE_BYTES * MAX_HEIGHT);

    if ((r = libusb_set_interface_alt_setting(e->h, VIDEO_IFACE, VIDEO_ALT))) {
        elog(e, "evc: cannot select video alt setting: %s\n", libusb_error_name(r));
        return -1;
    }
    e->streaming = 1;
    if (start_iso(e, e->vx, N_VIDEO_XFERS, VIDEO_PKTS, VIDEO_IFACE, VIDEO_ALT, VIDEO_EP, video_done)) return -1;
    reg_or(e, TS_MODE_REG, 0x100);
    if (reg_or(e, EP_MODE_SET, ENABLE_EP4)) return -1;

    if (acb && e->claimed_audio) {
        if ((r = libusb_set_interface_alt_setting(e->h, AUDIO_IFACE, AUDIO_ALT))) {
            elog(e, "evc: cannot select audio alt setting: %s\n", libusb_error_name(r));
        } else if (start_iso(e, e->ax, N_AUDIO_XFERS, AUDIO_PKTS, AUDIO_IFACE, AUDIO_ALT, AUDIO_EP, audio_done) == 0) {
            /* Linux also ORs 0x9300 into TS_MODE_REG here; the vendor driver does that only
             * for audio embedded in the HANC stream, which this board does not use. */
            reg_or(e, EP_MODE_SET, ENABLE_EP3);
        }
    }
    return 0;
}

int evc_stop(evc *e) {
    uint32_t v;
    if (!e->streaming) return 0;
    e->streaming = 0;
    e->io_err = 0;
    if (reg_read(e, EP_MODE_SET, &v) == 0) reg_write(e, EP_MODE_SET, v & ~(uint32_t)(ENABLE_EP3 | ENABLE_EP4));
    for (int i = 0; i < N_VIDEO_XFERS; i++)
        if (e->vx[i]) libusb_cancel_transfer(e->vx[i]);
    for (int i = 0; i < N_AUDIO_XFERS; i++)
        if (e->ax[i]) libusb_cancel_transfer(e->ax[i]);
    /* every submitted transfer comes back through its callback, cancelled or not */
    for (int tries = 0; e->active_xfers > 0 && tries < 250; tries++) {
        struct timeval tv = {0, 20000};
        libusb_handle_events_timeout(e->ctx, &tv);
    }
    if (e->active_xfers > 0) {
        /* libusb still owns some transfers after 5 s. Freeing them, or this object, would
         * be a use-after-free when they do complete; evc_close() leaks instead. */
        e->leaked = 1;
        return -1;
    }
    for (int i = 0; i < N_VIDEO_XFERS; i++) {
        if (e->vx[i]) libusb_free_transfer(e->vx[i]);
        e->vx[i] = NULL;
    }
    for (int i = 0; i < N_AUDIO_XFERS; i++) {
        if (e->ax[i]) libusb_free_transfer(e->ax[i]);
        e->ax[i] = NULL;
    }
    libusb_set_interface_alt_setting(e->h, VIDEO_IFACE, 0);
    if (e->claimed_audio) libusb_set_interface_alt_setting(e->h, AUDIO_IFACE, 0);
    return 0;
}

int evc_poll(evc *e, int timeout_ms) {
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return libusb_handle_events_timeout(e->ctx, &tv);
}
