/*
 * Native DirectSound for Black (Street 1 hle_dsound_native.c, extended with streams).
 *
 * The XDK DirectSound library programs the MCPX voice processor through its registers (0xFE800000); here the
 * entry points Black and its XMV/WMA libraries call are implemented directly, with a voice mixer, a speaker fold
 * and a 48 kHz output thread feeding the host (XAudio2). Neither the library's voice layer nor any model of the
 * APU runs.
 *
 *   buffers: handles with a magic, called through the IDirectSoundBuffer_* wrappers (stdcall);
 *   streams: XMediaObjects, called through their vtable -- the native object keeps the library's interface
 *            vtable (0x2838A0), whose methods (CDirectSoundStream_*) are these functions. Packets play in order;
 *            completion writes status/size, signals the packet's event from the output thread, and runs the
 *            stream callback on the next game-thread call (DirectSoundDoWork, Process, GetStatus).
 *
 * Constants read from the library: GetInfo flags 5 (fixed sample size, async input); stream status READY 1,
 * PLAYING 0x10000, PAUSED 0x20000, STARVED 0x40000; Process refuses with 0x88780032 when not READY. Gains as in
 * Street 1 (library 0x269D3A there): attenuation in mB = mixbin volume + voice volume - headroom (default 600).
 * 3D positioning, I3DL2 and the effects DSP are accepted and not processed.
 */
#include "black_win32.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "black_native.h"

#define NUM_MIXBINS   32
#define FRAME         32
#define MAX_BUFS      512
#define MAX_STREAMS   64
#define MAX_PACKETS   64
#define BUF_MAGIC     0x4253534Eu        /* 'NSSB' */
#define STREAM_MAGIC  0x5453534Eu        /* 'NSST' */
#define DS_MAGIC      0x4453534Eu
#define STREAM_VTABLE 0x002838A0u        /* the library's XMediaObject interface vtable for streams */

#define STATUS_SUCCESS 0u
#define STATUS_PENDING 0x8000000Au       /* XMEDIAPACKET_STATUS_PENDING (E_PENDING) */
#define STATUS_FLUSHED 0x80004004u       /* XMEDIAPACKET_STATUS_FLUSHED (E_ABORT) */

extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
extern HANDLE xbox_ResolveHandle(uint32_t token);
extern int xa2_init(void);
extern int xa2_submit_samples(const int16_t *samples, int num_samples);

typedef struct { float f, q, qnrm, h, b, l, p, n; } Svf;

/* What a voice plays from: format and the per-voice mix. */
typedef struct {
    int adpcm, channels, bits, align, rate, pitch;
    int nbins;
    uint8_t bin[8];
    int32_t binvol[NUM_MIXBINS];
    int32_t volume, headroom;
    int fmode;
    int16_t fc0;
    uint16_t fc1;
    Svf svf[6];
    int cached_block;
    uint32_t cached_data;
    int16_t dec[65 * 6];
} Voice;

typedef struct {
    uint32_t handle;
    int used, refs;
    Voice v;
    uint32_t data, bytes;
    uint32_t loop_start, loop_len;
    uint32_t play_start, play_len;
    double pos;                       /* frames */
    int playing, looping, paused;
} DsBuf;

typedef struct {
    uint32_t buffer, size, completed_ptr, status_ptr, event_or_ctx;
} Packet;

typedef struct {
    uint32_t handle;
    int used, refs;
    Voice v;
    uint32_t callback, context, max_packets;
    Packet q[MAX_PACKETS];
    int head, count;                  /* queued packets (head = playing) */
    double pos;                       /* frames into the head packet */
    int paused, starved, discontinuity;
} DsStream;

typedef struct { uint32_t callback, context, packet_ctx, status; } Done;

static CRITICAL_SECTION s_lock;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static DsBuf s_buf[MAX_BUFS];
static DsStream s_stream[MAX_STREAMS];
static Done s_done[256];
static int s_done_count;
static uint32_t s_ds_handle;
static int s_ds_refs;
static int s_headroom[NUM_MIXBINS];
static int s_started;
static uint64_t s_mix_frames, s_packets_done;

static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_lock);
    for (int i = 0; i < NUM_MIXBINS; i++)
        s_headroom[i] = 0;
    return TRUE;
}

static void lock(void) { InitOnceExecuteOnce(&s_once, init_once, NULL, NULL); EnterCriticalSection(&s_lock); }
static void unlock(void) { LeaveCriticalSection(&s_lock); }

/* ------------------------------------------------------------- voice */

static void voice_format(Voice *v, uint32_t wfx)
{
    v->adpcm = MEM16(wfx) == 0x69;
    v->channels = MEM16(wfx + 2) ? MEM16(wfx + 2) : 1;
    if (v->channels > 6)
        v->channels = 6;
    v->rate = (int)MEM32(wfx + 4);
    v->align = MEM16(wfx + 12);
    v->bits = MEM16(wfx + 14);
    v->pitch = v->rate && v->rate != 48000 ? (int)lrint(4096.0 * log2((double)v->rate / 48000.0)) : 0;
    v->cached_block = -1;
}

static uint32_t frames_of_bytes(const Voice *v, uint32_t bytes)
{
    if (v->adpcm)
        return bytes / (36u * (uint32_t)v->channels) * 64u;
    return v->align ? bytes / (uint32_t)v->align : 0;
}

static uint32_t bytes_of_frames(const Voice *v, uint32_t frames)
{
    if (v->adpcm)
        return frames / 64u * 36u * (uint32_t)v->channels;
    return frames * (uint32_t)v->align;
}

static void voice_mixbins(Voice *v, uint32_t list)
{
    uint32_t cnt, p, k;
    if (!list || !(cnt = MEM32(list)) || !(p = MEM32(list + 4))) {
        v->nbins = v->channels < 8 ? v->channels : 8;     /* channel i -> speaker bin i, 0 dB */
        for (k = 0; k < (uint32_t)v->nbins; k++) {
            v->bin[k] = (uint8_t)k;
            v->binvol[k] = 0;
        }
        return;
    }
    if (cnt > 8)
        cnt = 8;
    v->nbins = (int)cnt;
    for (k = 0; k < cnt; k++) {
        uint32_t bin = MEM32(p + 8 * k) & (NUM_MIXBINS - 1);
        v->bin[k] = (uint8_t)bin;
        v->binvol[bin] = (int32_t)MEM32(p + 8 * k + 4);
    }
}

static void voice_mixbin_volumes(Voice *v, uint32_t list)
{
    uint32_t cnt, p, k;
    if (!list)
        return;
    cnt = MEM32(list);
    p = MEM32(list + 4);
    for (k = 0; k < cnt && p; k++)
        v->binvol[MEM32(p + 8 * k) & (NUM_MIXBINS - 1)] = (int32_t)MEM32(p + 8 * k + 4);
}

static void voice_init(Voice *v, uint32_t wfx, uint32_t mixbins)
{
    memset(v, 0, sizeof *v);
    v->headroom = 600;
    if (wfx)
        voice_format(v, wfx);
    else
        v->channels = 1, v->align = 2, v->bits = 16, v->rate = 48000, v->cached_block = -1;
    voice_mixbins(v, mixbins);
}

/* Attenuation in mB -> linear gain, saturating at the voice processor's 64 dB. */
static float gain_of(int32_t mb)
{
    if (mb > 0)
        mb = 0;
    if (mb <= -6400)
        return 0.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

static void adpcm_block(int16_t *out, const uint8_t *in, int channels)
{
    static const uint16_t steps[89] = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
        34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
        157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
        724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
        3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
        15289,16818,18500,20350,22385,24623,27086,29794,32767 };
    static const int idx_tab[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    int32_t pcm[6];
    int index[6], ch, chunks, i, k;
    int16_t *o = out;
    for (ch = 0; ch < channels; ch++) {
        *o++ = (int16_t)(pcm[ch] = (int16_t)(in[0] | (in[1] << 8)));
        index[ch] = in[2] > 88 ? 88 : in[2];
        in += 4;
    }
    for (chunks = 8; chunks--;) {
        for (ch = 0; ch < channels; ch++) {
            for (i = 0; i < 4; i++) {
                for (k = 0; k < 2; k++) {
                    int nib = k ? (*in >> 4) & 0xF : *in & 0xF;
                    int step = steps[index[ch]], delta = step >> 3;
                    if (nib & 1) delta += step >> 2;
                    if (nib & 2) delta += step >> 1;
                    if (nib & 4) delta += step;
                    if (nib & 8) delta = -delta;
                    pcm[ch] += delta;
                    if (pcm[ch] > 32767) pcm[ch] = 32767;
                    if (pcm[ch] < -32768) pcm[ch] = -32768;
                    index[ch] += idx_tab[nib & 7];
                    if (index[ch] < 0) index[ch] = 0;
                    if (index[ch] > 88) index[ch] = 88;
                    o[(i * 2 + k) * channels] = (int16_t)pcm[ch];
                }
                in++;
            }
            o++;
        }
        o += channels * 7;
    }
}

/* One frame of `data` (all channels) as floats -1..1. */
static void fetch(Voice *v, uint32_t data, uint32_t frame, float *out)
{
    int c;
    if (v->adpcm) {
        uint32_t block = frame / 64u, at = frame % 64u;
        if ((int)block != v->cached_block || v->cached_data != data) {
            adpcm_block(v->dec, (const uint8_t *)XBOX_PTR(data + block * 36u * (uint32_t)v->channels), v->channels);
            v->cached_block = (int)block;
            v->cached_data = data;
        }
        for (c = 0; c < v->channels; c++)
            out[c] = (float)v->dec[at * (uint32_t)v->channels + (uint32_t)c] / 32768.0f;
        return;
    }
    for (c = 0; c < v->channels; c++) {
        uint32_t a = data + frame * (uint32_t)v->align;
        if (v->bits == 8)
            out[c] = ((float)MEM8(a + (uint32_t)c) - 128.0f) / 128.0f;
        else
            out[c] = (float)(int16_t)MEM16(a + 2u * (uint32_t)c) / 32768.0f;
    }
}

static float ftz(float x) { return fabsf(x) < 1e-30f ? 0.0f : x; }

static float svf_run(Svf *s, float in)
{
    in = s->qnrm * in;
    s->b = ftz(s->b - s->b * s->b * s->b * 0.001f);
    s->h = ftz(in - s->l - s->q * s->b);
    s->b = s->b + s->f * s->h;
    s->l = ftz(s->l + s->f * s->b);
    return s->l;
}

/* Filter and send `n` frames of a voice to its mixbins. */
static void voice_out(Voice *v, float out[FRAME][6], int n, float mixbins[NUM_MIXBINS][FRAME])
{
    int i, c, j;
    if (v->fmode == 1) {
        float fc = powf(2.0f, v->fc0 / 4096.0f), q = v->fc1 / (float)0x8000;
        if (fc < 0.003906f) fc = 0.003906f;
        if (fc > 1.0f) fc = 1.0f;
        if (q < 0.079407f) q = 0.079407f;
        if (q > 1.0f) q = 1.0f;
        for (c = 0; c < v->channels; c++) {
            v->svf[c].f = fc;
            v->svf[c].q = q;
            v->svf[c].qnrm = (float)sqrt(q / 2.0 + 0.01);
            for (i = 0; i < n; i++) {
                float s = svf_run(&v->svf[c], out[i][c]);
                out[i][c] = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
            }
        }
    }
    for (j = 0; j < v->nbins; j++) {
        int bin = v->bin[j], ch = v->channels == 1 ? 0 : j % v->channels;
        float g = gain_of(v->binvol[bin] + v->volume - v->headroom) / (float)(1 << (s_headroom[bin] & 7));
        if (g == 0.0f)
            continue;
        for (i = 0; i < n; i++)
            mixbins[bin][i] += g * out[i][ch];
    }
}

/* ------------------------------------------------------------- mixer */

static void mix_buffer(DsBuf *b, float mixbins[NUM_MIXBINS][FRAME])
{
    float out[FRAME][6], s0[6], s1[6];
    Voice *v = &b->v;
    uint32_t total = frames_of_bytes(v, b->bytes), ls, le, ps, pe;
    double step = pow(2.0, v->pitch / 4096.0);
    int i, c, n = 0;
    if (!b->data || !total) {
        b->playing = 0;
        return;
    }
    ps = b->play_len ? frames_of_bytes(v, b->play_start) : 0;
    pe = b->play_len ? ps + frames_of_bytes(v, b->play_len) : total;
    if (pe > total) pe = total;
    ls = b->loop_len ? frames_of_bytes(v, b->loop_start) : ps;
    le = b->loop_len ? ls + frames_of_bytes(v, b->loop_len) : pe;
    if (le > total) le = total;
    if (ls >= le) ls = 0;
    for (i = 0; i < FRAME; i++) {
        uint32_t i0 = (uint32_t)b->pos, i1 = i0 + 1;
        float fr = (float)(b->pos - (double)i0);
        if (b->looping) {
            if (i1 >= le) i1 = ls;
        } else if (i1 >= pe) {
            i1 = i0;
        }
        fetch(v, b->data, i0, s0);
        fetch(v, b->data, i1, s1);
        for (c = 0; c < v->channels; c++)
            out[i][c] = s0[c] + (s1[c] - s0[c]) * fr;
        n++;
        b->pos += step;
        if (b->looping) {
            while (b->pos >= (double)le)
                b->pos -= (double)(le - ls);
        } else if (b->pos >= (double)pe) {
            b->pos = (double)pe;
            b->playing = 0;
            break;
        }
    }
    voice_out(v, out, n, mixbins);
}

static void complete_head(DsStream *s, uint32_t status)
{
    Packet *p = &s->q[s->head];
    uint32_t consumed = status == STATUS_SUCCESS ? p->size
                      : bytes_of_frames(&s->v, (uint32_t)s->pos) < p->size ? bytes_of_frames(&s->v, (uint32_t)s->pos)
                      : p->size;
    if (p->completed_ptr)
        MEM32(p->completed_ptr) = consumed;
    if (p->status_ptr)
        MEM32(p->status_ptr) = status;
    if (s->callback) {
        if (s_done_count < (int)(sizeof s_done / sizeof s_done[0])) {
            Done *d = &s_done[s_done_count++];
            d->callback = s->callback;
            d->context = s->context;
            d->packet_ctx = p->event_or_ctx;
            d->status = status;
        }
    } else if (p->event_or_ctx) {
        HANDLE h = xbox_ResolveHandle(p->event_or_ctx);
        if (h)
            SetEvent(h);
    }
    s->head = (s->head + 1) % MAX_PACKETS;
    s->count--;
    s->pos = 0.0;
    s_packets_done++;
}

static void mix_stream(DsStream *s, float mixbins[NUM_MIXBINS][FRAME])
{
    float out[FRAME][6], s0[6];
    Voice *v = &s->v;
    double step = pow(2.0, v->pitch / 4096.0);
    int i, c, n = 0;
    for (i = 0; i < FRAME; i++) {
        Packet *p;
        uint32_t frames;
        while (s->count) {
            p = &s->q[s->head];
            frames = frames_of_bytes(v, p->size);
            if ((uint32_t)s->pos < frames)
                break;
            complete_head(s, STATUS_SUCCESS);
        }
        if (!s->count) {
            s->starved = 1;
            break;
        }
        s->starved = 0;
        p = &s->q[s->head];
        fetch(v, p->buffer, (uint32_t)s->pos, s0);
        for (c = 0; c < v->channels; c++)
            out[i][c] = s0[c];
        n++;
        s->pos += step;
    }
    if (s->count) {                   /* a packet finished exactly at the end of the frame */
        Packet *p = &s->q[s->head];
        if ((uint32_t)s->pos >= frames_of_bytes(v, p->size))
            complete_head(s, STATUS_SUCCESS);
    }
    if (n)
        voice_out(v, out, n, mixbins);
}

static void mix_voices(float mixbins[NUM_MIXBINS][FRAME])
{
    int i;
    lock();
    for (i = 0; i < MAX_BUFS; i++)
        if (s_buf[i].used && s_buf[i].playing && !s_buf[i].paused)
            mix_buffer(&s_buf[i], mixbins);
    for (i = 0; i < MAX_STREAMS; i++)
        if (s_stream[i].used && !s_stream[i].paused)
            mix_stream(&s_stream[i], mixbins);
    s_mix_frames++;
    unlock();
}

/* Six speaker mixbins (FL, FR, C, LFE, BL, BR) folded to stereo (ITU), LFE dropped, under a peak limiter. */
static float s_limit_gain = 1.0f;
float black_audio_volume = 1.0f;

static void fold_to_stereo(float mixbins[NUM_MIXBINS][FRAME], int16_t (*out)[2])
{
    int i;
    for (i = 0; i < FRAME; i++) {
        float c = 0.70710678f * mixbins[2][i];
        float l = (mixbins[0][i] + c + 0.70710678f * mixbins[4][i]) * black_audio_volume;
        float r = (mixbins[1][i] + c + 0.70710678f * mixbins[5][i]) * black_audio_volume;
        float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if (peak * s_limit_gain > 0.98f)
            s_limit_gain = 0.98f / peak;
        else
            s_limit_gain += (1.0f - s_limit_gain) * 0.00035f;
        l *= s_limit_gain;
        r *= s_limit_gain;
        l = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
        r = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
        out[i][0] = (int16_t)(l * 32767.0f);
        out[i][1] = (int16_t)(r * 32767.0f);
    }
}

#define OUT_FRAMES 8                  /* 256 samples, 5.33 ms per block */

static DWORD WINAPI audio_out_thread(LPVOID unused)
{
    LARGE_INTEGER f, now;
    long long period, next;
    (void)unused;
    if (!xa2_init())
        fprintf(stderr, "[BLACK-DS] host audio output unavailable; mixing silently\n");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    QueryPerformanceFrequency(&f);
    period = f.QuadPart * 256 / 48000;
    QueryPerformanceCounter(&now);
    next = now.QuadPart;
    for (;;) {
        int16_t block[OUT_FRAMES * FRAME][2];
        for (int k = 0; k < OUT_FRAMES; k++) {
            float mixbins[NUM_MIXBINS][FRAME];
            memset(mixbins, 0, sizeof mixbins);
            mix_voices(mixbins);
            fold_to_stereo(mixbins, block + k * FRAME);
        }
        xa2_submit_samples(&block[0][0], OUT_FRAMES * FRAME);
        next += period;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - next > 8 * period)
            next = now.QuadPart;
        for (;;) {
            long long left;
            QueryPerformanceCounter(&now);
            left = next - now.QuadPart;
            if (left <= 0)
                break;
            left = left * 1000 / f.QuadPart;
            Sleep(left > 1 ? (DWORD)(left - 1) : 1);
        }
    }
    return 0;
}

static void start_mixer(void)
{
    if (!s_started) {
        HANDLE th;
        s_started = 1;
        th = CreateThread(NULL, 0, audio_out_thread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
        fprintf(stderr, "[BLACK-DS] native DirectSound: mixer and host output started (no APU)\n");
    }
}

/* Stream callbacks are game code: run them on the calling game thread, outside the lock. */
static void deliver_callbacks(void)
{
    Done local[256];
    int n;
    lock();
    n = s_done_count;
    memcpy(local, s_done, sizeof(Done) * (size_t)n);
    s_done_count = 0;
    unlock();
    for (int i = 0; i < n; i++) {
        uint32_t a[3] = { local[i].context, local[i].packet_ctx, local[i].status };
        uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp, ecx = g_ecx, edx = g_edx;
        black_guest_call(local[i].callback, 3, a);
        g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp; g_ecx = ecx; g_edx = edx;
    }
}

/* ------------------------------------------------------------- objects */

static uint32_t alloc_object(uint32_t first, uint32_t magic, uint32_t index)
{
    uint32_t h = xbox_HeapAlloc(64, 16);
    if (h) {
        memset((void *)XBOX_PTR(h), 0, 64);
        MEM32(h) = first;
        MEM32(h + 4) = magic;
        MEM32(h + 8) = index;
    }
    return h;
}

static DsBuf *buf_of(uint32_t h)
{
    uint32_t i;
    if (!h || MEM32(h + 4) != BUF_MAGIC)
        return NULL;
    i = MEM32(h + 8);
    return i < MAX_BUFS && s_buf[i].used && s_buf[i].handle == h ? &s_buf[i] : NULL;
}

static DsStream *stream_of(uint32_t h)
{
    uint32_t i;
    if (!h || MEM32(h + 4) != STREAM_MAGIC)
        return NULL;
    i = MEM32(h + 8);
    return i < MAX_STREAMS && s_stream[i].used && s_stream[i].handle == h ? &s_stream[i] : NULL;
}

static uint32_t create_buffer(uint32_t desc, uint32_t out)
{
    uint32_t wfx = MEM32(desc + 12), i;
    DsBuf *b = NULL;
    for (i = 0; i < MAX_BUFS; i++)
        if (!s_buf[i].used) {
            b = &s_buf[i];
            break;
        }
    if (!b)
        return 0x8007000Eu;
    memset(b, 0, sizeof *b);
    b->handle = alloc_object(0, BUF_MAGIC, i);
    if (!b->handle)
        return 0x8007000Eu;
    b->used = 1;
    b->refs = 1;
    voice_init(&b->v, wfx, MEM32(desc + 16));
    b->bytes = MEM32(desc + 8);
    if (b->bytes) {
        b->data = xbox_HeapAlloc(b->bytes, 16);
        if (b->data)
            memset((void *)XBOX_PTR(b->data), b->v.bits == 8 ? 0x80 : 0, b->bytes);
    }
    if (out)
        MEM32(out) = b->handle;
    return 0;
}

static uint32_t create_stream(uint32_t desc, uint32_t out)
{
    uint32_t i;
    DsStream *s = NULL;
    for (i = 0; i < MAX_STREAMS; i++)
        if (!s_stream[i].used) {
            s = &s_stream[i];
            break;
        }
    if (!s)
        return 0x8007000Eu;
    memset(s, 0, sizeof *s);
    s->handle = alloc_object(STREAM_VTABLE, STREAM_MAGIC, i);
    if (!s->handle)
        return 0x8007000Eu;
    s->used = 1;
    s->refs = 1;
    s->max_packets = MEM32(desc + 4) ? MEM32(desc + 4) : 1;
    if (s->max_packets > MAX_PACKETS)
        s->max_packets = MAX_PACKETS;
    voice_init(&s->v, MEM32(desc + 8), MEM32(desc + 20));
    s->callback = MEM32(desc + 12);
    s->context = MEM32(desc + 16);
    if (out)
        MEM32(out) = s->handle;
    return 0;
}

static void flush_stream(DsStream *s)
{
    while (s->count)
        complete_head(s, STATUS_FLUSHED);
    s->pos = 0.0;
}

static uint32_t stream_status(DsStream *s)
{
    uint32_t st = (uint32_t)s->count < s->max_packets ? 1u : 0u;
    if (s->paused)
        st |= 0x20000u;
    else if (s->count)
        st |= 0x10000u;
    else if (s->starved)
        st |= 0x40000u;
    return st;
}

/* ------------------------------------------------------------- API */

#define ARG(i) black_arg(i)

/* DirectSoundCreate(guid, ppDirectSound, unused) */
void sub_0021B4F6(void)
{
    uint32_t out = ARG(1);
    lock();
    if (!s_ds_handle)
        s_ds_handle = alloc_object(0, DS_MAGIC, 0);
    s_ds_refs++;
    unlock();
    if (out)
        MEM32(out) = s_ds_handle;
    start_mixer();
    black_ret(3, s_ds_handle ? 0 : 0x8007000Eu);
}

/* DirectSoundCreateStream(pdssd, ppStream) */
void sub_0021B53D(void)
{
    uint32_t r;
    lock();
    r = create_stream(ARG(0), ARG(1));
    unlock();
    start_mixer();
    black_ret(2, r);
}

void sub_0021828A(void) { lock(); if (s_ds_refs) s_ds_refs--; unlock(); black_ret(1, (uint32_t)s_ds_refs); }  /* IDirectSound_Release */
void sub_002182B6(void) { black_ret(0, 0); }                                  /* DirectSoundUseLightHRTF */
void sub_002195E3(void) { if (ARG(1)) MEM32(ARG(1)) = 0; black_ret(2, 0); }   /* GetSpeakerConfig: stereo */

/* IDirectSound_DownloadEffectsImage(this, image, size, loc, ppImageDesc): a descriptor the title may read, all
 * effects' code/state/Y/scratch segments in one zeroed block. */
void sub_002195FF(void)
{
    static uint32_t desc;
    if (!desc) {
        uint32_t seg = xbox_HeapAlloc(0x4000, 16), k;
        desc = xbox_HeapAlloc(8 + 16 * 32, 16);
        if (desc && seg) {
            memset((void *)XBOX_PTR(seg), 0, 0x4000);
            MEM32(desc) = 16;
            MEM32(desc + 4) = 0;
            for (k = 0; k < 16; k++) {
                uint32_t m = desc + 8 + 32 * k;
                MEM32(m) = seg; MEM32(m + 4) = 0x400;
                MEM32(m + 8) = seg; MEM32(m + 12) = 0x400;
                MEM32(m + 16) = seg; MEM32(m + 20) = 0x400;
                MEM32(m + 24) = seg; MEM32(m + 28) = 0x400;
            }
        }
    }
    if (ARG(4))
        MEM32(ARG(4)) = desc;
    black_ret(5, 0);
}

/* GetEffectData(this, index, offset, data, size): effect state reads as zero. */
void sub_00219626(void) { if (ARG(3) && ARG(4)) memset((void *)XBOX_PTR(ARG(3)), 0, ARG(4)); black_ret(5, 0); }
void sub_0021964D(void) { black_ret(6, 0); }                                  /* SetEffectData */
void sub_00219677(void) { black_ret(1, 0); }                                  /* CommitEffectData */
void sub_0021968F(void) { black_ret(2, 0); }                                  /* EnableHeadphones */
void sub_002196EB(void) { black_ret(1, 0); }                                  /* SynchPlayback */
void sub_0021ACF4(void) { black_ret(1, 0); }                                  /* CommitDeferredSettings */

/* SetMixBinHeadroom(this, mask, headroom) */
void sub_002196AB(void)
{
    uint32_t mask = ARG(1);
    for (int i = 0; i < NUM_MIXBINS; i++)
        if (mask & (1u << i))
            s_headroom[i] = (int)ARG(2);
    black_ret(3, 0);
}

/* GetOutputLevels(this, levels, reset): no meters. */
void sub_002196CB(void) { if (ARG(1)) memset((void *)XBOX_PTR(ARG(1)), 0, 16 * 4); black_ret(3, 0); }

/* Listener 3D state: accepted. */
void sub_0021B210(void) { black_ret(3, 0); }   /* SetDistanceFactor */
void sub_0021B234(void) { black_ret(3, 0); }   /* SetDopplerFactor */
void sub_0021B258(void) { black_ret(8, 0); }   /* SetOrientation */
void sub_0021B2A2(void) { black_ret(5, 0); }   /* SetPosition */
void sub_0021B2D7(void) { black_ret(3, 0); }   /* SetRolloffFactor */
void sub_0021B2FB(void) { black_ret(5, 0); }   /* SetVelocity */
void sub_0021B330(void) { black_ret(3, 0); }   /* SetI3DL2Listener */

/* IDirectSound_CreateSoundBuffer(this, desc, ppBuffer, unused) */
void sub_0021B1EC(void)
{
    uint32_t r;
    lock();
    r = create_buffer(ARG(1), ARG(2));
    unlock();
    start_mixer();
    black_ret(4, r);
}

/* DirectSoundDoWork: deliver stream callbacks. */
void sub_00219877(void)
{
    deliver_callbacks();
    black_ret(0, 0);
}

/* --- buffers */

#define WITH_BUF(args, ...) do { DsBuf *b; lock(); if ((b = buf_of(ARG(0))) != NULL) { __VA_ARGS__; } unlock(); black_ret(args, 0); } while (0)

void sub_002182A0(void)                                                       /* IDirectSoundBuffer_Release */
{
    uint32_t refs = 0;
    DsBuf *b;
    lock();
    if ((b = buf_of(ARG(0))) != NULL) {
        refs = (uint32_t)--b->refs;
        if (!b->refs) {
            MEM32(b->handle + 4) = 0;
            b->used = 0;
        }
    }
    unlock();
    black_ret(1, refs);
}
void sub_00219703(void) { WITH_BUF(2, b->v.volume = (int32_t)ARG(1)); }                    /* SetVolume */
void sub_0021971F(void)                                                                    /* SetFilter */
{
    WITH_BUF(2, if (ARG(1)) { b->v.fmode = (int)MEM32(ARG(1)); b->v.fc0 = (int16_t)MEM32(ARG(1) + 8);
                              b->v.fc1 = (uint16_t)MEM32(ARG(1) + 12); });
}
void sub_0021973B(void) { WITH_BUF(2, b->v.headroom = (int32_t)ARG(1)); }                  /* SetHeadroom */
void sub_00219757(void) { WITH_BUF(2, voice_mixbin_volumes(&b->v, ARG(1))); }              /* SetMixBinVolumes */
void sub_00219773(void)                                                                    /* Play(this, r1, r2, flags) */
{
    WITH_BUF(4, if (ARG(3) & 2u) b->pos = 0.0; b->looping = (ARG(3) & 1u) != 0; b->paused = 0;
                if (!b->playing) memset(b->v.svf, 0, sizeof b->v.svf); b->playing = 1);
}
void sub_00219797(void) { WITH_BUF(1, b->playing = 0); }                                   /* Stop */
void sub_002197AF(void)                                                                    /* StopEx(this, rt lo, rt hi, flags) */
{
    WITH_BUF(4, if ((ARG(3) & 3u) == 2u) b->looping = 0; else b->playing = 0);
}
void sub_002197D3(void) { WITH_BUF(3, b->loop_start = ARG(1); b->loop_len = ARG(2)); }     /* SetLoopRegion */
void sub_002197F3(void)                                                                    /* GetStatus(this, pdw) */
{
    DsBuf *b;
    lock();
    b = buf_of(ARG(0));
    if (ARG(1))
        MEM32(ARG(1)) = !b || !b->playing ? 0u : b->paused ? 2u : (1u | (b->looping ? 4u : 0u));
    unlock();
    black_ret(2, 0);
}
void sub_0021980F(void)                                                                    /* GetCurrentPosition */
{
    WITH_BUF(3, {
        uint32_t play = bytes_of_frames(&b->v, (uint32_t)b->pos), write = play;
        if (b->playing) {
            uint32_t gap = b->v.adpcm ? 36u * (uint32_t)b->v.channels : 0u;
            if (gap < bytes_of_frames(&b->v, 32))
                gap = bytes_of_frames(&b->v, 32);
            write = play + gap;
            if (b->loop_len && play >= b->loop_start && play < b->loop_start + b->loop_len)
                write = b->loop_start + (write - b->loop_start) % b->loop_len;
            else if (b->bytes)
                write %= b->bytes;
        }
        if (ARG(1)) MEM32(ARG(1)) = play;
        if (ARG(2)) MEM32(ARG(2)) = write;
    });
}
void sub_0021982F(void) { WITH_BUF(2, b->pos = (double)frames_of_bytes(&b->v, ARG(1))); }  /* SetCurrentPosition */
void sub_0021A137(void)                                                                    /* SetFrequency */
{
    WITH_BUF(2, { uint32_t hz = ARG(1) ? ARG(1) : (uint32_t)b->v.rate;
                  b->v.pitch = hz != 48000 ? (int)lrint(4096.0 * log2((double)hz / 48000.0)) : 0; });
}
void sub_0021A153(void) { black_ret(2, 0); }                                               /* SetOutputBuffer */
void sub_0021A16F(void) { WITH_BUF(2, voice_mixbins(&b->v, ARG(1))); }                     /* SetMixBins */
void sub_0021A18B(void) { black_ret(3, 0); }                                               /* SetAllParameters (3D) */
void sub_0021A1AB(void) { black_ret(4, 0); }                                               /* SetRolloffCurve */
void sub_0021A1CF(void) { black_ret(3, 0); }                                               /* SetI3DL2Source */
void sub_0021A1EF(void) { WITH_BUF(3, b->play_start = ARG(1); b->play_len = ARG(2)); }     /* SetPlayRegion */
void sub_0021A7E1(void) { WITH_BUF(2, if (ARG(1)) voice_format(&b->v, ARG(1))); }          /* SetFormat */
void sub_0021A7FD(void)                                                                    /* SetBufferData */
{
    WITH_BUF(3, b->data = ARG(1); b->bytes = ARG(2); b->v.cached_block = -1;
                if (b->pos >= (double)frames_of_bytes(&b->v, b->bytes)) b->pos = 0.0);
}

/* --- streams (CDirectSoundStream methods: the interface vtable and the IDirectSoundStream_* thunks) */

#define WITH_STREAM(args, ...) do { DsStream *s; lock(); if ((s = stream_of(ARG(0))) != NULL) { __VA_ARGS__; } unlock(); black_ret(args, 0); } while (0)

void sub_00218D5D(void)                                                       /* AddRef */
{
    uint32_t refs = 0;
    DsStream *s;
    lock();
    if ((s = stream_of(ARG(0))) != NULL)
        refs = (uint32_t)++s->refs;
    unlock();
    black_ret(1, refs);
}

void sub_00218DA4(void)                                                       /* Release */
{
    uint32_t refs = 0;
    DsStream *s;
    lock();
    if ((s = stream_of(ARG(0))) != NULL) {
        refs = (uint32_t)--s->refs;
        if (!s->refs) {
            flush_stream(s);
            MEM32(s->handle + 4) = 0;
            s->used = 0;
        }
    }
    unlock();
    deliver_callbacks();
    black_ret(1, refs);
}

void sub_00218DF2(void)                                                       /* GetInfo(this, XMEDIAINFO*) */
{
    WITH_STREAM(2, if (ARG(1)) {
        uint32_t block = s->v.adpcm ? 36u * (uint32_t)s->v.channels : (uint32_t)s->v.align;
        MEM32(ARG(1)) = 5;                                    /* fixed sample size, async input */
        MEM32(ARG(1) + 4) = block;
        MEM32(ARG(1) + 8) = 0;
        MEM32(ARG(1) + 12) = s->v.adpcm ? 2u * block : 32u * (uint32_t)s->v.align;
    });
}

void sub_00218EF1(void)                                                       /* GetStatus(this, pdw) */
{
    deliver_callbacks();
    WITH_STREAM(2, if (ARG(1)) MEM32(ARG(1)) = stream_status(s));
}

void sub_00218F42(void)                                                       /* Process(this, input, output) */
{
    uint32_t r = 0, pkt = ARG(1);
    DsStream *s;
    deliver_callbacks();
    lock();
    if ((s = stream_of(ARG(0))) == NULL) {
        r = 0x80004005u;
    } else if (!((uint32_t)s->count < s->max_packets) || !pkt) {
        r = 0x88780032u;
    } else {
        Packet *p = &s->q[(s->head + s->count) % MAX_PACKETS];
        p->buffer = MEM32(pkt);
        p->size = MEM32(pkt + 4);
        p->completed_ptr = MEM32(pkt + 8);
        p->status_ptr = MEM32(pkt + 12);
        p->event_or_ctx = MEM32(pkt + 16);
        if (p->completed_ptr)
            MEM32(p->completed_ptr) = 0;
        if (p->status_ptr)
            MEM32(p->status_ptr) = STATUS_PENDING;
        s->count++;
        s->starved = 0;
    }
    unlock();
    black_ret(3, r);
}

void sub_00218E59(void) { WITH_STREAM(1, s->discontinuity = 1); }           /* Discontinuity */
void sub_00218EA6(void) { WITH_STREAM(1, flush_stream(s)); deliver_callbacks(); }   /* Flush */
void sub_00218FB9(void) { WITH_STREAM(2, s->paused = (ARG(1) & 1u) != 0); }  /* Pause(this, dwPause) */
void sub_0021900A(void)                                                       /* FlushEx(this, rt lo, rt hi, flags) */
{
    WITH_STREAM(4, if (!(ARG(3) & 1u)) flush_stream(s));                      /* async: the queue plays out */
    deliver_callbacks();
}
void sub_002194ED(void) { WITH_STREAM(2, s->v.volume = (int32_t)ARG(1)); }   /* SetVolume */
void sub_0021953F(void) { WITH_STREAM(2, s->v.headroom = (int32_t)ARG(1)); } /* SetHeadroom */
void sub_00219591(void) { WITH_STREAM(2, voice_mixbin_volumes(&s->v, ARG(1))); }   /* SetMixBinVolumes */
void sub_0021A0E5(void) { WITH_STREAM(2, voice_mixbins(&s->v, ARG(1))); }    /* SetMixBins */
