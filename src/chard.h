/* chard.h -- auto-generated umbrella header. Includes shared types,
 * shared globals, and every module's public function declarations,
 * so any .c file under src/<folder>/ can just #include "../chard.h". */
#ifndef CHARD_H
#define CHARD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <setjmp.h>
#include <limits.h>
#include <stdarg.h>

#include "chard_types.h"
#include "chard_globals.h"

#include "core/errors/errors.h"
#include "core/util/util.h"
#include "core/decls/decls.h"
#include "parse/validation/pins/pins.h"
#include "parse/validation/externs/externs.h"
#include "frontend/syntax/stmts/stmts.h"
#include "frontend/symbols/macros/macros.h"
#include "frontend/symbols/symtab/symtab.h"
#include "frontend/lexical/lexer/lexer.h"
#include "frontend/lexical/regs/regs.h"
#include "frontend/lexical/numlit/numlit.h"
#include "frontend/syntax/expr/expr.h"
#include "frontend/syntax/operands/operands.h"
#include "parse/instructions/iprog/iprog.h"
#include "parse/control_flow/frames/frames.h"
#include "parse/instructions/decl/decl.h"
#include "parse/control_flow/cond/cond.h"
#include "parse/instructions/instr/instr.h"
#include "preprocess/pp-lines/pp-lines.h"
#include "preprocess/sys-include/sys-include.h"
#include "preprocess/directives/directives.h"
#include "preprocess/file/file.h"
#include "backend/x86_64/x86_64.h"
#include "backend/aarch64/aarch64.h"
#include "backend/riscv/riscv.h"
#include "driver/driver.h"

#endif /* CHARD_H */
