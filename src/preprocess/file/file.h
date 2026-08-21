/* file.h -- auto-generated declarations for the 'file_parse' module
 * (3 function(s), defined in file.c). */
#ifndef CHARD_MOD_FILE_H
#define CHARD_MOD_FILE_H

#include "../../chard_types.h"
#include "../../chard_globals.h"

void collect_source_and_macro_defs(FILE *f);
void parse__file(FILE *f);
void apply_entry_symbol_override(void);

#endif /* CHARD_MOD_FILE_H */
