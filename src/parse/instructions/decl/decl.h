/* decl.h -- auto-generated declarations for the 'decl_parse' module
 * (3 function(s), defined in decl.c). */
#ifndef CHARD_MOD_DECL_H
#define CHARD_MOD_DECL_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

int parse__decl(char *tokens[], int ntok, char *raw_line);
int two_op_has_comma_form(opcode_t op);
int is_symbolic_op_name(const char *name);

#endif /* CHARD_MOD_DECL_H */
