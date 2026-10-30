/*
 * Native particle batches: 0x0015B380 (simulate, cull and colour one emitter's particles, 1024 at a time, and draw
 * them through 0x0002AC00) and its quad callback 0x0015BF30 (four rotated corners into the vertex buffer at
 * 0x0053CAE0). Together they were ~10% of the game thread in mission 1 (smoke, sparks, dust): the translated bodies
 * keep every temporary in guest stack memory through volatile accesses.
 *
 * Both are ports of the translated code operation by operation -- the same single-precision operations in the same
 * order, minss/maxss as (a < b ? a : b), cvttss2si as an (int32_t) cast, fsin/fcos through double like the
 * translation -- so the vertices are bit-identical. Lanes the original reads from uninitialised stack (the w lane of
 * the acceleration and of the rotation rows) never reach an output, so they are zero here.
 *
 * BLACK_NATIVE_PARTICLES=0 runs the translated bodies. BLACK_PARTICLES_CHECK=1 runs the translated 0x0015B380 first
 * with its draws captured (not drawn), then the native one, and compares every batch byte by byte (log: MISMATCH);
 * RECOMP_HLE_AB=2 alternates native and translated every 600 frames.
 */
#include "black_win32.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "black_native.h"
#include "native_gpu/black_gpu.h"

void sub_0015B380_gen(void);
void sub_0015BF30_gen(void);
void sub_0002AC00_gen(void);

#define K_ONE      0x295C74u       /* 1.0f */
#define K_255      0x295CB4u       /* colour scale */
#define K_ANGLE    0x2F81B0u       /* rotation offset */
#define VB         0x53CAE0u       /* the batch vertex buffer, 32-byte vertices */
#define CB_TABLE   0x289DF0u       /* quad callback per emitter type */
#define CB_QUAD    0x0015BF30u

static int s_on = -1, s_check;
static uint64_t s_batches, s_mismatch;

static int native_on(void)
{
    if (s_on < 0) {
        const char *e = getenv("BLACK_NATIVE_PARTICLES");
        s_on = !(e && *e == '0');
        e = getenv("BLACK_PARTICLES_CHECK");
        s_check = e && *e && *e != '0';
        fprintf(stderr, "[BLACK-PARTICLES] native %s%s\n", s_on ? "on" : "off", s_check ? " (check mode)" : "");
    }
    return s_on && !BlackGpuAbNewOff();
}

static inline float F(uint32_t a) { return MEMF(a); }
static inline float fmin_x(float a, float b) { return a < b ? a : b; }   /* minss a, b */
static inline float fmax_x(float a, float b) { return a > b ? a : b; }   /* maxss a, b */

/* 0x0015BF30(cam, particle, first vertex, age, colour[4], pos[4], size[4]) */
static void quad(uint32_t a, uint32_t p, uint32_t first, float age, const float col[4], const float pos[4],
                 const float size[4])
{
    float f98 = F(a + 0x98), tmin = fmin_x(age, f98), tmax = fmax_x(age, f98), x2, x3, s, c, one = F(K_ONE);
    float rows[4][4], v[4][4], k255 = F(K_255);
    uint32_t i, l, dst;

    x2 = F(a + 0x9C) * tmin;
    x2 = x2 * tmin;
    x3 = F(a + 0x94) * tmax;
    x2 = x2 + x3;
    x3 = F(a + 0x90) * tmin;
    x2 = x2 + x3;
    x2 = x2 + F(a + 0xA0);
    x2 = x2 * F(p + 0x1C);
    x2 = x2 + F(K_ANGLE);
    s = (float)sin((double)x2);
    c = (float)cos((double)x2);
    {
        float ns = 0.0f - s, nc = 0.0f - c;
        float r0[4] = { ns, c, one, 0 }, r1[4] = { c, s, one, 0 }, r2[4] = { s, nc, one, 0 }, r3[4] = { nc, ns, one, 0 };
        float e0[4] = { ns, c, 0.0f, 0 }, e1[4] = { c, s, 0.0f, 0 };
        memcpy(rows[0], r0, 16); memcpy(rows[1], r1, 16); memcpy(rows[2], r2, 16); memcpy(rows[3], r3, 16);
        for (l = 0; l < 4; l++) {
            v[0][l] = size[l] * e0[l];
            v[1][l] = size[l] * e1[l];
            v[2][l] = -v[0][l];                                    /* xorps sign */
            v[3][l] = -v[1][l];
        }
    }
    dst = VB + (first << 5);
    for (i = 0; i < 4; i++, dst += 0x20) {
        float d[3], sum, f, lerp[4], out[4];
        int32_t ia, ir, ig, ib;
        uint32_t px, py, pz, color;
        for (l = 0; l < 4; l++) {
            v[i][l] = v[i][l] + pos[l];
            rows[i][l] = rows[i][l] * F(a + 0x30 + 4 * l);
        }
        for (l = 0; l < 3; l++)
            d[l] = rows[i][l] * F(a + 4 * l);
        sum = d[0] + d[1];
        sum = sum + d[2];
        f = F(p + 0x24) * sum;
        f = f + F(p + 0x20);
        for (l = 0; l < 4; l++) {
            lerp[l] = F(a + 0x20 + 4 * l) - F(a + 0x10 + 4 * l);
            lerp[l] = lerp[l] * f;
            lerp[l] = lerp[l] + F(a + 0x10 + 4 * l);
            out[l] = col[l] * lerp[l];
            out[l] = out[l] * k255;
        }
        ia = (int32_t)out[3];
        ir = (int32_t)out[0];
        ig = (int32_t)out[1];
        ib = (int32_t)out[2];
        color = (((((uint32_t)ir & 0xFFu) | ((uint32_t)ia << 8)) << 8 | ((uint32_t)ig & 0xFFu)) << 8) | ((uint32_t)ib & 0xFFu);
        memcpy(&px, &v[i][0], 4);
        memcpy(&py, &v[i][1], 4);
        memcpy(&pz, &v[i][2], 4);
        MEM32(dst + 0x10) = color;
        MEM32(dst + 0x00) = px;
        MEM32(dst + 0x04) = py;
        MEM32(dst + 0x08) = pz;
        MEMF(dst + 0x0C) = one;
    }
}

void sub_0015BF30(void)
{
    float col[4], pos[4], size[4], age;
    uint32_t a5, a6, a7, l, bits;
    if (!native_on()) {
        sub_0015BF30_gen();
        return;
    }
    a5 = black_arg(4); a6 = black_arg(5); a7 = black_arg(6);
    for (l = 0; l < 4; l++) {
        col[l] = F(a5 + 4 * l);
        pos[l] = F(a6 + 4 * l);
        size[l] = F(a7 + 4 * l);
    }
    bits = black_arg(3);
    memcpy(&age, &bits, 4);
    quad(black_arg(0), black_arg(1), black_arg(2), age, col, pos, size);
    g_esp += 4;                                                    /* cdecl ret */
}

/* --- check mode: the translated batch's draws are captured instead of drawn --------------------------------- */

typedef struct { uint32_t args[4]; uint32_t bytes; uint8_t *data; } Batch;
static Batch s_cap[64];
static int s_capturing, s_ncap, s_ncmp = -1;

static void draw_batch(uint32_t count)
{
    uint32_t a[4] = { 8u, count, VB, 0x20u };
    if (s_ncmp >= 0) {
        /* check mode, native pass: compare with the translated pass's batch of the same index */
        Batch *b = s_ncmp < s_ncap ? &s_cap[s_ncmp] : NULL;
        uint32_t bytes = count * 0x20u;
        if (!b || memcmp(b->args, a, sizeof a) || b->bytes != bytes || memcmp(b->data, (const void *)XBOX_PTR(VB), bytes)) {
            if (++s_mismatch <= 16)
                fprintf(stderr, "[BLACK-PARTICLES] MISMATCH batch %d: native %u vertices, translated %u\n",
                        s_ncmp, count, b ? b->args[1] : 0u);
        }
        s_ncmp++;
    }
    black_guest_call(0x0002AC00u, 4, a);
    g_esp += 16;                                                   /* cdecl: the caller pops */
}

void sub_0002AC00(void)
{
    if (s_capturing) {
        Batch *b;
        if (s_ncap < 64) {
            b = &s_cap[s_ncap++];
            for (int i = 0; i < 4; i++) b->args[i] = black_arg(i);
            b->bytes = b->args[1] * 0x20u;
            b->data = (uint8_t *)realloc(b->data, b->bytes ? b->bytes : 1);
            memcpy(b->data, (const void *)XBOX_PTR(b->args[2]), b->bytes);
        }
        g_esp += 4;
        return;
    }
    sub_0002AC00_gen();
}

/* --- 0x0015B380(list, emitter, camera), stdcall -------------------------------------------------------------- */

static void batch_native(uint32_t list, uint32_t em, uint32_t cam)
{
    float one = F(K_ONE), acc[4] = { 0.0f, F(em + 0x78), 0.0f, 0.0f }, inv_life = one / F(em + 0x70);
    uint32_t cb = MEM32(MEM32(em + 0x64) * 4u + CB_TABLE), vcount = 0, edi = 0, total = MEM32(list + 8);
    if (total <= edi)
        return;
    for (;;) {
        uint32_t rem = total - edi, n = rem > 0x400u ? 0x400u : rem;
        if (edi < n) {
            uint32_t off = edi * 0x30u, cnt = n - edi;
            edi = edi + cnt;
            do {
                uint32_t pp = MEM32(list + 0xC) + off;
                float age = F(cam + 0x140) - F(pp + 0xC);
                off += 0x30u;
                if (age >= F(em + 0x70) || 0.0f >= age)
                    continue;
                {
                    float x2 = age * inv_life, e84 = F(em + 0x84), tmin = fmin_x(age, e84), tmax = fmax_x(age, e84);
                    float k, x5, age2, P[4], T[4], z, x, y, w6, s0, s1, av[4], bv[4], size, invz, sz4[4], col[4];
                    float f0, f1, m, alpha, pos[4];
                    uint32_t l;
                    k = tmin * tmin;
                    k = k * F(em + 0x88);
                    x5 = F(em + 0x80) * tmax;
                    k = k + x5;
                    x5 = F(em + 0x7C) * tmin;
                    age2 = age * age;
                    k = k + x5;
                    k = k - F(em + 0x8C);
                    for (l = 0; l < 4; l++) {
                        float a = acc[l] * age2, v = F(pp + 0x10 + 4 * l) * k;
                        P[l] = F(pp + 4 * l) + v;
                        P[l] = P[l] + a;
                    }
                    for (l = 0; l < 4; l++) {
                        float t = P[0] * F(cam + 0xF0 + 4 * l);
                        t = t + F(cam + 0x120 + 4 * l);
                        t = t + P[1] * F(cam + 0x100 + 4 * l);
                        t = t + P[2] * F(cam + 0x110 + 4 * l);
                        T[l] = t;
                    }
                    z = T[2];
                    if (F(cam + 0x40) >= z || z >= F(cam + 0x44))
                        continue;
                    x = T[0];
                    if (0.0f - F(cam + 0x60) * z >= x)
                        continue;
                    y = T[1];
                    if (0.0f - F(cam + 0x64) * z >= y)
                        continue;
                    if (x >= (F(cam + 0x60) + one) * z || y >= (F(cam + 0x64) + one) * z)
                        continue;
                    if (F(em + 0x74) > x2) {
                        w6 = F(cam + 0x50) * x2;
                        s0 = F(em + 0xA8);
                        s1 = F(em + 0xA4);
                        for (l = 0; l < 4; l++) { av[l] = F(em + 4 * l); bv[l] = F(em + 0x10 + 4 * l); }
                    } else {
                        w6 = x2 - F(em + 0x74);
                        w6 = w6 * F(cam + 0x54);
                        s0 = F(em + 0xAC);
                        s1 = F(em + 0xA8);
                        for (l = 0; l < 4; l++) { av[l] = F(em + 0x10 + 4 * l); bv[l] = F(em + 0x20 + 4 * l); }
                    }
                    size = s0 - s1;
                    size = size * w6;
                    invz = one / z;
                    size = size + s1;
                    x = x * invz;
                    y = y * invz;
                    size = size * F(pp + 0x28);
                    size = size * invz;
                    for (l = 0; l < 4; l++)
                        sz4[l] = F(cam + 0x80 + 4 * l) * size;
                    if (F(cam + 0x70) > sz4[0])
                        continue;
                    for (l = 0; l < 4; l++) {                       /* 0x00011DD0: (b - a) * t + a */
                        col[l] = bv[l] - av[l];
                        col[l] = col[l] * w6;
                        col[l] = col[l] + av[l];
                    }
                    f1 = F(cam + 0x44) - z;
                    f0 = z - F(cam + 0x40);
                    f1 = f1 * F(cam + 0x4C);
                    f0 = f0 * F(cam + 0x48);
                    m = fmin_x(f0, f1);
                    m = fmin_x(m, one);
                    alpha = col[3] * m;
                    alpha = alpha * F(pp + 0x2C);
                    if (F(em + 0xD0) >= alpha)
                        continue;
                    if (sz4[0] > F(cam + 0x68)) {
                        sz4[0] = F(cam + 0x68);
                        sz4[1] = F(cam + 0x6C);
                        sz4[2] = 0.0f;
                    }
                    pos[0] = x; pos[1] = y; pos[2] = invz; pos[3] = T[3];
                    col[3] = alpha;
                    if (cb == CB_QUAD) {
                        quad(cam, pp, vcount, age, col, pos, sz4);
                    } else {
                        /* another emitter type: its callback gets the three vectors in guest memory */
                        uint32_t frame = (g_esp - 0x40u) & ~15u, saved = g_esp, a[7], bits;
                        for (l = 0; l < 4; l++) {
                            MEMF(frame + 4 * l) = col[l];
                            MEMF(frame + 0x10 + 4 * l) = pos[l];
                            MEMF(frame + 0x20 + 4 * l) = sz4[l];
                        }
                        memcpy(&bits, &age, 4);
                        a[0] = cam; a[1] = pp; a[2] = vcount; a[3] = bits; a[4] = frame; a[5] = frame + 0x10; a[6] = frame + 0x20;
                        g_esp = frame;
                        black_guest_call(cb, 7, a);
                        g_esp = saved;
                    }
                    vcount += 4;
                }
            } while (--cnt);
        }
        if (vcount > 0) {
            draw_batch(vcount);
            vcount = 0;
        }
        if (edi >= MEM32(list + 8))
            return;
        total = MEM32(list + 8);
    }
}

void sub_0015B380(void)
{
    uint32_t list, em, cam;
    if (!native_on()) {
        sub_0015B380_gen();
        return;
    }
    list = black_arg(0); em = black_arg(1); cam = black_arg(2);
    if (s_check) {
        uint32_t a[3] = { list, em, cam };
        s_capturing = 1;
        s_ncap = 0;
        for (int i = 2; i >= 0; --i)                             /* translated pass, called directly: the */
            PUSH32(g_esp, a[i]);                                   /* dispatch would find this native body */
        PUSH32(g_esp, 0u);
        sub_0015B380_gen();                                        /* stdcall: pops its arguments */
        s_capturing = 0;
        s_ncmp = 0;
        batch_native(list, em, cam);
        if (s_ncmp != s_ncap && ++s_mismatch <= 16)
            fprintf(stderr, "[BLACK-PARTICLES] MISMATCH: native drew %d batches, translated %d\n", s_ncmp, s_ncap);
        s_ncmp = -1;
        if ((++s_batches & 1023u) == 0)
            fprintf(stderr, "[BLACK-PARTICLES] checked %llu emitter updates, mismatches %llu\n",
                    (unsigned long long)s_batches, (unsigned long long)s_mismatch);
    } else {
        batch_native(list, em, cam);
    }
    g_esp += 16;                                                   /* ret 12 */
}
