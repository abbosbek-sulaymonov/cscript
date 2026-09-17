/* vm_ops_arith.inc — arithmetic, comparison, jumps and control flow.
 *
 * Both operands of an arithmetic operation must be numbers. JavaScript would
 * coerce and often produce NaN; an error at the mistake is far easier to debug
 * than a NaN that spreads silently. A BigInt on either side takes the exact
 * path instead, which is what the three macros above this include distinguish.
 *
 * Included inside run() in vm.c, not compiled on its own: every case ends in
 * VM_NEXT(), which under computed-goto dispatch is a jump to a label in that
 * function. A case cannot become a function without giving up the dispatch
 * strategy ARCHITECTURE.md measured and chose — so what moved here is the
 * text, and the loop around it is unchanged.
 */
/* clang-format off */
      VM_CASE(OP_ADD_NUM) {
        /* The checker guaranteed both operands are numbers, so there is
         * nothing to test. */
        Value b = peekStack(0);
        Value a = peekStack(1);
        vm.stackTop -= 2;
        csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
        VM_NEXT();
      }

      VM_CASE(OP_ADD) {
        /* Adding two numbers is overwhelmingly the common case, and routing it
         * through concatenateOrAdd() costs a call plus two string checks. */
        Value b = peekStack(0);
        Value a = peekStack(1);
        if (IS_NUMBER(a) && IS_NUMBER(b)) {
          vm.stackTop -= 2;
          csVMPush(NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b)));
          VM_NEXT();
        }
        if (!concatenateOrAdd()) {
          HANDLE_FAILED_CALL();
          VM_NEXT();
        }
        VM_NEXT();
      }

      VM_CASE(OP_SUBTRACT) BINARY_ARITHMETIC_OP(-, '-'); VM_NEXT();
      VM_CASE(OP_MULTIPLY) BINARY_ARITHMETIC_OP(*, '*'); VM_NEXT();
      VM_CASE(OP_DIVIDE)   BINARY_ARITHMETIC_OP(/, '/'); VM_NEXT();

      VM_CASE(OP_MODULO) {
        if (IS_BIGINT(peekStack(0)) || IS_BIGINT(peekStack(1))) {
          if (!bigintArithmetic('%', "%")) return CS_RUNTIME_ERROR;
          VM_NEXT();
        }
        if (!IS_NUMBER(peekStack(0)) || !IS_NUMBER(peekStack(1))) {
          csVMRuntimeError("operands of '%%' must be numbers, got %s and %s",
                           csValueTypeName(peekStack(1)), csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        double b = AS_NUMBER(csVMPop());
        double a = AS_NUMBER(csVMPop());

        /* fmod() is a libm call and costs roughly 20ns; integer remainder is
         * about 1ns. Loop counters are integers virtually always, so take the
         * integer path whenever it is exactly equivalent.
         *
         * C's % truncates toward zero and so keeps the sign of the dividend,
         * which is what JavaScript's % does too. A zero dividend is excluded so
         * that -0 %% n stays -0 rather than becoming +0. */
        if (b != 0 && a != 0 && isExactInteger(a) && isExactInteger(b)) {
          csVMPush(NUMBER_VAL((double)((int64_t)a % (int64_t)b)));
        } else {
          csVMPush(NUMBER_VAL(fmod(a, b)));
        }
        VM_NEXT();
      }

      VM_CASE(OP_EXPONENT) {
        if (IS_BIGINT(peekStack(0)) || IS_BIGINT(peekStack(1))) {
          if (!bigintArithmetic('p', "**")) return CS_RUNTIME_ERROR;
          VM_NEXT();
        }
        if (!IS_NUMBER(peekStack(0)) || !IS_NUMBER(peekStack(1))) {
          csVMRuntimeError("operands of '**' must be numbers, got %s and %s",
                           csValueTypeName(peekStack(1)), csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        double exponent = AS_NUMBER(csVMPop());
        double base = AS_NUMBER(csVMPop());
        csVMPush(NUMBER_VAL(pow(base, exponent)));
        VM_NEXT();
      }

      VM_CASE(OP_NEGATE)
        if (IS_BIGINT(peekStack(0))) {
          BigInt flipped;
          csBigInit(&flipped);
          if (!csBigNegate(&flipped, &AS_BIGINT(peekStack(0))->value)) {
            csBigFree(&flipped);
            csVMRuntimeError("out of memory negating a BigInt");
            return CS_RUNTIME_ERROR;
          }
          ObjBigInt *negated = csBigIntNew(flipped);
          csVMPop();
          csVMPush(OBJ_VAL(negated));
          VM_NEXT();
        }
        if (!IS_NUMBER(peekStack(0))) {
          csVMRuntimeError("operand of unary '-' must be a number, got %s",
                           csValueTypeName(peekStack(0)));
          return CS_RUNTIME_ERROR;
        }
        csVMPush(NUMBER_VAL(-AS_NUMBER(csVMPop())));
        VM_NEXT();

      VM_CASE(OP_NOT)
        csVMPush(BOOL_VAL(!csValueIsTruthy(csVMPop())));
        VM_NEXT();

      VM_CASE(OP_TYPEOF) {
        const char *name = csValueTypeName(csVMPop());
        csVMPush(OBJ_VAL(csStringCopy(name, (int)strlen(name))));
        VM_NEXT();
      }

      VM_CASE(OP_EQUAL) {
        Value b = csVMPop();
        Value a = csVMPop();
        csVMPush(BOOL_VAL(csValuesStrictEqual(a, b)));
        VM_NEXT();
      }
      VM_CASE(OP_NOT_EQUAL) {
        Value b = csVMPop();
        Value a = csVMPop();
        csVMPush(BOOL_VAL(!csValuesStrictEqual(a, b)));
        VM_NEXT();
      }

      VM_CASE(OP_GREATER)       BINARY_COMPARE_OP(>); VM_NEXT();
      VM_CASE(OP_GREATER_EQUAL) BINARY_COMPARE_OP(>=); VM_NEXT();
      VM_CASE(OP_LESS)          BINARY_COMPARE_OP(<); VM_NEXT();
      VM_CASE(OP_LESS_EQUAL)    BINARY_COMPARE_OP(<=); VM_NEXT();

      VM_CASE(OP_JUMP) {
        uint16_t offset = READ_SHORT();
        frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_FALSE) {
        uint16_t offset = READ_SHORT();
        if (!csValueIsTruthy(peekStack(0))) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_TRUE) {
        uint16_t offset = READ_SHORT();
        if (csValueIsTruthy(peekStack(0))) frame->ip += offset;
        VM_NEXT();
      }
      /* Nullish, not falsy: `0 ?? 1` is 0 and `"" ?? "x"` is "". Getting this
       * wrong would make `??` an alias for `||`, which is the whole reason the
       * operator was added to JavaScript. */
      VM_CASE(OP_JUMP_IF_NULLISH) {
        uint16_t offset = READ_SHORT();
        Value top = peekStack(0);
        if (IS_NULL(top) || IS_UNDEFINED(top)) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_NO_METHOD) {
        ObjString *name = READ_STRING();
        uint16_t offset = READ_SHORT();
        if (!receiverHasMethod(peekStack(0), name)) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_JUMP_IF_NOT_NULLISH) {
        uint16_t offset = READ_SHORT();
        Value top = peekStack(0);
        if (!IS_NULL(top) && !IS_UNDEFINED(top)) frame->ip += offset;
        VM_NEXT();
      }
      VM_CASE(OP_POP_JUMP_IF_FALSE) {
        uint16_t offset = READ_SHORT();
        if (!csValueIsTruthy(csVMPop())) frame->ip += offset;
        VM_NEXT();
      }
/* Fused compare-and-branch. The operand check is the same one the standalone
 * comparison performs, so the error message a program sees does not change. */
#define COMPARE_JUMP(op)                                                       \
  do {                                                                         \
    uint16_t offset = READ_SHORT();                                            \
    Value b = peekStack(0);                                                    \
    Value a = peekStack(1);                                                    \
    if (IS_BIGINT(a) || IS_BIGINT(b)) {                                        \
      int order = 0;                                                           \
      OrderResult ordering = comparingExactly(a, b, &order);                   \
      if (ordering == ORDER_INVALID) return CS_RUNTIME_ERROR;                  \
      vm.stackTop -= 2;                                                        \
      if (!(ordering == ORDER_KNOWN && (order op 0))) frame->ip += offset;     \
      break;                                                                   \
    }                                                                          \
    if (!IS_NUMBER(a) || !IS_NUMBER(b)) {                                      \
      csVMRuntimeError("operands of '" #op "' must be numbers, got %s and %s", \
                       csValueTypeName(a), csValueTypeName(b));                 \
      return CS_RUNTIME_ERROR;                                                 \
    }                                                                          \
    vm.stackTop -= 2;                                                          \
    if (!(AS_NUMBER(a) op AS_NUMBER(b))) frame->ip += offset;                   \
  } while (false)

/* VM_NEXT stays outside the macro. Under computed goto it is a goto, but under
 * switch dispatch it is a `break` — which a do/while(false) would swallow,
 * falling through to the next case instead of dispatching. */
      VM_CASE(OP_JUMP_IF_NOT_LESS) COMPARE_JUMP(<); VM_NEXT();
      VM_CASE(OP_JUMP_IF_NOT_LESS_EQUAL) COMPARE_JUMP(<=); VM_NEXT();
      VM_CASE(OP_JUMP_IF_NOT_GREATER) COMPARE_JUMP(>); VM_NEXT();
      VM_CASE(OP_JUMP_IF_NOT_GREATER_EQUAL) COMPARE_JUMP(>=); VM_NEXT();

      /* Equality accepts any types, so it needs no operand check. */
      VM_CASE(OP_JUMP_IF_NOT_EQUAL) {
        uint16_t offset = READ_SHORT();
        Value b = csVMPop();
        Value a = csVMPop();
        if (!csValuesStrictEqual(a, b)) frame->ip += offset;
        VM_NEXT();
      }

      VM_CASE(OP_JUMP_IF_EQUAL) {
        uint16_t offset = READ_SHORT();
        Value b = csVMPop();
        Value a = csVMPop();
        if (csValuesStrictEqual(a, b)) frame->ip += offset;
        VM_NEXT();
      }

#undef COMPARE_JUMP

      VM_CASE(OP_LOOP) {
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;
        /* A back-edge counts as much as a call: a function called once around
         * a millionfold loop is as hot as one called a million times, and only
         * this catches the first kind. */
        CS_JIT_TICK(frame->closure->function);

#ifdef CS_DEBUG_JIT
        /* And having counted it, this is where the loop is handed over.
         *
         * Compiling at the call site cannot help a loop: the function a
         * back-edge makes hot is already running, and for `heavy` — one call
         * around twenty million iterations — the compiled code was entered
         * exactly zero times. The guard is one load and a compare, paid only
         * on a back-edge, and it fails for every function that never
         * compiled. */
        if (frame->closure->function->jitState == JIT_COMPILED) {
          Value produced;
          int resumeAt = -1;
          int resumeHeight = 0;
          bool compiledFailed = false;
          if (csJitOsr(frame->closure->function,
                       (int)(frame->ip - frame->closure->function->chunk.code),
                       frame->slots, &produced, &resumeAt, &resumeHeight,
                       &compiledFailed)) {
            /* Handed back part-way: the compiled code took the loop and left
             * whatever follows it alone. The frame is already in the shape the
             * interpreter expects — the slots are the same slots — so only the
             * two things the compiled code does not keep have to be put back:
             * where to resume, and how deep the operand stack is. */
            if (resumeAt >= 0) {
              frame->ip = frame->closure->function->chunk.code + resumeAt;
              vm.stackTop = frame->slots + resumeHeight;
              VM_NEXT();
            }

            /* The compiled code ran the function to completion, so what is
             * left is the frame teardown OP_RETURN does — kept in step with
             * it deliberately rather than shared, because the two differ in
             * where the result comes from and in nothing else. */
            while (vm.handlerCount > 0 &&
                   vm.handlers[vm.handlerCount - 1].frameCount >= vm.frameCount) {
              vm.handlerCount--;
            }
            closeUpvalues(frame->slots);
            vm.frameCount--;

            vm.stackTop = frame->slots;
            csVMPush(produced);
            if (vm.frameCount == baseFrame) return CS_OK;
            frame = &vm.frames[vm.frameCount - 1];
          } else if (compiledFailed) {
            /* A call the compiled loop made threw. The loop is over; this
             * frame takes the throw where it stands. */
            HANDLE_FAILED_CALL();
          }
        }
#endif
        VM_NEXT();
      }

      VM_CASE(OP_TRY) {
        uint16_t offset = READ_SHORT();
        if (vm.handlerCount == vm.handlerCapacity) {
          csVMRuntimeError("too many nested 'try' blocks (limit %d)",
                           vm.handlerCapacity);
          return CS_RUNTIME_ERROR;
        }
        ExceptionHandler *handler = &vm.handlers[vm.handlerCount++];
        handler->frameCount = vm.frameCount;
        handler->stackTop = vm.stackTop;
        handler->ip = frame->ip + offset;
        handler->handlerCount = vm.handlerCount - 1;
        VM_NEXT();
      }

      VM_CASE(OP_END_TRY)
        vm.handlerCount--;
        VM_NEXT();

      VM_CASE(OP_THROW) {
        Value thrown = csVMPop();
        switch (performThrow(thrown, baseFrame, &frame)) {
          case THROW_HANDLED:
            VM_NEXT();
          case THROW_PROPAGATE:
            return CS_RUNTIME_ERROR;
          case THROW_UNCAUGHT:
            return uncaught(thrown);
        }
        VM_NEXT();
      }

      VM_CASE(OP_RETURN) {
        Value result = csVMPop();

        /* Returning past a `try` abandons its handler; leaving it installed
         * would send a later throw back into a frame that no longer exists. */
        while (vm.handlerCount > 0 &&
               vm.handlers[vm.handlerCount - 1].frameCount >= vm.frameCount) {
          vm.handlerCount--;
        }

        /* Anything this frame's locals were captured into has to move to the
         * heap before the slots are reused. */
        closeUpvalues(frame->slots);
        vm.frameCount--;

        /* Hand control back to whoever started this loop: the driver at depth
         * 0, or a native that called into user code at some deeper level. The
         * result is left on the stack in place of the callee. */
        if (vm.frameCount == baseFrame) {
          vm.stackTop = frame->slots;
          csVMPush(result);
          return CS_OK;
        }

        /* Discard the whole window, then leave the result where the callee was. */
        vm.stackTop = frame->slots;
        csVMPush(result);
        frame = &vm.frames[vm.frameCount - 1];
        VM_NEXT();
      }

#ifndef CS_COMPUTED_GOTO
      default:
        /* Unreachable: the only producer of bytecode is our own compiler. The
         * computed-goto path has no equivalent guard — an out-of-range byte
         * would index past the table — which is the price of the faster
         * dispatch, and is acceptable because chunks never come from disk. */
        csVMRuntimeError("unknown opcode %d", instruction);
        return CS_RUNTIME_ERROR;
#endif
