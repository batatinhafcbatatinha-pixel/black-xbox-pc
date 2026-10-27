/* Linux/macOS implementation of hle_gdi_posix.h (see there). */
#ifndef _WIN32
#include "hle_gdi_posix.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <math.h>

enum { O_DC = 1, O_BITMAP, O_FONT, O_BRUSH, O_PEN };

typedef struct GdiObj {
    int kind;
    COLORREF color;                 /* brush, pen */
    uint32_t *bits;                 /* bitmap */
    int w, h;
    TTF_Font *font;                 /* font */
} GdiObj;

typedef struct GdiDC {
    int kind;
    GdiObj *bmp, *font, *brush, *pen;
    COLORREF text;
} GdiDC;

static GdiObj s_black_brush = { O_BRUSH, 0, NULL, 0, 0, NULL };

/* DIB pixels are 0x00RRGGBB (BGRA bytes); COLORREF is 0x00BBGGRR. */
static uint32_t px(COLORREF c)
{
    return ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c);
}

HDC CreateCompatibleDC(HDC dc)
{
    GdiDC *d = (GdiDC *)calloc(1, sizeof *d);
    (void)dc;
    if (d)
        d->kind = O_DC;
    return (HDC)d;
}

HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset)
{
    GdiObj *o;
    int w = bi->bmiHeader.biWidth, h = bi->bmiHeader.biHeight;
    (void)dc; (void)usage; (void)section; (void)offset;
    if (h < 0)
        h = -h;                     /* top-down is what the screen asks for */
    if (w <= 0 || h <= 0 || bi->bmiHeader.biBitCount != 32)
        return NULL;
    o = (GdiObj *)calloc(1, sizeof *o);
    if (!o)
        return NULL;
    o->kind = O_BITMAP;
    o->w = w;
    o->h = h;
    o->bits = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    if (!o->bits) {
        free(o);
        return NULL;
    }
    *bits = o->bits;
    return (HBITMAP)o;
}

HGDIOBJ SelectObject(HDC dch, HGDIOBJ obj)
{
    GdiDC *d = (GdiDC *)dch;
    GdiObj *o = (GdiObj *)obj, *old = NULL;
    if (!d || !o)
        return NULL;
    switch (o->kind) {
    case O_BITMAP: old = d->bmp; d->bmp = o; break;
    case O_FONT: old = d->font; d->font = o; break;
    case O_BRUSH: old = d->brush; d->brush = o; break;
    case O_PEN: old = d->pen; d->pen = o; break;
    }
    return (HGDIOBJ)old;
}

BOOL DeleteObject(HGDIOBJ obj)
{
    GdiObj *o = (GdiObj *)obj;
    if (!o || o == &s_black_brush)
        return FALSE;
    if (o->kind == O_BITMAP)
        free(o->bits);
    if (o->kind == O_FONT && o->font)
        TTF_CloseFont(o->font);
    free(o);
    return TRUE;
}

HGDIOBJ GetStockObject(int which)
{
    (void)which;
    return (HGDIOBJ)&s_black_brush;
}

HBRUSH CreateSolidBrush(COLORREF c)
{
    GdiObj *o = (GdiObj *)calloc(1, sizeof *o);
    if (o) { o->kind = O_BRUSH; o->color = c; }
    return (HBRUSH)o;
}

HPEN CreatePen(int style, int width, COLORREF c)
{
    GdiObj *o = (GdiObj *)calloc(1, sizeof *o);
    (void)style; (void)width;
    if (o) { o->kind = O_PEN; o->color = c; }
    return (HPEN)o;
}

COLORREF SetTextColor(HDC dch, COLORREF c)
{
    GdiDC *d = (GdiDC *)dch;
    COLORREF old = d ? d->text : 0;
    if (d)
        d->text = c;
    return old;
}

int SetBkMode(HDC dc, int mode)
{
    (void)dc;
    return mode;                    /* text is always drawn transparent here */
}

/* ------------------------------------------------------------- fonts */

/* A sans-serif TrueType face: next to the executable (fonts/), then the
 * usual places Linux distributions and macOS keep one. */
static TTF_Font *open_face(int px_size, int bold, int italic)
{
    static const char *const dirs[] = {
        "", "/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/dejavu/", "/usr/share/fonts/TTF/",
        "/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/liberation/",
        "/usr/share/fonts/truetype/noto/", "/usr/share/fonts/noto/", "/usr/share/fonts/truetype/freefont/",
        "/System/Library/Fonts/Supplemental/", "/Library/Fonts/",
    };
    static const char *const regular[] = { "DejaVuSans.ttf", "LiberationSans-Regular.ttf", "NotoSans-Regular.ttf", "FreeSans.ttf", "Arial.ttf" };
    static const char *const boldf[] = { "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf", "NotoSans-Bold.ttf", "FreeSansBold.ttf", "Arial Bold.ttf" };
    static const char *const bolditalic[] = { "DejaVuSans-BoldOblique.ttf", "LiberationSans-BoldItalic.ttf", "NotoSans-BoldItalic.ttf", "FreeSansBoldOblique.ttf", "Arial Bold Italic.ttf" };
    const char *const *names = italic ? bolditalic : bold ? boldf : regular;
    char exe_dir[MAX_PATH], path[MAX_PATH + 256];
    wchar_t w[MAX_PATH];
    size_t d, k;
    TTF_Font *f = NULL;
    int styled = 0;

    if (!TTF_WasInit() && TTF_Init() != 0) {
        fprintf(stderr, "[OPTIONS] TTF_Init failed: %s\n", TTF_GetError());
        return NULL;
    }
    GetModuleFileNameW(NULL, w, MAX_PATH);
    hle_posix_path(w, exe_dir, sizeof exe_dir);
    if (strrchr(exe_dir, '/'))
        strcpy(strrchr(exe_dir, '/') + 1, "fonts/");
    for (int pass = 0; pass < 2 && !f; pass++) {
        if (pass)                        /* no styled file: the regular face, styled by SDL_ttf */
            names = regular, styled = 1;
        for (d = 0; d < sizeof dirs / sizeof dirs[0] && !f; d++)
            for (k = 0; k < 5 && !f; k++) {
                snprintf(path, sizeof path, "%s%s", d ? dirs[d] : exe_dir, names[k]);
                f = TTF_OpenFont(path, px_size);
            }
    }
    if (f && styled)
        TTF_SetFontStyle(f, (bold ? TTF_STYLE_BOLD : 0) | (italic ? TTF_STYLE_ITALIC : 0));
    if (!f)
        fprintf(stderr, "[OPTIONS] no TrueType font found (put DejaVuSans*.ttf in fonts/ next to the game)\n");
    return f;
}

/* Faces handed over in memory (the game's own fonts), by family name. The
 * data stays owned by the caller for the life of the process. */
static struct { wchar_t name[64]; const void *data; size_t len; } s_faces[8];

int hle_gdi_register_face(const wchar_t *family, const void *data, size_t len)
{
    size_t i;
    for (i = 0; i < sizeof s_faces / sizeof s_faces[0]; i++)
        if (!s_faces[i].data) {
            wcsncpy(s_faces[i].name, family, 63);
            s_faces[i].data = data;
            s_faces[i].len = len;
            return 1;
        }
    return 0;
}

static TTF_Font *open_registered(const wchar_t *face, int px_size)
{
    size_t i;
    if (!face || (!TTF_WasInit() && TTF_Init() != 0))
        return NULL;
    for (i = 0; i < sizeof s_faces / sizeof s_faces[0]; i++)
        if (s_faces[i].data && !wcscmp(s_faces[i].name, face))
            return TTF_OpenFontRW(SDL_RWFromConstMem(s_faces[i].data, (int)s_faces[i].len), 1, px_size);
    return NULL;
}

HFONT CreateFontW(int height, int width, int esc, int orient, int weight, DWORD italic, DWORD underline,
                  DWORD strike, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality,
                  DWORD pitch, const wchar_t *face)
{
    GdiObj *o = (GdiObj *)calloc(1, sizeof *o);
    (void)width; (void)esc; (void)orient; (void)underline; (void)strike; (void)charset;
    (void)out_precision; (void)clip_precision; (void)quality; (void)pitch;
    if (!o)
        return NULL;
    o->kind = O_FONT;
    o->font = open_registered(face, height < 0 ? -height : height);
    if (!o->font)
        o->font = open_face(height < 0 ? -height : height, weight >= FW_BOLD, italic != 0);
    return (HFONT)o;
}

static void to_utf8(const wchar_t *s, int n, char *out, size_t cap)
{
    size_t o = 0;
    int i;
    for (i = 0; (n < 0 ? s[i] != 0 : i < n) && o + 4 < cap; i++) {
        uint32_t c = (uint32_t)s[i];
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63));
            out[o++] = (char)(0x80 | (c & 63));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 63));
            out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63));
        }
    }
    out[o] = 0;
}

BOOL GetTextExtentPoint32W(HDC dch, const wchar_t *s, int n, SIZE *sz)
{
    GdiDC *d = (GdiDC *)dch;
    char u[1024];
    int w = 0, h = 0;
    sz->cx = sz->cy = 0;
    if (!d || !d->font || !d->font->font)
        return FALSE;
    to_utf8(s, n, u, sizeof u);
    TTF_SizeUTF8(d->font->font, u, &w, &h);
    sz->cx = w;
    sz->cy = h;
    return TRUE;
}

/* Coverage of the rendered glyphs, blended onto the DIB in the text colour. */
static void blit_text(GdiDC *d, SDL_Surface *s, int x, int y, const RECT *clip)
{
    uint32_t col = px(d->text);
    int cr = (col >> 16) & 255, cg = (col >> 8) & 255, cb = col & 255;
    GdiObj *b = d->bmp;
    for (int j = 0; j < s->h; j++) {
        int ty = y + j;
        const uint32_t *row = (const uint32_t *)((const uint8_t *)s->pixels + (size_t)j * s->pitch);
        if (ty < 0 || ty >= b->h || ty < clip->top || ty >= clip->bottom)
            continue;
        for (int i = 0; i < s->w; i++) {
            int tx = x + i;
            uint32_t a = row[i] >> 24, p;
            if (!a || tx < 0 || tx >= b->w || tx < clip->left || tx >= clip->right)
                continue;
            p = b->bits[(size_t)ty * b->w + tx];
            b->bits[(size_t)ty * b->w + tx] =
                (p & 0xFF000000u) |
                ((uint32_t)((((p >> 16) & 255) * (255 - a) + cr * a) / 255) << 16) |
                ((uint32_t)((((p >> 8) & 255) * (255 - a) + cg * a) / 255) << 8) |
                (uint32_t)(((p & 255) * (255 - a) + cb * a) / 255);
        }
    }
}

int DrawTextW(HDC dch, const wchar_t *s, int n, RECT *r, UINT format)
{
    GdiDC *d = (GdiDC *)dch;
    char u[1024];
    int w = 0, h = 0, x, y, len;
    SDL_Surface *surf;
    SDL_Color white = { 255, 255, 255, 255 };
    if (!d || !d->bmp || !d->font || !d->font->font || !s)
        return 0;
    to_utf8(s, n, u, sizeof u);
    if (!u[0])
        return 0;
    TTF_SizeUTF8(d->font->font, u, &w, &h);
    if ((format & DT_END_ELLIPSIS) && w > r->right - r->left) {
        /* Drop characters (whole UTF-8 sequences) until "text…" fits. */
        len = (int)strlen(u);
        while (len > 0) {
            do len--; while (len > 0 && ((unsigned char)u[len] & 0xC0) == 0x80);
            memcpy(u + len, "\xE2\x80\xA6", 4);
            TTF_SizeUTF8(d->font->font, u, &w, &h);
            if (w <= r->right - r->left)
                break;
        }
    }
    surf = TTF_RenderUTF8_Blended(d->font->font, u, white);
    if (!surf)
        return 0;
    if (format & DT_CENTER)
        x = r->left + ((r->right - r->left) - surf->w) / 2;
    else if (format & DT_RIGHT)
        x = r->right - surf->w;
    else
        x = r->left;
    y = (format & DT_VCENTER) ? r->top + ((r->bottom - r->top) - surf->h) / 2 : r->top;
    if (surf->format->format != SDL_PIXELFORMAT_ARGB8888) {
        SDL_Surface *c = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ARGB8888, 0);
        SDL_FreeSurface(surf);
        surf = c;
        if (!surf)
            return 0;
    }
    blit_text(d, surf, x, y, r);
    h = surf->h;
    SDL_FreeSurface(surf);
    return h;
}

/* ------------------------------------------------------------ shapes */

static void span(GdiObj *b, int y, int x0, int x1, uint32_t c)
{
    if (y < 0 || y >= b->h)
        return;
    if (x0 < 0) x0 = 0;
    if (x1 > b->w) x1 = b->w;
    for (int x = x0; x < x1; x++)
        b->bits[(size_t)y * b->w + x] = c;
}

int FillRect(HDC dch, const RECT *r, HBRUSH br)
{
    GdiDC *d = (GdiDC *)dch;
    GdiObj *o = (GdiObj *)br;
    if (!d || !d->bmp || !o)
        return 0;
    for (int y = r->top; y < r->bottom; y++)
        span(d->bmp, y, r->left, r->right, px(o->color));
    return 1;
}

/* Convex polygon, filled with the brush; GDI includes the left/top edge. */
BOOL Polygon(HDC dch, const POINT *p, int n)
{
    GdiDC *d = (GdiDC *)dch;
    int ymin = INT32_MAX, ymax = INT32_MIN;
    uint32_t c;
    if (!d || !d->bmp || n < 3)
        return FALSE;
    c = px(d->brush ? d->brush->color : 0);
    for (int i = 0; i < n; i++) {
        if (p[i].y < ymin) ymin = p[i].y;
        if (p[i].y > ymax) ymax = p[i].y;
    }
    for (int y = ymin; y < ymax; y++) {
        double fy = y + 0.5, xl = 1e9, xr = -1e9;
        for (int i = 0; i < n; i++) {
            const POINT *a = &p[i], *b = &p[(i + 1) % n];
            if ((a->y <= fy && b->y > fy) || (b->y <= fy && a->y > fy)) {
                double x = a->x + (fy - a->y) * (double)(b->x - a->x) / (double)(b->y - a->y);
                if (x < xl) xl = x;
                if (x > xr) xr = x;
            }
        }
        if (xl <= xr)
            span(d->bmp, y, (int)lround(xl), (int)lround(xr), c);
    }
    return TRUE;
}

/* Filled with the brush, outlined one pixel in the pen colour. */
static void rounded(GdiDC *d, int l, int t, int r, int b, int rw, int rh)
{
    uint32_t fillc = px(d->brush ? d->brush->color : 0), penc = px(d->pen ? d->pen->color : 0);
    double ax = rw / 2.0, ay = rh / 2.0;
    for (int y = t; y < b; y++) {
        double fy = y + 0.5, dx = 0.0;
        int x0, x1;
        if (fy < t + ay)
            dx = ax - ax * sqrt(fmax(0.0, 1.0 - ((t + ay - fy) / ay) * ((t + ay - fy) / ay)));
        else if (fy > b - ay)
            dx = ax - ax * sqrt(fmax(0.0, 1.0 - ((fy - (b - ay)) / ay) * ((fy - (b - ay)) / ay)));
        x0 = (int)lround(l + dx);
        x1 = (int)lround(r - dx);
        span(d->bmp, y, x0, x1, (y == t || y == b - 1) ? penc : fillc);
        span(d->bmp, y, x0, x0 + 1, penc);
        span(d->bmp, y, x1 - 1, x1, penc);
    }
}

BOOL Ellipse(HDC dch, int l, int t, int r, int b)
{
    GdiDC *d = (GdiDC *)dch;
    if (!d || !d->bmp)
        return FALSE;
    rounded(d, l, t, r, b, r - l, b - t);
    return TRUE;
}

BOOL RoundRect(HDC dch, int l, int t, int r, int b, int w, int h)
{
    GdiDC *d = (GdiDC *)dch;
    if (!d || !d->bmp)
        return FALSE;
    rounded(d, l, t, r, b, w, h);
    return TRUE;
}

#endif /* !_WIN32 */
