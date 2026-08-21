/* directives.h -- auto-generated declarations for the 'directives' module
 * (8 function(s), defined in directives.c). */
#ifndef CHARD_MOD_DIRECTIVES_H
#define CHARD_MOD_DIRECTIVES_H

#include "../../chard_types.h"
#include "../../chard_globals.h"

void do_entry_symbol_directive(const char *t);
void do_mode_directive(const char *t);
void do_foot_directive(const char *t);
void do_heap_size_directive(const char *t);
void do_init_scratch_directive(const char *t);
void do_finit_scratch_directive(const char *t);
void do_alias_directive(const char *t, int is_float);
void do_argv_directive(const char *t);

#endif /* CHARD_MOD_DIRECTIVES_H */
