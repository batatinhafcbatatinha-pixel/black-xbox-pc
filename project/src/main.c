/* Black USA AOT bootstrap. No addresses/hooks from another game. */
#ifdef BLACK_NATIVE_HOST
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "native_gpu/black_gpu.h"
#else
#include <xbox/xboxrecomp.h>
#endif
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int recomp_dispatch_init(void);
extern void xbe_entry_point(void);

static int load(const char *path, void **data, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 0; }
    long size = ftell(f);
    if (size != 2813952 || fseek(f, 0, SEEK_SET)) { fclose(f); return 0; }
    *data = malloc((size_t)size);
    if (!*data) { fclose(f); return 0; }
    *bytes = fread(*data, 1, (size_t)size, f);
    fclose(f);
    if (*bytes != (size_t)size || memcmp(*data, "XBEH", 4)) {
        free(*data); *data = NULL; return 0;
    }
    /* Retail entry XOR key, matching the verified source XBE. */
    uint32_t entry;
    memcpy(&entry, (unsigned char *)*data + 0x128, sizeof(entry));
    if ((entry ^ 0xA8FC57ABu) != 0x00025A3Fu) {
        free(*data); *data = NULL; return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    #ifdef BLACK_NATIVE_HOST
    extern void xa2_set_volume(float volume);
    BlackGpuInitOptions(xa2_set_volume);
    #endif
    /* Caller selects a data directory; no copy or extraction is performed. */
    const char *game = argc > 1 ? argv[1] : "game_files";
    char xbe[MAX_PATH];
    void *data = NULL;
    size_t bytes = 0;
    if (snprintf(xbe, sizeof(xbe), "%s\\default.xbe", game) >= (int)sizeof(xbe)
            || !load(xbe, &data, &bytes)) {
        fprintf(stderr, "Black USA default.xbe missing or wrong identity: %s\n", game);
        return 2;
    }
#ifdef BLACK_NATIVE_HOST
    xbox_SetNoHardware(1);
#endif
    if (!xbox_MemoryLayoutInit(data, bytes)) {
        fprintf(stderr, "Cannot initialize Black memory layout\n"); free(data); return 3;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    {
        extern void black_watch_init(void);
        black_watch_init();
    }
#ifdef BLACK_NATIVE_HOST
    extern void black_key_event(int key, int down);
    if (!BlackGpuAttachMemory(g_xbox_mem_offset, black_key_event)) {
        fprintf(stderr, "Cannot attach native GPU memory\n");
        xbox_MemoryLayoutShutdown(); free(data); return 5;
    }
#endif
    xbox_kernel_init();
    xbox_path_init(game, NULL);
    xbox_kernel_set_thunk_address(0x00282FE0u, 115);
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init()) {
        fprintf(stderr, "Cannot allocate native AOT dispatch\n");
        xbox_kernel_shutdown(); xbox_MemoryLayoutShutdown(); free(data); return 4;
    }
    /* An absent function/instruction is fatal in integration.c. */
    xbe_entry_point();
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(data);
    return 0;
}
