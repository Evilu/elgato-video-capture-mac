// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * evc-capture - bring up the Elgato Video Capture from user space and save what it sends.
 *
 *   evc-capture --info                         device + signal status only
 *   evc-capture --frames 60 --raw out.uyvy     raw UYVY frames, back to back
 *   evc-capture --ppm frame.ppm                last captured frame as a viewable image
 *   evc-capture --seconds 5 --wav out.wav      audio
 */
#include "evc.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop;
static void on_sigint(int s) { (void)s; g_stop = 1; }

typedef struct {
    FILE *raw, *wav;
    uint8_t *last;
    size_t last_len;
    int w, h, have_complete;
    uint64_t frames, complete, audio_frames, first_ns, last_ns;
    int64_t audio_peak;
} ctx;

static void on_video(void *user, const uint8_t *uyvy, size_t len, int w, int h, uint64_t t, int complete) {
    ctx *c = user;
    c->frames++;
    if (!c->first_ns) c->first_ns = t;
    c->last_ns = t;
    if (!complete) return;
    c->complete++;
    if (c->raw) fwrite(uyvy, 1, len, c->raw);
    if (!c->last) c->last = malloc((size_t)EVC_WIDTH * 2 * 576);
    if (!c->last) return;
    memcpy(c->last, uyvy, len);
    c->last_len = len;
    c->w = w;
    c->h = h;
    c->have_complete = 1;
}

static void on_audio(void *user, const int16_t *pcm, size_t frames, uint64_t t) {
    ctx *c = user;
    (void)t;
    c->audio_frames += frames;
    for (size_t i = 0; i < frames * 2; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > c->audio_peak) c->audio_peak = v;
    }
    if (c->wav) fwrite(pcm, 4, frames, c->wav);
}

static void wav_header(FILE *f, uint32_t data_bytes) {
    uint32_t rate = EVC_AUDIO_RATE, byte_rate = rate * 4, riff = 36 + data_bytes, fmt_len = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
}

static int clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* BT.601 limited-range UYVY -> RGB */
static int write_ppm(const char *path, const uint8_t *uyvy, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *p = uyvy + (size_t)y * w * 2;
        for (int x = 0; x < w; x++) {
            int Y = p[x * 2 + 1] - 16, U = p[(x & ~1) * 2] - 128, V = p[(x & ~1) * 2 + 2] - 128;
            uint8_t rgb[3] = {clamp((298 * Y + 409 * V + 128) >> 8),
                              clamp((298 * Y - 100 * U - 208 * V + 128) >> 8),
                              clamp((298 * Y + 516 * U + 128) >> 8)};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: evc-capture [--std auto|ntsc|pal] [--input composite|svideo] [--info]\n"
            "                   [--frames N | --seconds S] [--raw FILE] [--ppm FILE] [--wav FILE] [-v|-vv]\n");
}

int main(int argc, char **argv) {
    evc_std std = EVC_STD_NTSC;
    int auto_std = 1;
    evc_input input = EVC_INPUT_COMPOSITE;
    const char *raw = NULL, *ppm = NULL, *wav = NULL;
    int frames = 0, verbose = 0, info = 0;
    double seconds = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--std") && n) { auto_std = !strcmp(n, "auto"); std = !strcmp(n, "pal") ? EVC_STD_PAL : EVC_STD_NTSC; i++; }
        else if (!strcmp(a, "--input") && n) { input = !strcmp(n, "svideo") ? EVC_INPUT_SVIDEO : EVC_INPUT_COMPOSITE; i++; }
        else if (!strcmp(a, "--frames") && n) { frames = atoi(n); i++; }
        else if (!strcmp(a, "--seconds") && n) { seconds = atof(n); i++; }
        else if (!strcmp(a, "--raw") && n) { raw = n; i++; }
        else if (!strcmp(a, "--ppm") && n) { ppm = n; i++; }
        else if (!strcmp(a, "--wav") && n) { wav = n; i++; }
        else if (!strcmp(a, "--info")) info = 1;
        else if (!strcmp(a, "-v")) verbose = 1;
        else if (!strcmp(a, "-vv")) verbose = 2;
        else { usage(); return 2; }
    }
    if (!frames && seconds <= 0) seconds = 3;

    evc *e;
    if (evc_open(&e, verbose)) return 1; /* evc_open has said why */
    if (evc_init(e, std, input)) {
        fprintf(stderr, "device initialisation failed\n");
        evc_close(e);
        return 1;
    }

    if (auto_std) std = evc_autodetect_std(e);
    evc_signal sig = {0};
    struct timespec ts = {0, 300 * 1000 * 1000};
    nanosleep(&ts, NULL);
    evc_get_signal(e, &sig);
    printf("standard: %s  input: %s  %dx%d\n", std == EVC_STD_PAL ? "PAL" : "NTSC",
           input == EVC_INPUT_SVIDEO ? "S-Video" : "composite", EVC_WIDTH, evc_height(e));
    printf("signal: video %s, hlock %s, detected format code %d (GEN_STAT %08x)\n",
           sig.video_present ? "present" : "absent", sig.hlock ? "yes" : "no", sig.detected_fmt, sig.gen_stat);
    if (info) {
        evc_close(e);
        return 0;
    }

    ctx c = {0};
    if (raw && !(c.raw = fopen(raw, "wb"))) perror(raw);
    if (wav) {
        if ((c.wav = fopen(wav, "wb"))) wav_header(c.wav, 0);
        else perror(wav);
    }

    signal(SIGINT, on_sigint);
    if (evc_start(e, on_video, on_audio, &c)) {
        fprintf(stderr, "could not start streaming\n");
        evc_close(e);
        return 1;
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!g_stop) {
        evc_poll(e, 50);
        if (!evc_alive(e)) { fprintf(stderr, "the device stopped responding\n"); break; }
        if (frames && c.complete >= (uint64_t)frames) break;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (seconds > 0 && el >= seconds) break;
        if (frames && el > 10 + frames / 10.0) { fprintf(stderr, "timed out waiting for frames\n"); break; }
    }
    evc_stats st;
    evc_get_stats(e, &st);
    evc_stop(e);

    double span = c.frames > 1 ? (c.last_ns - c.first_ns) / 1e9 : 0;
    printf("video: %llu frames (%llu complete), %.2f fps, %llu iso packets (%llu errors), %.1f MB\n",
           (unsigned long long)c.frames, (unsigned long long)c.complete,
           span > 0 ? (c.frames - 1) / span : 0.0, (unsigned long long)st.iso_packets,
           (unsigned long long)st.iso_errors, st.bytes / 1e6);
    printf("audio: %llu sample frames (%.2f s at 48 kHz), peak %lld / 32768\n",
           (unsigned long long)c.audio_frames, c.audio_frames / 48000.0, (long long)c.audio_peak);

    if (c.wav) {
        wav_header(c.wav, (uint32_t)(c.audio_frames * 4));
        fclose(c.wav);
    }
    if (c.raw) fclose(c.raw);
    if (ppm) {
        if (c.have_complete && write_ppm(ppm, c.last, c.w, c.h) == 0) printf("wrote %s\n", ppm);
        else fprintf(stderr, "no complete frame to write to %s\n", ppm);
    }
    evc_close(e);
    return c.complete ? 0 : 3;
}
