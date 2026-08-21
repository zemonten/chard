/* x86.h -- auto-generated declarations for the 'backend_x86' module
 * (4 function(s), defined in x86.c). */
#ifndef CHARD_MOD_X86_H
#define CHARD_MOD_X86_H

#include "../../chard_types.h"
#include "../../chard_globals.h"

void emit_x86_load_scratch(FILE *out, const operand_t *o);
void emit_x86_string_bytes(FILE *out, const char *s, int len);
void x86_emit_int_val(FILE *out, long val, const int *is_label, char *const *val_labels, int v, int first);
void emit_x86_64(FILE *out);

#endif /* CHARD_MOD_X86_H */
