#include "../../chard.h"

decl_t *find__decl(const char *name) {
    for (int i = 0; i < ndecls; i++)
        if (strcmp(decls[i].name, name) == 0) return &decls[i];
    return NULL;
}

func_sig_t *find_func_sig(const char *name) {
    for (int i = 0; i < nfunc_sigs; i++)
        if (strcmp(func_sigs[i].name, name) == 0) return &func_sigs[i];
    return NULL;
}

