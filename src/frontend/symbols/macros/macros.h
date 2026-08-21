/* macros.h -- auto-generated declarations for the 'macros' module
 * (5 function(s), defined in macros.c). */
#ifndef CHARD_MOD_MACROS_H
#define CHARD_MOD_MACROS_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

macro_def_t *find__macro(const char *name);
void expand_macro_line(const char *line, const macro_def_t *m,
                               char *args[MAX_MACRO_PARAMS], int expand_id,
                               char *out, size_t out_cap);
int split_macro_args(char *arglist, char *args[MAX_MACRO_PARAMS]);
macro_def_t *match_macro_call(const char *line, const char **args_start, const char **args_end);
void expand_all_macro_calls(void);

#endif /* CHARD_MOD_MACROS_H */
