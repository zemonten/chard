/* driver.h -- auto-generated declarations for the 'driver' module
 * (7 function(s), defined in driver.c). */
#ifndef CHARD_MOD_DRIVER_H
#define CHARD_MOD_DRIVER_H

#include "../chard_types.h"
#include "../chard_globals.h"

void usage(const char *prog);
target_t detect_native_target(void);
char *derive_output_path(const char *input_path);
char *derive_ld_script_path(const char *output_path);
void write_bare_mode_linker_script(const char *ld_path);
void check_bare_mode_requirements(target_t target);
int main(int argc, char **argv);

#endif /* CHARD_MOD_DRIVER_H */
