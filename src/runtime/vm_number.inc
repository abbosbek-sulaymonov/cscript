/* vm_number.inc — errors, and the arithmetic that has to be exact.
 *
 * A BigInt on either side takes the exact path, because that is what a BigInt
 * is for; mixing one with a number is an error rather than a coercion, so the
 * boundary is checked rather than crossed.
 *
 * Included by vm.c rather than compiled on its own, and that is a measured
 * decision rather than a habit: these are the interpreter's hot helpers, and
 * the compiler inlines and specialises them into the dispatch loop. Splitting
 * them into translation units of their own cost 13-49% on the interpreter
 * benchmarks — 49% on bench/branches.cx — because cross-unit calls cannot be
 * inlined. One unit, several files.
 */
/* clang-format off */
void csVMRuntimeError(const char *format, ...) {
  /* stdout is block-buffered to a pipe and stderr is not, so without this the
   * error would jump ahead of output the program already produced. */
  fflush(stdout);

  va_list args;
  va_start(args, format);
  fprintf(stderr, "cscript: runtime error: ");
  vfprintf(stderr, format, args);
  va_end(args);
  fprintf(stderr, "\n");

  /* Walk the frames innermost-first so the trace reads like a call stack. */
  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame *frame = &vm.frames[i];
    ObjFunction *function = frame->closure->function;
    /* `ip` has already advanced past the failing instruction. */
    size_t instruction = (size_t)(frame->ip - function->chunk.code - 1);
    int line = function->chunk.lines[instruction];

    const char *source = frameSourceName(frame);
    if (function->name == NULL) {
      fprintf(stderr, "  at %s:%d\n", source, line);
    } else {
      fprintf(stderr, "  at %s (%s:%d)\n", function->name->chars, source, line);
    }
  }

  resetStack();
}

/* JS `+`: if either side is a string, both are stringified and concatenated;
 * otherwise both are coerced to numbers and added. */
/* --- BigInt arithmetic -----------------------------------------------------
 *
 * JavaScript keeps BigInt and Number strictly apart under the arithmetic
 * operators: `1n + 1` is a TypeError. That looks unhelpful until you see what
 * the alternative costs — widening the BigInt to a double would silently throw
 * away exactly the precision it was created to keep, and narrowing the number
 * would have to invent an answer for `1n + 0.5`. Refusing is the only choice
 * that cannot be wrong. Ordering is different, and mixes freely: `1n < 2` has
 * one right answer, and comparingExactly() finds it without rounding.
 */

/* Replaces the top two stack slots with the result of `op` applied to them.
 * `symbol` is the operator as written, for the error. */
static bool bigintArithmetic(char op, const char *symbol) {
  Value b = peekStack(0);
  Value a = peekStack(1);

  if (!IS_BIGINT(a) || !IS_BIGINT(b)) {
    csVMRuntimeError("cannot mix BigInt and %s in '%s'",
                     csValueTypeName(IS_BIGINT(a) ? b : a), symbol);
    return false;
  }

  const BigInt *left = &AS_BIGINT(a)->value;
  const BigInt *right = &AS_BIGINT(b)->value;

  BigInt result;
  csBigInit(&result);
  bool computed = false;
  switch (op) {
    case '+': computed = csBigAdd(&result, left, right); break;
    case '-': computed = csBigSubtract(&result, left, right); break;
    case '*': computed = csBigMultiply(&result, left, right); break;
    case '/':
    case '%':
      if (csBigIsZero(right)) {
        csBigFree(&result);
        csVMRuntimeError("division of a BigInt by zero");
        return false;
      }
      computed = op == '/' ? csBigDivide(&result, left, right)
                           : csBigRemainder(&result, left, right);
      break;
    case 'p':
      if (right->negative) {
        csBigFree(&result);
        csVMRuntimeError("a BigInt cannot be raised to a negative power");
        return false;
      }
      computed = csBigPower(&result, left, right);
      break;
    default: break;
  }

  if (!computed) {
    csBigFree(&result);
    csVMRuntimeError("out of memory in BigInt arithmetic");
    return false;
  }

  /* Allocated while both operands are still on the stack, so a collection
   * triggered here cannot free the limbs being read. */
  ObjBigInt *object = csBigIntNew(result);
  vm.stackTop -= 2;
  csVMPush(OBJ_VAL(object));
  return true;
}

typedef enum {
  ORDER_KNOWN,     /* `*order` holds it */
  ORDER_UNORDERED, /* NaN was involved: every comparison is false */
  ORDER_INVALID,   /* not comparable at all; an error has been reported */
} OrderResult;

/* Orders two values when at least one is a BigInt, without rounding either. */
static OrderResult comparingExactly(Value a, Value b, int *order) {
  if (IS_BIGINT(a) && IS_BIGINT(b)) {
    *order = csBigCompare(&AS_BIGINT(a)->value, &AS_BIGINT(b)->value);
    return ORDER_KNOWN;
  }
  if (IS_BIGINT(a) && IS_NUMBER(b)) {
    *order = csBigCompareDouble(&AS_BIGINT(a)->value, AS_NUMBER(b));
    return *order == 2 ? ORDER_UNORDERED : ORDER_KNOWN;
  }
  if (IS_NUMBER(a) && IS_BIGINT(b)) {
    int reversed = csBigCompareDouble(&AS_BIGINT(b)->value, AS_NUMBER(a));
    if (reversed == 2) return ORDER_UNORDERED;
    *order = -reversed;
    return ORDER_KNOWN;
  }

  csVMRuntimeError("cannot compare %s with %s", csValueTypeName(a),
                   csValueTypeName(b));
  return ORDER_INVALID;
}
