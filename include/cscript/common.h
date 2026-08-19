/* common.h — shared types, version and tunables for the whole interpreter. */
#ifndef CSCRIPT_COMMON_H
#define CSCRIPT_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CS_VERSION_MAJOR 0
#define CS_VERSION_MINOR 8
#define CS_VERSION_PATCH 1
#define CS_VERSION_STRING "0.8.1"

/* Value stack depth, shared by every active call frame. */
#define CS_STACK_MAX (64 * 256)

/* Heap growth factor: collect once allocation doubles since the last GC. */
#define CS_GC_HEAP_GROW_FACTOR 2

/* Debug switches are set by the build (make debug / make trace), never here.
 *   CS_DEBUG_PRINT_TOKENS     dump the token stream
 *   CS_DEBUG_PRINT_AST        dump the parse tree
 *   CS_DEBUG_PRINT_CODE       disassemble each compiled chunk
 *   CS_DEBUG_TRACE_EXECUTION  trace every instruction and the stack
 *   CS_DEBUG_STRESS_GC        collect on every allocation
 *   CS_DEBUG_LOG_GC           log every allocation, mark and sweep
 */

#endif /* CSCRIPT_COMMON_H */
