/* numlit.h -- auto-generated declarations for the 'numbers' module
 * (4 function(s), defined in numlit.c). */
#ifndef CHARD_MOD_NUMLIT_H
#define CHARD_MOD_NUMLIT_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

long parse__number(const char *tok);
const char *resolve_size_alias(const char *tok);
uint64_t double__bits(double d);
uint32_t float__bits(double d);

#endif /* CHARD_MOD_NUMLIT_H */
