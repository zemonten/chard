/* instr.h -- auto-generated declarations for the 'instr_line' module
 * (5 function(s), defined in instr.c). */
#ifndef CHARD_MOD_INSTR_H
#define CHARD_MOD_INSTR_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

int parse_fused_store(const char *raw_trimmed);
int syscall_num_for_name(const char *tok, long *out);
void emit_array_bounds_check(operand_t idx_reg, int len);
void parse_libc_call_body(const char *name_start, const char *kw_for_errors);
int parse_instr_line(char *tokens[], int ntok, const char *raw_trimmed);

#endif /* CHARD_MOD_INSTR_H */
