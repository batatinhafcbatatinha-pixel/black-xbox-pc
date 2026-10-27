/*
 * Linux/macOS: the slice of GDI the options screen draws with (hle_options.c)
 * -- a memory DC over a 32-bit top-down DIB, TrueType text (SDL_ttf), solid
 * rectangles, a convex polygon, an ellipse and a rounded rectangle -- so the
 * screen's drawing code is the same on every system.
 */
#ifndef HLE_GDI_POSIX_H
#define HLE_GDI_POSIX_H

#ifndef _WIN32
#include "hle_posix_win32.h"

typedef void *HGDIOBJ, *HBITMAP, *HFONT, *HBRUSH, *HPEN;

typedef struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth, biHeight;
    WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; DWORD bmiColors[1]; } BITMAPINFO;

#define BI_RGB              0
#define DIB_RGB_COLORS      0
#define TRANSPARENT         1
#define BLACK_BRUSH         4
#define PS_SOLID            0
#define FW_NORMAL           400
#define FW_BOLD             700
#define FW_HEAVY            900
#define DEFAULT_CHARSET     1
#define OUT_TT_PRECIS       4
#define CLIP_DEFAULT_PRECIS 0
#define ANTIALIASED_QUALITY 4
#define DEFAULT_PITCH       0
#define DT_LEFT             0x0000
#define DT_CENTER           0x0001
#define DT_RIGHT            0x0002
#define DT_VCENTER          0x0004
#define DT_SINGLELINE       0x0020
#define DT_NOPREFIX         0x0800
#define DT_END_ELLIPSIS     0x8000
#ifndef GetRValue
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))
#endif

static inline BOOL SetRect(RECT *r, int l, int t, int rr, int b)
{
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
    return TRUE;
}

HDC CreateCompatibleDC(HDC dc);
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj);
BOOL DeleteObject(HGDIOBJ obj);
HGDIOBJ GetStockObject(int which);
HFONT CreateFontW(int height, int width, int esc, int orient, int weight, DWORD italic, DWORD underline,
                  DWORD strike, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality,
                  DWORD pitch, const wchar_t *face);
HBRUSH CreateSolidBrush(COLORREF c);
HPEN CreatePen(int style, int width, COLORREF c);
COLORREF SetTextColor(HDC dc, COLORREF c);
int SetBkMode(HDC dc, int mode);
int DrawTextW(HDC dc, const wchar_t *s, int n, RECT *r, UINT format);
BOOL GetTextExtentPoint32W(HDC dc, const wchar_t *s, int n, SIZE *sz);
int FillRect(HDC dc, const RECT *r, HBRUSH b);
BOOL Polygon(HDC dc, const POINT *p, int n);
BOOL Ellipse(HDC dc, int l, int t, int r, int b);
BOOL RoundRect(HDC dc, int l, int t, int r, int b, int w, int h);
static inline BOOL GdiFlush(void) { return TRUE; }
/* A TrueType face in memory, used by CreateFontW for that family name. */
int hle_gdi_register_face(const wchar_t *family, const void *data, size_t len);

#endif /* !_WIN32 */
#endif
