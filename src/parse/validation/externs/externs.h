/* externs.h -- auto-generated declarations for the 'externs' module
 * (3 function(s), defined in externs.c). */
#ifndef CHARD_MOD_EXTERNS_H
#define CHARD_MOD_EXTERNS_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

void note_extern_lib(const char *lib);
extern_sig_t *find_extern(const char *name);
extern_sig_t *ensure_libc_heap_extern(const char *name, int nargs);

#endif /* CHARD_MOD_EXTERNS_H */
