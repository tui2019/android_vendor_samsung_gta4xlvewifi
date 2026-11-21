/*
 * Mainline ALSA Primary Audio HAL (audio.primary.atoll.so) for SM-P613 (gta4xlvewifi)
 * Speaks directly to mainline Linux QDSP6 ALSA card0:
 *   - Speakers: MultiMedia1 (pcmC0D0p) -> QUIN_MI2S_RX -> dual TAS2562
 *   - Headphones: MultiMedia1 (pcmC0D0p) -> RX_CODEC_DMA_RX_0 -> WCD9380 HPHL/HPHR
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

#include <hardware/audio.h>
#include <hardware/hardware.h>

extern int *__errno(void);
extern int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define B_ERRNO (*__errno())
#define LOGI(...) __android_log_print(4, "audio_hw_p613", __VA_ARGS__)
#define LOGE(...) __android_log_print(6, "audio_hw_p613", __VA_ARGS__)

#define SAMPLE_RATE   48000
#define CHANNELS      2
#define FRAME_BYTES   4
#define PERIOD_FRAMES 960
#define PERIOD_COUNT  4

struct p613_audio_dev {
    struct audio_hw_device dev;
    pthread_mutex_t lock;
    bool mic_mute;
    audio_mode_t mode;
    int active_out_count;
    audio_patch_handle_t next_patch_handle;
    struct p613_stream_out *out_streams[4];
};

struct p613_stream_out {
    struct audio_stream_out stream;
    struct p613_audio_dev *adev;
    pthread_mutex_t lock;
    audio_devices_t devices;
    int pcm_fd;
    int ctl_fd;
    int pcm_dev_id;
    bool standby;
    bool routed_hph;
    uint64_t written_frames;
};

struct p613_stream_in {
    struct audio_stream_in stream;
    struct p613_audio_dev *adev;
    pthread_mutex_t lock;
    uint32_t rate;
    audio_channel_mask_t ch_mask;
    audio_devices_t device;
    bool standby;
    int64_t frames_read;
};

static void set_ctl_int(int cfd, const char *name, long v) {
    if (cfd < 0) return;
    struct snd_ctl_elem_value val;
    memset(&val, 0, sizeof(val));
    val.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    strncpy((char *)val.id.name, name, sizeof(val.id.name) - 1);
    val.value.integer.value[0] = v;
    if (ioctl(cfd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) {
        LOGE("mixer int '%s'=%ld err=%d", name, v, B_ERRNO);
    }
}

static void set_ctl_enum(int cfd, const char *name, const char *item) {
    if (cfd < 0) return;
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof(info));
    info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    strncpy((char *)info.id.name, name, sizeof(info.id.name) - 1);
    if (ioctl(cfd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) return;
    unsigned n = info.value.enumerated.items;
    int match = -1;
    for (unsigned i = 0; i < n; i++) {
        info.value.enumerated.item = i;
        if (ioctl(cfd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0 &&
            strcmp(info.value.enumerated.name, item) == 0) {
            match = (int)i;
            break;
        }
    }
    if (match < 0) return;
    struct snd_ctl_elem_value val;
    memset(&val, 0, sizeof(val));
    val.id = info.id;
    val.value.enumerated.item[0] = (unsigned)match;
    ioctl(cfd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val);
}

static void route_speaker(int cfd, int mm_idx, bool on) {
    char ctl[64];
    snprintf(ctl, sizeof(ctl), "QUIN_MI2S_RX Audio Mixer MultiMedia%d", mm_idx);
    set_ctl_int(cfd, ctl, on ? 1 : 0);
    if (on) {
        set_ctl_int(cfd, "Left Amp Gain Volume", 18);
        set_ctl_int(cfd, "Right Amp Gain Volume", 18);
        set_ctl_int(cfd, "Left Digital Volume Control", 94);
        set_ctl_int(cfd, "Right Digital Volume Control", 94);
    }
}

static void route_headphones(int cfd, int mm_idx, bool on) {
    char ctl[64];
    snprintf(ctl, sizeof(ctl), "RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia%d", mm_idx);
    if (on) {
        set_ctl_enum(cfd, "RX_MACRO RX0 MUX", "AIF1_PB");
        set_ctl_enum(cfd, "RX_MACRO RX1 MUX", "AIF1_PB");
        set_ctl_enum(cfd, "RX INT0_1 MIX1 INP0", "RX0");
        set_ctl_enum(cfd, "RX INT1_1 MIX1 INP0", "RX1");
        set_ctl_enum(cfd, "RX INT0 DEM MUX", "CLSH_DSM_OUT");
        set_ctl_enum(cfd, "RX INT1 DEM MUX", "CLSH_DSM_OUT");
        set_ctl_int(cfd, "RX_COMP1 Switch", 1);
        set_ctl_int(cfd, "RX_COMP2 Switch", 1);
        set_ctl_int(cfd, "HPHL_COMP Switch", 1);
        set_ctl_int(cfd, "HPHR_COMP Switch", 1);
        set_ctl_int(cfd, "HPHL_RDAC Switch", 1);
        set_ctl_int(cfd, "HPHR_RDAC Switch", 1);
        set_ctl_int(cfd, "HPHL Switch", 1);
        set_ctl_int(cfd, "HPHR Switch", 1);
        set_ctl_int(cfd, "RX_RX0 Digital Volume", 84);
        set_ctl_int(cfd, "RX_RX1 Digital Volume", 84);
        set_ctl_int(cfd, "HPHL Volume", 20);
        set_ctl_int(cfd, "HPHR Volume", 20);
        set_ctl_int(cfd, ctl, 1);
    } else {
        set_ctl_int(cfd, ctl, 0);
        set_ctl_int(cfd, "HPHL Switch", 0);
        set_ctl_int(cfd, "HPHR Switch", 0);
        set_ctl_int(cfd, "HPHL_RDAC Switch", 0);
        set_ctl_int(cfd, "HPHR_RDAC Switch", 0);
        set_ctl_int(cfd, "RX_COMP1 Switch", 0);
        set_ctl_int(cfd, "RX_COMP2 Switch", 0);
        set_ctl_enum(cfd, "RX INT0_1 MIX1 INP0", "ZERO");
        set_ctl_enum(cfd, "RX INT1_1 MIX1 INP0", "ZERO");
        set_ctl_enum(cfd, "RX_MACRO RX0 MUX", "ZERO");
        set_ctl_enum(cfd, "RX_MACRO RX1 MUX", "ZERO");
    }
}

static void param_set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit) {
    struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m->bits, 0, sizeof(m->bits));
    m->bits[bit / 32] |= (1u << (bit % 32));
}
static void param_set_int(struct snd_pcm_hw_params *p, int n, unsigned v) {
    struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = v; i->max = v; i->integer = 1; i->openmin = 0; i->openmax = 0;
}
static void param_init(struct snd_pcm_hw_params *p) {
    memset(p, 0, sizeof(*p));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits, 0xff, sizeof(p->masks[0].bits));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 0;
        p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = ~0u;
    }
    p->rmask = ~0u;
}

static bool use_headphones(audio_devices_t d) {
    return (d & (AUDIO_DEVICE_OUT_WIRED_HEADSET | AUDIO_DEVICE_OUT_WIRED_HEADPHONE)) != 0;
}

static int out_open_pcm_locked(struct p613_stream_out *out) {
    int cfd = open("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
    if (cfd < 0) return -1;

    bool hph = use_headphones(out->devices);
    int mm = out->pcm_dev_id + 1;
    if (hph) {
        route_speaker(cfd, mm, false);
        route_headphones(cfd, mm, true);
    } else {
        route_headphones(cfd, mm, false);
        route_speaker(cfd, mm, true);
    }

    char path[32];
    snprintf(path, sizeof(path), "/dev/snd/pcmC0D%dp", out->pcm_dev_id);
    int pfd = open(path, O_RDWR | O_CLOEXEC);
    if (pfd < 0) {
        LOGE("open %s err=%d", path, B_ERRNO);
        close(cfd);
        return -1;
    }

    struct snd_pcm_hw_params hp;
    param_init(&hp);
    param_set_mask(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    param_set_int(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, CHANNELS);
    param_set_int(&hp, SNDRV_PCM_HW_PARAM_RATE, SAMPLE_RATE);
    param_set_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD_FRAMES);
    param_set_int(&hp, SNDRV_PCM_HW_PARAM_PERIODS, PERIOD_COUNT);
    if (ioctl(pfd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
        LOGE("HW_PARAMS %s err=%d", path, B_ERRNO);
        close(pfd);
        close(cfd);
        return -1;
    }

    struct snd_pcm_sw_params sp;
    memset(&sp, 0, sizeof(sp));
    sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sp.period_step = 1;
    sp.avail_min = PERIOD_FRAMES;
    sp.start_threshold = PERIOD_FRAMES;
    sp.stop_threshold = PERIOD_FRAMES * PERIOD_COUNT;
    sp.boundary = PERIOD_FRAMES * PERIOD_COUNT;
    ioctl(pfd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp);
    if (ioctl(pfd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        LOGE("PREPARE %s err=%d", path, B_ERRNO);
        close(pfd);
        close(cfd);
        return -1;
    }

    out->ctl_fd = cfd;
    out->pcm_fd = pfd;
    out->routed_hph = hph;
    out->standby = false;
    LOGI("opened %s (route=%s)", path, hph ? "headphones" : "speakers");
    return 0;
}

static void out_close_pcm_locked(struct p613_stream_out *out) {
    if (out->pcm_fd >= 0) {
        ioctl(out->pcm_fd, SNDRV_PCM_IOCTL_DROP);
        close(out->pcm_fd);
        out->pcm_fd = -1;
    }
    if (out->ctl_fd >= 0) {
        int mm = out->pcm_dev_id + 1;
        route_speaker(out->ctl_fd, mm, false);
        route_headphones(out->ctl_fd, mm, false);
        close(out->ctl_fd);
        out->ctl_fd = -1;
    }
    out->standby = true;
}

/* audio_stream_out callbacks */
static uint32_t out_get_sample_rate(const struct audio_stream *s) { (void)s; return SAMPLE_RATE; }
static int out_set_sample_rate(struct audio_stream *s, uint32_t r) { (void)s; return r == SAMPLE_RATE ? 0 : -22; }
static size_t out_get_buffer_size(const struct audio_stream *s) { (void)s; return PERIOD_FRAMES * FRAME_BYTES; }
static audio_channel_mask_t out_get_channels(const struct audio_stream *s) { (void)s; return AUDIO_CHANNEL_OUT_STEREO; }
static audio_format_t out_get_format(const struct audio_stream *s) { (void)s; return AUDIO_FORMAT_PCM_16_BIT; }
static int out_set_format(struct audio_stream *s, audio_format_t f) { (void)s; (void)f; return -38; }
static int out_standby(struct audio_stream *s) {
    struct p613_stream_out *out = (struct p613_stream_out *)s;
    pthread_mutex_lock(&out->lock);
    out_close_pcm_locked(out);
    pthread_mutex_unlock(&out->lock);
    return 0;
}
static int out_dump(const struct audio_stream *s, int fd) { (void)s; (void)fd; return 0; }
static audio_devices_t out_get_device(const struct audio_stream *s) {
    return ((const struct p613_stream_out *)s)->devices;
}
static int out_set_device(struct audio_stream *s, audio_devices_t d) {
    struct p613_stream_out *out = (struct p613_stream_out *)s;
    pthread_mutex_lock(&out->lock);
    out->devices = d;
    pthread_mutex_unlock(&out->lock);
    return 0;
}
static int out_set_parameters(struct audio_stream *s, const char *kv) {
    struct p613_stream_out *out = (struct p613_stream_out *)s;
    if (!kv) return 0;
    const char *p = strstr(kv, "routing=");
    if (p) {
        unsigned long dev = strtoul(p + 8, NULL, 10);
        if (dev != 0) {
            pthread_mutex_lock(&out->lock);
            bool need_reopen = !out->standby && (use_headphones((audio_devices_t)dev) != out->routed_hph);
            out->devices = (audio_devices_t)dev;
            if (need_reopen) out_close_pcm_locked(out);
            pthread_mutex_unlock(&out->lock);
        }
    }
    return 0;
}
static char *out_get_parameters(const struct audio_stream *s, const char *keys) {
    (void)s; (void)keys;
    return strdup("");
}
static int out_add_effect(const struct audio_stream *s, effect_handle_t e) { (void)s; (void)e; return 0; }
static int out_del_effect(const struct audio_stream *s, effect_handle_t e) { (void)s; (void)e; return 0; }
static uint32_t out_get_latency(const struct audio_stream_out *s) {
    (void)s;
    return (PERIOD_FRAMES * PERIOD_COUNT * 1000) / SAMPLE_RATE;
}
static int out_set_volume(struct audio_stream_out *s, float l, float r) {
    (void)s; (void)l; (void)r;
    return -38; /* Let AudioFlinger software mixer handle volume */
}
static ssize_t out_write(struct audio_stream_out *s, const void *buffer, size_t bytes) {
    struct p613_stream_out *out = (struct p613_stream_out *)s;
    size_t frames = bytes / FRAME_BYTES;
    if (frames == 0) return 0;

    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        if (out_open_pcm_locked(out) < 0) {
            pthread_mutex_unlock(&out->lock);
            usleep((useconds_t)((frames * 1000000ULL) / SAMPLE_RATE));
            out->written_frames += frames;
            return (ssize_t)bytes;
        }
    }

    const uint8_t *ptr = (const uint8_t *)buffer;
    size_t rem = frames;
    while (rem > 0) {
        struct snd_xferi x = { .buf = (void *)ptr, .frames = rem };
        if (ioctl(out->pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            int err = B_ERRNO;
            if (err == 32 /* EPIPE */ || err == 77 /* EBADFD */) {
                if (ioctl(out->pcm_fd, SNDRV_PCM_IOCTL_PREPARE) == 0) continue;
            }
            LOGE("WRITEI_FRAMES err=%d, resetting PCM", err);
            out_close_pcm_locked(out);
            usleep((useconds_t)((rem * 1000000ULL) / SAMPLE_RATE));
            break;
        }
        if (x.result <= 0) break;
        ptr += (size_t)x.result * FRAME_BYTES;
        rem -= (size_t)x.result;
    }
    out->written_frames += frames;
    pthread_mutex_unlock(&out->lock);
    return (ssize_t)bytes;
}
static int out_get_render_position(const struct audio_stream_out *s, uint32_t *dsp_frames) {
    const struct p613_stream_out *out = (const struct p613_stream_out *)s;
    if (dsp_frames) *dsp_frames = (uint32_t)out->written_frames;
    return 0;
}
static int out_get_presentation_position(const struct audio_stream_out *s, uint64_t *frames, struct timespec *ts) {
    const struct p613_stream_out *out = (const struct p613_stream_out *)s;
    if (frames) *frames = out->written_frames;
    if (ts) clock_gettime(CLOCK_MONOTONIC, ts);
    return 0;
}

/* Minimal clean audio_stream_in (returns silent paced PCM so recording queries succeed) */
static uint32_t in_get_sample_rate(const struct audio_stream *s) { return ((const struct p613_stream_in *)s)->rate; }
static int in_set_sample_rate(struct audio_stream *s, uint32_t r) { ((struct p613_stream_in *)s)->rate = r; return 0; }
static size_t in_get_buffer_size(const struct audio_stream *s) {
    const struct p613_stream_in *in = (const struct p613_stream_in *)s;
    size_t ch = (in->ch_mask == AUDIO_CHANNEL_IN_MONO) ? 1 : 2;
    return (in->rate / 50) * ch * 2;
}
static audio_channel_mask_t in_get_channels(const struct audio_stream *s) { return ((const struct p613_stream_in *)s)->ch_mask; }
static audio_format_t in_get_format(const struct audio_stream *s) { (void)s; return AUDIO_FORMAT_PCM_16_BIT; }
static int in_set_format(struct audio_stream *s, audio_format_t f) { (void)s; (void)f; return -38; }
static int in_standby(struct audio_stream *s) { ((struct p613_stream_in *)s)->standby = true; return 0; }
static int in_dump(const struct audio_stream *s, int fd) { (void)s; (void)fd; return 0; }
static audio_devices_t in_get_device(const struct audio_stream *s) { return ((const struct p613_stream_in *)s)->device; }
static int in_set_device(struct audio_stream *s, audio_devices_t d) { ((struct p613_stream_in *)s)->device = d; return 0; }
static int in_set_parameters(struct audio_stream *s, const char *kv) { (void)s; (void)kv; return 0; }
static char *in_get_parameters(const struct audio_stream *s, const char *k) { (void)s; (void)k; return strdup(""); }
static int in_set_gain(struct audio_stream_in *s, float g) { (void)s; (void)g; return 0; }
static ssize_t in_read(struct audio_stream_in *s, void *buf, size_t bytes) {
    struct p613_stream_in *in = (struct p613_stream_in *)s;
    size_t ch = (in->ch_mask == AUDIO_CHANNEL_IN_MONO) ? 1 : 2;
    size_t frames = bytes / (ch * 2);
    memset(buf, 0, bytes);
    usleep((useconds_t)((frames * 1000000ULL) / (in->rate ? in->rate : 48000)));
    in->frames_read += (int64_t)frames;
    return (ssize_t)bytes;
}
static uint32_t in_get_frames_lost(struct audio_stream_in *s) { (void)s; return 0; }
static int in_get_capture_position(const struct audio_stream_in *s, int64_t *frames, int64_t *time_ns) {
    const struct p613_stream_in *in = (const struct p613_stream_in *)s;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (frames) *frames = in->frames_read;
    if (time_ns) *time_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 0;
}

/* audio_hw_device callbacks */
static int adev_init_check(const struct audio_hw_device *d) { (void)d; return 0; }
static int adev_set_voice_volume(struct audio_hw_device *d, float v) { (void)d; (void)v; return 0; }
static int adev_set_master_volume(struct audio_hw_device *d, float v) { (void)d; (void)v; return -38; }
static int adev_set_master_mute(struct audio_hw_device *d, bool m) { (void)d; (void)m; return -38; }
static int adev_set_mode(struct audio_hw_device *d, audio_mode_t m) {
    ((struct p613_audio_dev *)d)->mode = m;
    return 0;
}
static int adev_set_mic_mute(struct audio_hw_device *d, bool st) {
    ((struct p613_audio_dev *)d)->mic_mute = st;
    return 0;
}
static int adev_get_mic_mute(const struct audio_hw_device *d, bool *st) {
    if (st) *st = ((const struct p613_audio_dev *)d)->mic_mute;
    return 0;
}
static int adev_set_parameters(struct audio_hw_device *d, const char *kv) { (void)d; (void)kv; return 0; }
static char *adev_get_parameters(const struct audio_hw_device *d, const char *k) { (void)d; (void)k; return strdup(""); }
static size_t adev_get_input_buffer_size(const struct audio_hw_device *d, const struct audio_config *cfg) {
    (void)d;
    uint32_t rate = (cfg && cfg->sample_rate) ? cfg->sample_rate : 48000;
    size_t ch = (cfg && cfg->channel_mask == AUDIO_CHANNEL_IN_MONO) ? 1 : 2;
    return (rate / 50) * ch * 2;
}
static int adev_dump(const struct audio_hw_device *d, int fd) { (void)d; (void)fd; return 0; }

static int adev_open_output_stream(struct audio_hw_device *dev, audio_io_handle_t handle,
                                   audio_devices_t devices, audio_output_flags_t flags,
                                   struct audio_config *config, struct audio_stream_out **stream_out,
                                   const char *address) {
    (void)handle; (void)address;
    struct p613_audio_dev *adev = (struct p613_audio_dev *)dev;
    struct p613_stream_out *out = (struct p613_stream_out *)calloc(1, sizeof(*out));
    if (!out) return -12;

    pthread_mutex_init(&out->lock, NULL);
    out->adev = adev;
    out->devices = devices ? devices : AUDIO_DEVICE_OUT_SPEAKER;
    out->pcm_fd = -1;
    out->ctl_fd = -1;
    out->standby = true;

    pthread_mutex_lock(&adev->lock);
    /* MultiMedia1 (0) for primary, MultiMedia2 (1) for fast/secondary */
    out->pcm_dev_id = (flags & AUDIO_OUTPUT_FLAG_FAST) ? 1 : 0;
    if (adev->active_out_count > 0 && out->pcm_dev_id == 0) out->pcm_dev_id = 1;
    adev->active_out_count++;
    for (int i = 0; i < 4; i++) {
        if (!adev->out_streams[i]) { adev->out_streams[i] = out; break; }
    }
    pthread_mutex_unlock(&adev->lock);

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.get_device = out_get_device;
    out->stream.common.set_device = out_set_device;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_effect;
    out->stream.common.remove_audio_effect = out_del_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_presentation_position = out_get_presentation_position;

    if (config) {
        config->sample_rate = SAMPLE_RATE;
        config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
        config->format = AUDIO_FORMAT_PCM_16_BIT;
    }
    *stream_out = &out->stream;
    LOGI("open_output_stream dev=0x%x flags=0x%x -> pcmC0D%dp", out->devices, flags, out->pcm_dev_id);
    return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev, struct audio_stream_out *stream) {
    struct p613_audio_dev *adev = (struct p613_audio_dev *)dev;
    struct p613_stream_out *out = (struct p613_stream_out *)stream;
    if (!out) return;
    out_standby(&out->stream.common);
    pthread_mutex_lock(&adev->lock);
    for (int i = 0; i < 4; i++) {
        if (adev->out_streams[i] == out) adev->out_streams[i] = NULL;
    }
    if (adev->active_out_count > 0) adev->active_out_count--;
    pthread_mutex_unlock(&adev->lock);
    pthread_mutex_destroy(&out->lock);
    free(out);
}

static int adev_open_input_stream(struct audio_hw_device *dev, audio_io_handle_t handle,
                                  audio_devices_t devices, struct audio_config *config,
                                  struct audio_stream_in **stream_in, audio_input_flags_t flags,
                                  const char *address, audio_source_t source) {
    (void)handle; (void)flags; (void)address; (void)source;
    struct p613_stream_in *in = (struct p613_stream_in *)calloc(1, sizeof(*in));
    if (!in) return -12;
    pthread_mutex_init(&in->lock, NULL);
    in->adev = (struct p613_audio_dev *)dev;
    in->device = devices;
    in->rate = (config && config->sample_rate) ? config->sample_rate : 48000;
    in->ch_mask = (config && config->channel_mask) ? config->channel_mask : AUDIO_CHANNEL_IN_MONO;
    in->standby = true;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.get_device = in_get_device;
    in->stream.common.set_device = in_set_device;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = out_add_effect;
    in->stream.common.remove_audio_effect = out_del_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_frames_lost;
    in->stream.get_capture_position = in_get_capture_position;

    if (config) {
        config->sample_rate = in->rate;
        config->channel_mask = in->ch_mask;
        config->format = AUDIO_FORMAT_PCM_16_BIT;
    }
    *stream_in = &in->stream;
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev, struct audio_stream_in *stream) {
    (void)dev;
    struct p613_stream_in *in = (struct p613_stream_in *)stream;
    if (!in) return;
    pthread_mutex_destroy(&in->lock);
    free(in);
}

static int adev_create_audio_patch(struct audio_hw_device *dev,
                                   unsigned int num_sources,
                                   const struct audio_port_config *sources,
                                   unsigned int num_sinks,
                                   const struct audio_port_config *sinks,
                                   audio_patch_handle_t *handle) {
    (void)num_sources; (void)sources;
    struct p613_audio_dev *adev = (struct p613_audio_dev *)dev;
    pthread_mutex_lock(&adev->lock);
    if (handle) *handle = ++adev->next_patch_handle;
    if (num_sinks > 0 && sinks && sinks[0].type == AUDIO_PORT_TYPE_DEVICE) {
        audio_devices_t new_dev = sinks[0].ext.device.type;
        for (int i = 0; i < 4; i++) {
            struct p613_stream_out *out = adev->out_streams[i];
            if (!out) continue;
            pthread_mutex_lock(&out->lock);
            bool need_reopen = !out->standby && (use_headphones(new_dev) != out->routed_hph);
            out->devices = new_dev;
            if (need_reopen) out_close_pcm_locked(out);
            pthread_mutex_unlock(&out->lock);
        }
        LOGI("create_audio_patch handle=%d -> device=0x%x", handle ? *handle : 0, new_dev);
    }
    pthread_mutex_unlock(&adev->lock);
    return 0;
}

static int adev_release_audio_patch(struct audio_hw_device *dev, audio_patch_handle_t handle) {
    (void)dev; (void)handle;
    return 0;
}

static int adev_get_audio_port(struct audio_hw_device *dev, struct audio_port *port) {
    (void)dev; (void)port;
    return -38;
}

static int adev_set_audio_port_config(struct audio_hw_device *dev, const struct audio_port_config *config) {
    (void)dev; (void)config;
    return 0;
}

static int adev_add_device_effect(struct audio_hw_device *dev, audio_port_handle_t device, effect_handle_t effect) {
    (void)dev; (void)device; (void)effect;
    return -38;
}

static int adev_remove_device_effect(struct audio_hw_device *dev, audio_port_handle_t device, effect_handle_t effect) {
    (void)dev; (void)device; (void)effect;
    return -38;
}

static int adev_get_microphones(const struct audio_hw_device *dev,
                                struct audio_microphone_characteristic_t *mic_array,
                                size_t *mic_count) {
    (void)dev; (void)mic_array;
    if (mic_count) *mic_count = 0;
    return -38;
}

static int adev_close(hw_device_t *device) {
    struct p613_audio_dev *adev = (struct p613_audio_dev *)device;
    pthread_mutex_destroy(&adev->lock);
    free(adev);
    return 0;
}

static int adev_open(const hw_module_t *module, const char *name, hw_device_t **device) {
    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) return -22;
    struct p613_audio_dev *adev = (struct p613_audio_dev *)calloc(1, sizeof(*adev));
    if (!adev) return -12;

    pthread_mutex_init(&adev->lock, NULL);
    adev->dev.common.tag = HARDWARE_DEVICE_TAG;
    adev->dev.common.version = AUDIO_DEVICE_API_VERSION_3_0;
    adev->dev.common.module = (struct hw_module_t *)module;
    adev->dev.common.close = adev_close;

    adev->dev.init_check = adev_init_check;
    adev->dev.set_voice_volume = adev_set_voice_volume;
    adev->dev.set_master_volume = adev_set_master_volume;
    adev->dev.set_master_mute = adev_set_master_mute;
    adev->dev.set_mode = adev_set_mode;
    adev->dev.set_mic_mute = adev_set_mic_mute;
    adev->dev.get_mic_mute = adev_get_mic_mute;
    adev->dev.set_parameters = adev_set_parameters;
    adev->dev.get_parameters = adev_get_parameters;
    adev->dev.get_input_buffer_size = adev_get_input_buffer_size;
    adev->dev.open_output_stream = adev_open_output_stream;
    adev->dev.close_output_stream = adev_close_output_stream;
    adev->dev.open_input_stream = adev_open_input_stream;
    adev->dev.close_input_stream = adev_close_input_stream;
    adev->dev.get_microphones = adev_get_microphones;
    adev->dev.dump = adev_dump;
    adev->dev.create_audio_patch = adev_create_audio_patch;
    adev->dev.release_audio_patch = adev_release_audio_patch;
    adev->dev.get_audio_port = adev_get_audio_port;
    adev->dev.set_audio_port_config = adev_set_audio_port_config;
    adev->dev.add_device_effect = adev_add_device_effect;
    adev->dev.remove_device_effect = adev_remove_device_effect;

    *device = &adev->dev.common;
    LOGI("Mainline SM-P613 ALSA audio HAL opened (v3.0)");
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "SM-P613 Mainline ALSA Audio HAL",
        .author = "p613_kernel_port",
        .methods = &hal_module_methods,
    },
};
