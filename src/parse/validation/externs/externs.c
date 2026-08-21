#include "../../../chard.h"

void note_extern_lib(const char *lib) {
    if (!lib || !*lib) return;
    for (int i = 0; i < g_n_extern_libs; i++)
        if (strcmp(g_extern_libs[i], lib) == 0) return;
    if (g_n_extern_libs >= (int)(sizeof(g_extern_libs) / sizeof(g_extern_libs[0])))
        return; /* silently cap -- this only feeds an advisory build note, not codegen */
    strncpy(g_extern_libs[g_n_extern_libs], lib, MAX_SYMLEN - 1);
    g_extern_libs[g_n_extern_libs][MAX_SYMLEN - 1] = '\0';
    g_n_extern_libs++;
}

extern_sig_t *find_extern(const char *name) {
    for (int i = 0; i < nexterns; i++)
        if (strcmp(externs[i].name, name) == 0) return &externs[i];
    return NULL;
}

extern_sig_t *ensure_libc_heap_extern(const char *name, int nargs) {
    extern_sig_t *sig = find_extern(name);
    if (sig) {
        if (sig->nargs != nargs)
            failf("'%s' is already declared with a different argument count than 'libc-heap-*' needs", name);
        return sig;
    }
    DA_ENSURE(externs, externs_cap, nexterns, extern_sig_t);
    sig = &externs[nexterns++];
    strncpy(sig->name, name, MAX_SYMLEN - 1);
    sig->nargs = nargs;
    return sig;
}

