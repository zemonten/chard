/* operands.h -- auto-generated declarations for the 'operands' module
 * (6 function(s), defined in operands.c). */
#ifndef CHARD_MOD_OPERANDS_H
#define CHARD_MOD_OPERANDS_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

int is_mem_operand(opnd_kind_t k);
void parse__xaddr(const char *mnemonic, const char *raw_expr,
                          operand_t *out_idx, int *out_scale, long *out_disp);
void parse__operand(const char *tok, operand_t *out);
void render_simple_operand(target_t t, operand_t *o, char *buf, size_t bufsz);
void x86_addr_text(FILE *out, const operand_t *o, char *buf, size_t bufsz);
int operand_mem_size(const operand_t *o);

#endif /* CHARD_MOD_OPERANDS_H */
