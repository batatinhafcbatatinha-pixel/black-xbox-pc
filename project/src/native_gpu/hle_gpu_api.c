// Black: ported from the FIFA Street 1 renderer (same DXVK layout: vulkan\ next to the executable).
/*
 * Graphics API choice: Direct3D 11 (the system's) or Vulkan.
 *
 * The renderer is written against D3D11. Vulkan is DXVK (vulkan\d3d11.dll and
 * vulkan\dxgi.dll next to the executable), which implements the same D3D11 and
 * DXGI interfaces on Vulkan -- one renderer, two APIs, and on Linux the same
 * code runs on DXVK's native build.
 *
 * The executable delay-loads d3d11.dll and dxgi.dll (CMakeLists.txt), so
 * nothing binds them before this runs: with Vulkan chosen, DXVK's two DLLs are
 * loaded here by full path, and every later D3D11 or DXGI call in the process
 * -- by name, through the delay-load thunks -- resolves to them. With D3D11
 * nothing is loaded here and the system DLLs come in on first use, as before.
 *
 * RECOMP_HLE_API=vulkan|d3d11 (the options' "API grafica", restart).
 */
#include "hle_d3d11_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle_gpu_api.h"

/* RECOMP_HLE_DUMP_HLSL=<dir>: every source, as <dir>/<hash>-<entry>-<target>.hlsl
 * (a run's complete shader set, for checking another compiler against it). */
static void dump_hlsl(const void *src, size_t len, const char *entry, const char *target)
{
    static int on = -1;
    static const char *dir;
    char path[1024];
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;
    FILE *f;
    if (on < 0) {
        dir = getenv("RECOMP_HLE_DUMP_HLSL");
        on = dir && *dir;
    }
    if (!on)
        return;
    for (i = 0; i < len; i++)
        h = (h ^ ((const uint8_t *)src)[i]) * 0x100000001B3ull;
    snprintf(path, sizeof path, "%s/%016llx-%s-%s.hlsl", dir, (unsigned long long)h, entry, target);
    if ((f = fopen(path, "rb")) != NULL) {
        fclose(f);
        return;
    }
    if ((f = fopen(path, "wb")) != NULL) {
        fwrite(src, 1, len, f);
        fclose(f);
    }
}

#ifdef _WIN32
HRESULT hle_d3d_compile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defines,
                        ID3DInclude *include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                        ID3DBlob **code, ID3DBlob **errors)
{
    dump_hlsl(src, len, entry, target);
    return D3DCompile(src, len, name, defines, include, entry, target, flags1, flags2, code, errors);
}
#else
/* Linux/macOS: HLSL -> DXBC through vkd3d-shader's C API (the compiler Wine's
 * d3dcompiler uses). Not vkd3d-utils' D3DCompile: on x86-64 it keeps the
 * Windows calling convention (ms_abi, for Wine) for itself and its blobs,
 * which DXVK native's headers do not. The result goes into a small ID3DBlob
 * of our own that the D3D11 calls (DXVK native) take as usual. */
#include <vkd3d_shader.h>

typedef struct HleBlob {
    ID3D10BlobVtbl *lpVtbl;
    LONG ref;
    void *data;
    SIZE_T size;
} HleBlob;

static HRESULT STDMETHODCALLTYPE blob_qi(ID3D10Blob *b, REFIID iid, void **out) { (void)iid; *out = b; return S_OK; }
static ULONG STDMETHODCALLTYPE blob_addref(ID3D10Blob *b) { return (ULONG)++((HleBlob *)b)->ref; }
static ULONG STDMETHODCALLTYPE blob_release(ID3D10Blob *b)
{
    HleBlob *h = (HleBlob *)b;
    LONG r = --h->ref;
    if (!r) {
        free(h->data);
        free(h);
    }
    return (ULONG)r;
}
static LPVOID STDMETHODCALLTYPE blob_ptr(ID3D10Blob *b) { return ((HleBlob *)b)->data; }
static SIZE_T STDMETHODCALLTYPE blob_size(ID3D10Blob *b) { return ((HleBlob *)b)->size; }
static ID3D10BlobVtbl s_blob_vtbl = { blob_qi, blob_addref, blob_release, blob_ptr, blob_size };

static ID3DBlob *blob_new(const void *data, size_t size)
{
    HleBlob *h = (HleBlob *)calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->lpVtbl = &s_blob_vtbl;
    h->ref = 1;
    h->size = size;
    h->data = malloc(size ? size : 1);
    if (!h->data) {
        free(h);
        return NULL;
    }
    memcpy(h->data, data, size);
    return (ID3DBlob *)h;
}

HRESULT hle_d3d_compile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defines,
                        ID3DInclude *include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                        ID3DBlob **code, ID3DBlob **errors)
{
    struct vkd3d_shader_hlsl_source_info hlsl;
    struct vkd3d_shader_compile_info info;
    struct vkd3d_shader_code out;
    char *messages = NULL;
    int r;
    (void)defines; (void)include; (void)flags1; (void)flags2;
    dump_hlsl(src, len, entry, target);
    memset(&hlsl, 0, sizeof hlsl);
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.entry_point = entry;
    hlsl.profile = target;
    memset(&info, 0, sizeof info);
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source.code = src;
    info.source.size = len;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = name;
    memset(&out, 0, sizeof out);
    r = vkd3d_shader_compile(&info, &out, &messages);
    if (code)
        *code = NULL;
    if (errors)
        *errors = NULL;
    if (r >= 0 && code)
        *code = blob_new(out.code, out.size);
    if (messages && *messages && errors)
        *errors = blob_new(messages, strlen(messages) + 1);
    vkd3d_shader_free_shader_code(&out);
    vkd3d_shader_free_messages(messages);
    return r >= 0 && (!code || *code) ? S_OK : E_FAIL;
}
#endif

#ifdef _WIN32
static int s_api = -1;
static int s_vulkan_available = -1;

static void vulkan_dir(wchar_t *dir)
{
    wchar_t *slash;
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    slash = wcsrchr(dir, L'\\');
    wcscpy(slash ? slash + 1 : dir, L"vulkan\\");
}

int hle_gpu_vulkan_available(void)
{
    if (s_vulkan_available < 0) {
        wchar_t dir[MAX_PATH], p[MAX_PATH];
        HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
        vulkan_dir(dir);
        swprintf(p, MAX_PATH, L"%lsd3d11.dll", dir);
        s_vulkan_available = loader != NULL && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
        swprintf(p, MAX_PATH, L"%lsdxgi.dll", dir);
        s_vulkan_available = s_vulkan_available && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
        if (loader)
            FreeLibrary(loader);
    }
    return s_vulkan_available;
}

int hle_gpu_api(void)
{
    if (s_api < 0) {
        const char *e = getenv("RECOMP_HLE_API");
        s_api = e && (*e == 'v' || *e == 'V' || *e == '1') ? HLE_API_VULKAN : HLE_API_D3D11;
    }
    return s_api;
}

const char *hle_gpu_api_name(void)
{
    return hle_gpu_api() == HLE_API_VULKAN ? "Vulkan" : "D3D11";
}

void hle_gpu_api_load(void)
{
    wchar_t dir[MAX_PATH], dxgi[MAX_PATH], d3d11[MAX_PATH];
    HMODULE m1, m2;
    if (hle_gpu_api() != HLE_API_VULKAN)
        return;
    if (!hle_gpu_vulkan_available()) {
        fprintf(stderr, "[GPU-API] Vulkan unavailable (no vulkan-1.dll or vulkan\\*.dll): using D3D11\n");
        s_api = HLE_API_D3D11;
        _putenv_s("RECOMP_HLE_API", "d3d11");
        return;
    }
    /* DXVK writes a log per DLL into the working directory unless told. */
    if (!getenv("DXVK_LOG_LEVEL"))
        _putenv_s("DXVK_LOG_LEVEL", "none");
    vulkan_dir(dir);
    swprintf(dxgi, MAX_PATH, L"%lsdxgi.dll", dir);
    swprintf(d3d11, MAX_PATH, L"%lsd3d11.dll", dir);
    m1 = LoadLibraryExW(dxgi, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    m2 = m1 ? LoadLibraryExW(d3d11, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
    if (!m1 || !m2) {
        fprintf(stderr, "[GPU-API] loading DXVK failed (%lu): using D3D11\n", (unsigned long)GetLastError());
        if (m1)
            FreeLibrary(m1);
        s_api = HLE_API_D3D11;
        _putenv_s("RECOMP_HLE_API", "d3d11");
        return;
    }
    {
        /* The modules every by-name lookup now finds. */
        wchar_t a[MAX_PATH], b[MAX_PATH];
        GetModuleFileNameW(GetModuleHandleW(L"dxgi.dll"), a, MAX_PATH);
        GetModuleFileNameW(GetModuleHandleW(L"d3d11.dll"), b, MAX_PATH);
        fprintf(stderr, "[GPU-API] Vulkan through DXVK: %ls, %ls\n", a, b);
    }
}

int hle_gpu_d3d11_available(void)
{
    return 1;
}
#else
/* Linux/macOS: DXVK native is the D3D11 -- Vulkan is the only API. */
int hle_gpu_api(void)
{
    return HLE_API_VULKAN;
}

const char *hle_gpu_api_name(void)
{
    return "Vulkan";
}

int hle_gpu_vulkan_available(void)
{
    return 1;
}

int hle_gpu_d3d11_available(void)
{
    return 0;
}

void hle_gpu_api_load(void)
{
    if (!getenv("DXVK_LOG_LEVEL"))
        setenv("DXVK_LOG_LEVEL", "none", 0);
}
#endif
