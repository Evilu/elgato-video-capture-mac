// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OBS Studio source for the Elgato Video Capture (USB 0fd9:0037).
 * Feeds OBS the device's uncompressed 4:2:2 video and 48 kHz PCM audio directly,
 * with no intermediate encoder.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include "evc.h"

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void) { return "Elgato Video Capture (analogue)"; }
MODULE_EXPORT const char *obs_module_description(void) {
    return "Uncompressed capture from the Elgato Video Capture USB dongle (0fd9:0037)";
}

#define S_INPUT "input"
#define S_STD "standard"
#define S_BRIGHTNESS "brightness"
#define S_CONTRAST "contrast"
#define S_SATURATION "saturation"
#define S_HUE "hue"
#define STD_AUTO 2

struct evc_source {
    obs_source_t *source;
    pthread_t thread;
    bool thread_started;
    os_event_t *stop;

    pthread_mutex_t lock; /* guards the wanted settings below */
    int std, input, brightness, contrast, saturation, hue;
    bool reinit, picture_dirty;

    uint64_t last_frame_ns;
};

static void on_video(void *user, const uint8_t *uyvy, size_t len, int width, int height, uint64_t t, int complete) {
    struct evc_source *s = user;
    UNUSED_PARAMETER(len);
    UNUSED_PARAMETER(t);
    UNUSED_PARAMETER(complete);
    struct obs_source_frame frame = {0};
    frame.data[0] = (uint8_t *)uyvy;
    frame.linesize[0] = (uint32_t)width * 2;
    frame.width = (uint32_t)width;
    frame.height = (uint32_t)height;
    frame.format = VIDEO_FORMAT_UYVY;
    frame.timestamp = os_gettime_ns();
    frame.full_range = false;
    video_format_get_parameters_for_format(VIDEO_CS_601, VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_UYVY, frame.color_matrix,
                                           frame.color_range_min, frame.color_range_max);
    s->last_frame_ns = frame.timestamp;
    obs_source_output_video(s->source, &frame);
}

static void on_audio(void *user, const int16_t *pcm, size_t frames, uint64_t t) {
    struct evc_source *s = user;
    UNUSED_PARAMETER(t);
    struct obs_source_audio audio = {0};
    audio.data[0] = (const uint8_t *)pcm;
    audio.frames = (uint32_t)frames;
    audio.speakers = SPEAKERS_STEREO;
    audio.format = AUDIO_FORMAT_16BIT;
    audio.samples_per_sec = EVC_AUDIO_RATE;
    /* the block just finished arriving, so it started frames/rate ago */
    audio.timestamp = os_gettime_ns() - (uint64_t)frames * 1000000000ULL / EVC_AUDIO_RATE;
    obs_source_output_audio(s->source, &audio);
}

static void apply_picture(struct evc_source *s, evc *dev) {
    pthread_mutex_lock(&s->lock);
    int b = s->brightness, c = s->contrast, sat = s->saturation, h = s->hue;
    s->picture_dirty = false;
    pthread_mutex_unlock(&s->lock);
    evc_set_brightness(dev, b);
    evc_set_contrast(dev, c);
    evc_set_saturation(dev, sat);
    evc_set_hue(dev, h);
}

/* Owns the device: opens it, streams, and reopens after unplug or a settings change. */
static void *capture_thread(void *arg) {
    struct evc_source *s = arg;
    os_set_thread_name("elgato-video-capture");
    bool warned = false;

    while (os_event_try(s->stop) == EAGAIN) {
        evc *dev = NULL;
        pthread_mutex_lock(&s->lock);
        int std = s->std, input = s->input;
        s->reinit = false;
        pthread_mutex_unlock(&s->lock);

        if (evc_open(&dev, 0) || evc_init(dev, std == STD_AUTO ? EVC_STD_NTSC : std, input)) {
            if (!warned) blog(LOG_WARNING, "[elgato-video-capture] device not available; will keep trying");
            warned = true;
            if (dev) evc_close(dev);
            obs_source_output_video(s->source, NULL);
            os_event_timedwait(s->stop, 1000);
            continue;
        }
        warned = false;
        if (std == STD_AUTO) std = evc_autodetect_std(dev);
        apply_picture(s, dev);
        if (evc_start(dev, on_video, on_audio, s)) {
            blog(LOG_WARNING, "[elgato-video-capture] could not start streaming");
            evc_close(dev);
            os_event_timedwait(s->stop, 1000);
            continue;
        }
        blog(LOG_INFO, "[elgato-video-capture] streaming %s %s, 720x%d", std == EVC_STD_PAL ? "PAL" : "NTSC",
             input == EVC_INPUT_SVIDEO ? "S-Video" : "composite", evc_height(dev));
        s->last_frame_ns = os_gettime_ns();

        while (os_event_try(s->stop) == EAGAIN) {
            evc_poll(dev, 50);
            pthread_mutex_lock(&s->lock);
            bool reinit = s->reinit, dirty = s->picture_dirty;
            pthread_mutex_unlock(&s->lock);
            if (reinit) break;
            if (dirty) apply_picture(s, dev);
            if (!evc_alive(dev)) {
                blog(LOG_WARNING, "[elgato-video-capture] device stopped responding (unplugged?)");
                break;
            }
            if (os_gettime_ns() - s->last_frame_ns > 3000000000ULL) {
                blog(LOG_WARNING, "[elgato-video-capture] no frames for 3 s; restarting the device");
                break;
            }
        }
        evc_stop(dev);
        evc_close(dev);
        obs_source_output_video(s->source, NULL);
    }
    return NULL;
}

static void evc_source_update(void *data, obs_data_t *settings) {
    struct evc_source *s = data;
    pthread_mutex_lock(&s->lock);
    int std = (int)obs_data_get_int(settings, S_STD), input = (int)obs_data_get_int(settings, S_INPUT);
    if (std != s->std || input != s->input) s->reinit = true;
    s->std = std;
    s->input = input;
    s->brightness = (int)obs_data_get_int(settings, S_BRIGHTNESS);
    s->contrast = (int)obs_data_get_int(settings, S_CONTRAST);
    s->saturation = (int)obs_data_get_int(settings, S_SATURATION);
    s->hue = (int)obs_data_get_int(settings, S_HUE);
    s->picture_dirty = true;
    pthread_mutex_unlock(&s->lock);
}

static void *evc_source_create(obs_data_t *settings, obs_source_t *source) {
    struct evc_source *s = bzalloc(sizeof(*s));
    s->source = source;
    pthread_mutex_init(&s->lock, NULL);
    os_event_init(&s->stop, OS_EVENT_TYPE_MANUAL);
    evc_source_update(s, settings);
    s->thread_started = pthread_create(&s->thread, NULL, capture_thread, s) == 0;
    return s;
}

static void evc_source_destroy(void *data) {
    struct evc_source *s = data;
    os_event_signal(s->stop);
    if (s->thread_started) pthread_join(s->thread, NULL);
    os_event_destroy(s->stop);
    pthread_mutex_destroy(&s->lock);
    bfree(s);
}

static void evc_source_defaults(obs_data_t *settings) {
    obs_data_set_default_int(settings, S_INPUT, EVC_INPUT_COMPOSITE);
    obs_data_set_default_int(settings, S_STD, STD_AUTO);
    obs_data_set_default_int(settings, S_BRIGHTNESS, 128);
    obs_data_set_default_int(settings, S_CONTRAST, 128);
    obs_data_set_default_int(settings, S_SATURATION, 128);
    obs_data_set_default_int(settings, S_HUE, 0);
}

static obs_properties_t *evc_source_properties(void *data) {
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();
    obs_property_t *p = obs_properties_add_list(props, S_INPUT, "Input", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, "Composite (yellow RCA)", EVC_INPUT_COMPOSITE);
    obs_property_list_add_int(p, "S-Video", EVC_INPUT_SVIDEO);
    p = obs_properties_add_list(props, S_STD, "Video standard", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, "Automatic", STD_AUTO);
    obs_property_list_add_int(p, "NTSC (720x480, 29.97 fps)", EVC_STD_NTSC);
    obs_property_list_add_int(p, "PAL (720x576, 25 fps)", EVC_STD_PAL);
    obs_properties_add_int_slider(props, S_BRIGHTNESS, "Brightness", 0, 255, 1);
    obs_properties_add_int_slider(props, S_CONTRAST, "Contrast", 0, 255, 1);
    obs_properties_add_int_slider(props, S_SATURATION, "Saturation", 0, 255, 1);
    obs_properties_add_int_slider(props, S_HUE, "Hue", -128, 127, 1);
    return props;
}

static const char *evc_source_name(void *unused) {
    UNUSED_PARAMETER(unused);
    return "Elgato Video Capture (analogue)";
}

static struct obs_source_info evc_source_info = {
    .id = "elgato_video_capture_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = evc_source_name,
    .create = evc_source_create,
    .destroy = evc_source_destroy,
    .update = evc_source_update,
    .get_defaults = evc_source_defaults,
    .get_properties = evc_source_properties,
    .icon_type = OBS_ICON_TYPE_CAMERA,
};

bool obs_module_load(void) {
    obs_register_source(&evc_source_info);
    blog(LOG_INFO, "[elgato-video-capture] plugin loaded");
    return true;
}
