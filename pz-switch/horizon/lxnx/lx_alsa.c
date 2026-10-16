// Just enough of ALSA's libasound for FMOD's ALSA output (the x86 FMOD reaches it through box64's libasound wrapper):
// one playback device, 48 kHz stereo, played through libnx audout. Float output from FMOD is converted to s16.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "lx.h"

enum { FMT_S16_LE = 2, FMT_FLOAT_LE = 14, STATE_PREPARED = 2, STATE_RUNNING = 3 };
#define NBUF 4
#define BUF_FRAMES 1024
#define BUF_BYTES (BUF_FRAMES * 2 * sizeof(s16))   // 4096: audout wants 0x1000-aligned sizes

typedef struct { int format, channels, rate; unsigned long period, buffer; } HwParams;
typedef struct { unsigned long avail_min, start_threshold; } SwParams;
typedef struct {
    HwParams hw;
    AudioOutBuffer bufs[NBUF];
    s16 *mem[NBUF];
    int queued[NBUF];
    int fill;           // index of the buffer being filled
    size_t filled;      // frames in it
    int started, nonblock;
} Pcm;

static int audio_up;

static int open_pcm(Pcm **out, const char *name, int stream, int mode) {
    (void)name;
    if (stream != 0) return -LX_ENOENT;   // playback only (no capture device)
    FILE *off = fopen(LX_ROOT "/noaudio", "r");   // debug switch: no output device at all
    if (off) {
        fclose(off); lx_logf("alsa: disabled by noaudio\n");
        return -LX_ENOENT;
    }
    if (!audio_up) {
        Result rc = audoutInitialize();
        if (R_FAILED(rc)) { lx_logf("alsa: audoutInitialize %#x\n", rc); return -LX_EIO; }
        rc = audoutStartAudioOut();
        if (R_FAILED(rc)) { lx_logf("alsa: audoutStartAudioOut %#x\n", rc); return -LX_EIO; }
        audio_up = 1;
        lx_logf("alsa: audout up, %u Hz, %u channels\n", audoutGetSampleRate(), audoutGetChannelCount());
    }
    Pcm *p = calloc(1, sizeof *p);
    for (int i = 0; i < NBUF; i++) {
        p->mem[i] = memalign(0x1000, BUF_BYTES);
        memset(p->mem[i], 0, BUF_BYTES);
        p->bufs[i] = (AudioOutBuffer){ .buffer = p->mem[i], .buffer_size = BUF_BYTES, .data_size = BUF_BYTES };
    }
    p->hw = (HwParams){ FMT_S16_LE, 2, 48000, BUF_FRAMES, BUF_FRAMES * NBUF };
    p->nonblock = mode & 1;
    *out = p;
    return 0;
}

static int close_pcm(Pcm *p) {
    // ponytail: the audout buffers may still be queued; leak them instead of freeing memory the service reads
    free(p);
    return 0;
}

unsigned long lx_audio_appended, lx_audio_released, lx_audio_dropped;   // watchdog line

static void reclaim(Pcm *p, int wait) {
    AudioOutBuffer *rel = NULL; u32 n = 0;
    if (wait) audoutWaitPlayFinish(&rel, &n, 100000000ull);
    else audoutGetReleasedAudioOutBuffer(&rel, &n);
    while (rel && n) {
        for (int i = 0; i < NBUF; i++) if (&p->bufs[i] == rel) { p->queued[i] = 0; lx_audio_released++; }
        rel = NULL; n = 0;
        audoutGetReleasedAudioOutBuffer(&rel, &n);
    }
}

static long writei(Pcm *p, const void *buf, unsigned long frames) {
    unsigned long done = 0;
    u64 stall = 0;
    while (done < frames) {
        if (p->queued[p->fill]) {   // wait for the oldest buffer to come back
            reclaim(p, !p->nonblock);
            if (p->queued[p->fill]) {
                if (p->nonblock) return done ? (long)done : -LX_EAGAIN;
                // never block forever: callers (openal-soft's mixer) hold locks the game waits on; drop instead
                if (!stall) stall = armGetSystemTick();
                else if (armTicksToNs(armGetSystemTick() - stall) > 500000000ull) {
                    static int told;
                    if (!told++) lx_logf("alsa: audout returned no buffer for 0.5 s, dropping audio (appended %lu, released %lu)\n",
                                         lx_audio_appended, lx_audio_released);
                    lx_audio_dropped += frames - done;
                    return frames;
                }
                continue;
            }
        }
        unsigned long n = BUF_FRAMES - p->filled;
        if (n > frames - done) n = frames - done;
        s16 *dst = p->mem[p->fill] + p->filled * 2;
        if (p->hw.format == FMT_FLOAT_LE) {
            const float *src = (const float *)buf + done * p->hw.channels;
            for (unsigned long i = 0; i < n; i++)
                for (int c = 0; c < 2; c++) {
                    float v = src[i * p->hw.channels + (c < p->hw.channels ? c : 0)];
                    dst[i * 2 + c] = v >= 1.f ? 32767 : v <= -1.f ? -32768 : (s16)(v * 32767.f);
                }
        } else {
            const s16 *src = (const s16 *)buf + done * p->hw.channels;
            for (unsigned long i = 0; i < n; i++)
                for (int c = 0; c < 2; c++) dst[i * 2 + c] = src[i * p->hw.channels + (c < p->hw.channels ? c : 0)];
        }
        p->filled += n; done += n;
        if (p->filled == BUF_FRAMES) {
            armDCacheFlush(p->mem[p->fill], BUF_BYTES);
            Result rc = audoutAppendAudioOutBuffer(&p->bufs[p->fill]);
            static int bad;
            if (R_FAILED(rc) && bad++ < 5) lx_logf("alsa: audoutAppendAudioOutBuffer %#x\n", rc);
            lx_audio_appended++;
            p->queued[p->fill] = 1;
            p->fill = (p->fill + 1) % NBUF;
            p->filled = 0;
            p->started = 1;
        }
    }
    return done;
}

static long avail_update(Pcm *p) {
    reclaim(p, 0);
    long free_frames = 0;
    for (int i = 0; i < NBUF; i++) if (!p->queued[i]) free_frames += BUF_FRAMES;
    return free_frames - (long)p->filled;
}

static int hw_any(Pcm *p, HwParams *h) { (void)p; *h = (HwParams){ FMT_S16_LE, 2, 48000, BUF_FRAMES, BUF_FRAMES * NBUF }; return 0; }
static int hw_access(Pcm *p, HwParams *h, int a) { (void)p; (void)h; return a == 3 ? 0 : -LX_EINVAL; }   // RW_INTERLEAVED
static int hw_format(Pcm *p, HwParams *h, int f) { (void)p; if (f != FMT_S16_LE && f != FMT_FLOAT_LE) return -LX_EINVAL; h->format = f; return 0; }
static int hw_channels(Pcm *p, HwParams *h, unsigned c) { (void)p; if (c < 1 || c > 8) return -LX_EINVAL; h->channels = c; return 0; }
static int hw_rate(Pcm *p, HwParams *h, unsigned r, int dir) { (void)p; (void)dir; if (r != 48000) return -LX_EINVAL; h->rate = r; return 0; }
static int hw_test_rate(Pcm *p, HwParams *h, unsigned r, int dir) { (void)p; (void)h; (void)dir; return r == 48000 ? 0 : -LX_EINVAL; }
static int hw_period(Pcm *p, HwParams *h, unsigned long *v, int *dir) { (void)p; (void)dir; *v = BUF_FRAMES; h->period = *v; return 0; }
static int hw_buffer(Pcm *p, HwParams *h, unsigned long *v) { (void)p; *v = BUF_FRAMES * NBUF; h->buffer = *v; return 0; }
static int hw_apply(Pcm *p, HwParams *h) {
    p->hw = *h;
    lx_logf("alsa: playback %d ch, format %d, %d Hz\n", h->channels, h->format, h->rate);
    return 0;
}
static size_t hw_sizeof(void) { return sizeof(HwParams); }
static size_t sw_sizeof(void) { return sizeof(SwParams); }
static int sw_current(Pcm *p, SwParams *s) { (void)p; s->avail_min = BUF_FRAMES; s->start_threshold = BUF_FRAMES; return 0; }
static int sw_set(Pcm *p, SwParams *s, unsigned long v) { (void)p; (void)s; (void)v; return 0; }
static int sw_apply(Pcm *p, SwParams *s) { (void)p; (void)s; return 0; }
static int pcm_ok(Pcm *p) { (void)p; return 0; }
static int pcm_nonblock(Pcm *p, int nb) { p->nonblock = nb; return 0; }
static int pcm_state(Pcm *p) { return p->started ? STATE_RUNNING : STATE_PREPARED; }
static long pcm_readi(void) { return -LX_EIO; }
static const char *strerror_(int e) { (void)e; return "audout error"; }
static const char *version(void) { return "1.2.10"; }

// one device: "default"
static char *hints[2] = { "hint", NULL };
static int name_hint(int card, const char *iface, void ***out) { (void)card; (void)iface; *out = (void **)hints; return 0; }
// static strings: lx_dl.c copies them onto the musl heap, where the caller frees them
static const char *name_get_hint(const void *h, const char *id) {
    (void)h;
    if (!strcmp(id, "NAME")) return "default";
    if (!strcmp(id, "DESC")) return "Nintendo Switch";
    if (!strcmp(id, "IOID")) return "Output";
    return NULL;
}
static int name_free_hint(void **h) { (void)h; return 0; }

static const struct { const char *name; void *fn; } table[] = {
    { "snd_pcm_open", open_pcm }, { "snd_pcm_close", close_pcm }, { "snd_pcm_writei", writei },
    { "snd_pcm_avail_update", avail_update }, { "snd_pcm_hw_params_any", hw_any },
    { "snd_pcm_hw_params_set_access", hw_access }, { "snd_pcm_hw_params_set_format", hw_format },
    { "snd_pcm_hw_params_set_channels", hw_channels }, { "snd_pcm_hw_params_set_rate", hw_rate },
    { "snd_pcm_hw_params_test_rate", hw_test_rate }, { "snd_pcm_hw_params_set_period_size_near", hw_period },
    { "snd_pcm_hw_params_set_buffer_size_near", hw_buffer }, { "snd_pcm_hw_params", hw_apply },
    { "snd_pcm_hw_params_sizeof", hw_sizeof }, { "snd_pcm_sw_params_sizeof", sw_sizeof },
    { "snd_pcm_sw_params_current", sw_current }, { "snd_pcm_sw_params_set_avail_min", sw_set },
    { "snd_pcm_sw_params_set_start_threshold", sw_set }, { "snd_pcm_sw_params", sw_apply },
    { "snd_pcm_prepare", pcm_ok }, { "snd_pcm_start", pcm_ok }, { "snd_pcm_drop", pcm_ok }, { "snd_pcm_resume", pcm_ok },
    { "snd_pcm_nonblock", pcm_nonblock }, { "snd_pcm_state", pcm_state }, { "snd_pcm_readi", pcm_readi },
    { "snd_strerror", strerror_ }, { "snd_asoundlib_version", version }, { "snd_device_name_hint", name_hint },
    { "snd_device_name_get_hint", name_get_hint }, { "snd_device_name_free_hint", name_free_hint },
};

// OSS /dev/dsp for openal-soft's OSS backend (lx_fs.c): the same playback path, 48 kHz s16, 1-2 channels
void *lx_dsp_open(void) { Pcm *p; return open_pcm(&p, "dsp", 0, 0) ? NULL : p; }
void lx_dsp_channels(void *p, int channels) { ((Pcm *)p)->hw.channels = channels; }
long lx_dsp_write(void *p, const void *buf, size_t bytes) {
    Pcm *q = p;
    size_t fs = 2 * q->hw.channels;
    long f = writei(q, buf, bytes / fs);   // blocks while every audout buffer is queued: that paces the mixer
    return f < 0 ? f : f * (long)fs;
}
void lx_dsp_close(void *p) { close_pcm(p); }
// select() on /dev/dsp: sleep until a buffer is free. openal-soft's mixer holds its device lock except around
// select(); returning at once there starves every other thread wanting the lock (seen: alSourceStop never returned).
int lx_dsp_wait(void *p, u64 timeout_ns) {
    Pcm *q = p;
    u64 t0 = armGetSystemTick();
    while (q->queued[q->fill] && armTicksToNs(armGetSystemTick() - t0) < timeout_ns) reclaim(q, 1);
    return !q->queued[q->fill];
}

void *lx_alsa_sym(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++)
        if (!strcmp(table[i].name, name)) return table[i].fn;
    return NULL;
}
