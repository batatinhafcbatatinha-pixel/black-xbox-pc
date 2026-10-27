/* Strict unresolved-code policy: never turn missing instructions into success. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"
#include "recovered_indirect.h"

recomp_func_t black_recovered_funcs_lookup(uint32_t va);   /* scripts/recover_missing_functions.py */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    recomp_func_t fn = black_recovered_lookup(va);
    return fn ? fn : black_recovered_funcs_lookup(va);
}
void recomp_icall_fail_log(uint32_t va) {
    fprintf(stderr, "Black: untranslated indirect target 0x%08X\n", va);
    abort();
}
void recomp_icall_not_code_log(uint32_t va) {
    fprintf(stderr, "Black: non-code indirect target 0x%08X\n", va);
    abort();
}
void recomp_unimpl(const char *text, uint32_t va) {
    fprintf(stderr, "Black: unsupported instruction %s at 0x%08X\n", text, va);
    abort();
}
