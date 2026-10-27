/*
 * BLACK_WATCH=<va>[:<bytes>] (diagnostic): log every guest write into [va, va+bytes) with the host instruction
 * address, as black_native+0x<offset> for black_native.map. A guard page on the host view of the page(s) traps the
 * access; single-step re-arms it after the instruction. Reads are trapped too but not logged.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

extern ptrdiff_t g_xbox_mem_offset;

static uintptr_t s_lo, s_hi, s_page_lo, s_page_hi;
static volatile LONG s_logged;
static DWORD s_tls = TLS_OUT_OF_INDEXES;

static void arm(void)
{
    DWORD old;
    VirtualProtect((void *)s_page_lo, s_page_hi - s_page_lo, PAGE_READWRITE | PAGE_GUARD, &old);
}

static LONG CALLBACK watch_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION && er->NumberParameters >= 2) {
        uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
        if (a < s_page_lo || a >= s_page_hi)
            return EXCEPTION_CONTINUE_SEARCH;
        if (er->ExceptionInformation[0] == 1 && a >= s_lo && a < s_hi && InterlockedIncrement(&s_logged) <= 400) {
            uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
            fprintf(stderr, "[WATCH] write %08X rip black_native+0x%llX thread %lu\n",
                    (unsigned)(a - (uintptr_t)g_xbox_mem_offset),
                    (unsigned long long)((uintptr_t)ep->ContextRecord->Rip - base), GetCurrentThreadId());
        }
        TlsSetValue(s_tls, (void *)1);
        ep->ContextRecord->EFlags |= 0x100;            /* single-step, then re-arm */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode == STATUS_SINGLE_STEP && TlsGetValue(s_tls)) {
        TlsSetValue(s_tls, NULL);
        arm();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void black_watch_init(void)
{
    const char *e = getenv("BLACK_WATCH");
    uint32_t va, bytes = 4;
    char *end;
    if (!e || !*e)
        return;
    va = (uint32_t)strtoul(e, &end, 16);
    if (*end == ':')
        bytes = (uint32_t)strtoul(end + 1, NULL, 0);
    s_lo = (uintptr_t)va + (uintptr_t)g_xbox_mem_offset;
    s_hi = s_lo + bytes;
    s_page_lo = s_lo & ~(uintptr_t)0xFFF;
    s_page_hi = (s_hi + 0xFFF) & ~(uintptr_t)0xFFF;
    s_tls = TlsAlloc();
    AddVectoredExceptionHandler(1, watch_handler);
    arm();
    fprintf(stderr, "[WATCH] guest %08X..%08X (host %p)\n", va, va + bytes, (void *)s_lo);
}
