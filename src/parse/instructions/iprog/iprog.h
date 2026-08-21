/* iprog.h -- auto-generated declarations for the 'instr_list' module
 * (3 function(s), defined in iprog.c). */
#ifndef CHARD_MOD_IPROG_H
#define CHARD_MOD_IPROG_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

void push__instr(instr_t i);
void insert_instrs_at(int at, const instr_t *instrs, int n);
void delete_instr_at(int at);

#endif /* CHARD_MOD_IPROG_H */
