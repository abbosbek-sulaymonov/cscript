/* opcode.h — the VM instruction set.
 *
 * The list is declared once, as an X-macro, and used to generate the enum here
 * and the computed-goto dispatch table in vm.c. Keeping them in one place is
 * what makes it impossible for the table to drift out of step with the enum —
 * a mismatch there would jump to the wrong handler rather than fail to build.
 *
 * Operands are inline bytes following the opcode, except constant-pool indices
 * and inline-cache indices, which are two bytes. One byte capped a function at 256 literals — reachable
 * by an ordinary file — and the extra byte costs one more read on instructions
 * that were already touching memory for the constant itself.
 *
 * Adding an opcode still means adding a case to the disassembler in debug.c,
 * which the compiler will not catch for you.
 */
#ifndef CSCRIPT_OPCODE_H
#define CSCRIPT_OPCODE_H

/* clang-format off */
#define CS_OPCODE_LIST(X)                                                     \
  X(OP_CONSTANT)          /* [const16]  push constants[const16]                */ \
  X(OP_NULL)              /*          push null                            */ \
  X(OP_UNDEFINED)         /*          push undefined                       */ \
  X(OP_TRUE)                                                                  \
  X(OP_FALSE)                                                                 \
                                                                              \
  X(OP_POP)               /*          discard top                          */ \
  X(OP_POP_N)             /* [count]  discard `count` values               */ \
  X(OP_DUP)               /*          duplicate top                        */ \
  X(OP_DUP2)              /*          duplicate the top two, in order       */ \
  X(OP_POP_UNDER)         /*          discard the value beneath the top     */ \
                                                                              \
  /* Variables. Locals live in stack slots resolved at compile time; globals  \
   * go through a hash lookup, which is why locals are the fast path. */       \
  X(OP_DEFINE_GLOBAL)     /* [const16]  pop, bind constants[const16] -> value   */ \
  X(OP_DEFINE_CONST)      /* [const16]  same, and refuse later assignment     */ \
  X(OP_GET_GLOBAL)        /* [const16][cache16]  push that global          */ \
  X(OP_SET_GLOBAL)        /* [const16][cache16]  assign top, leaving it   */ \
  X(OP_GET_LOCAL)         /* [slot]   push stack[slot]                      */ \
  X(OP_SET_LOCAL)         /* [slot]   stack[slot] = top, leaving it         */ \
  /* In-place ++/-- on a local whose result is discarded. One instruction     \
   * instead of the six a general update needs, and no stack traffic. */       \
  X(OP_INC_LOCAL)         /* [slot]   stack[slot] += 1                      */ \
  X(OP_DEC_LOCAL)         /* [slot]   stack[slot] -= 1                      */ \
  /* Superinstructions. An assignment used as a statement stores and then       \
   * discards, which profiling showed to be 14% of all instructions executed.   \
   * Fusing the pair halves the dispatches and skips the write-back of a value  \
   * nothing reads. */                                                          \
  X(OP_SET_LOCAL_POP)     /* [slot]   stack[slot] = pop()                   */ \
  X(OP_SET_GLOBAL_POP)    /* [const16][cache16]  global = pop()            */ \
  /* A local followed by a literal is 14-18% of all instructions in loop-heavy \
   * code — `i < n`, `i % 7`, `total + 1` all start this way. */                \
  X(OP_GET_LOCAL_CONST)   /* [slot][const16]  push both                       */ \
  /* Two locals feeding one binary operator, which the pair profile puts at    \
   * 7.5% of everything a class-heavy program executes. */                      \
  X(OP_GET_LOCAL_LOCAL)   /* [slotA][slotB]  push both                      */ \
                                                                              \
  X(OP_GET_PROPERTY)      /* [const16][cache16]  pop object, push property */ \
  X(OP_SET_PROPERTY)      /* [const16][cache16]  obj.name = value, leaves it */ \
  /* Assigning to a property in statement position: the value nothing reads     \
   * never has to be written back. 8.5% of the instructions in `bench/classes`  \
   * were an OP_SET_PROPERTY immediately followed by an OP_POP. */              \
  X(OP_SET_PROPERTY_POP)  /* [const16][cache16]  obj.name = pop()           */ \
  /* `this.x` and `local.x`, the most frequent pair in class-heavy code. */     \
  X(OP_GET_LOCAL_PROPERTY) /* [slot][const16][cache16]  push local.name     */ \
  X(OP_GET_INDEX)         /*          pop index and target, push element    */ \
  /* for...of support: replaces the value on top with its element count, so a  \
   * loop can leave [index, length] ready for a fused compare-and-branch. Only \
   * arrays and strings are iterable today, which is why this is one opcode    \
   * rather than an iterator protocol. */                                       \
  X(OP_ITER_LENGTH)       /*          replace top with its length           */ \
  /* One step of `for...of`: pops the iterable and the index, and either      \
   * pushes the next element or jumps past the loop.                          \
   *                                                                          \
   * One instruction rather than the six it replaces, because a generator     \
   * cannot be driven by an index at all — asking it for its length would mean \
   * running it to the end first, which is wrong for an infinite one and wrong \
   * for the order side effects happen in. Arrays keep a bounds check and a    \
   * load, and lose four dispatches. */                                        \
  /* `const [a, b] = something`. An array or a string is already indexable and \
   * is left alone; anything that offers `Symbol.iterator` is pulled from,      \
   * exactly as far as the pattern reaches — so destructuring an endless        \
   * sequence takes what it asked for and stops. `count` is 255 when a rest     \
   * element means "and the remainder". */                                      \
  X(OP_DESTRUCTURE_PREPARE) /* [count] */                                       \
  X(OP_ITER_STEP)         /* [hi][lo] pop index and iterable, push the next  \
                           *          element, or jump when there is none    */ \
                                                                              \
  /* `for await` over an async generator, which cannot be driven by an index: \
   * `next()` answers with a promise, so the loop has to await before it can  \
   * even know whether there is another value. The three below are the second \
   * shape of the loop, chosen at run time by the first of them. */            \
  X(OP_JUMP_IF_ASYNC_ITER) /* [hi][lo] jump if top is an async generator      */ \
  X(OP_ASYNC_NEXT)        /*          pop it, push the promise from next()   */ \
  X(OP_ITER_UNPACK)       /* [hi][lo] pop {value,done}; push value, or jump  */ \
  /* `for...in` desugars to `for...of` over this: an object's own keys, or an  \
   * array's indices as strings, which is what JavaScript enumerates. */        \
  X(OP_ENUM_KEYS)         /*          replace top with its keys as an array */ \
  /* `for...of` over a Map or Set. Arrays and strings are already iterable by  \
   * index, so this leaves them alone and only unpacks the collections that    \
   * are not — which keeps the loop itself one shape. */                        \
  /* Turns whatever a `for...of` was given into something the loop can pull    \
   * from: a Map or Set becomes its entries, and an object that offers          \
   * `Symbol.iterator` becomes what that hands back. The flag says the loop is  \
   * a `for await`, which asks for `Symbol.asyncIterator` first. */             \
  X(OP_ITER_PREPARE)      /* [forAwait] */                                      \
  /* A regex literal. Compiled at run time rather than baked into the constant \
   * pool, because a fresh object is needed per evaluation: `lastIndex` is     \
   * mutable state that two evaluations of the same literal must not share. */  \
  X(OP_REGEX)             /* [const16][const16]  source, flags             */ \
  X(OP_SET_INDEX)         /*          target[index] = value, leaving value  */ \
  X(OP_OBJECT)            /* [count]  build from `count` key/value pairs    */ \
  X(OP_ARRAY)             /* [count]  build from the top `count` values     */ \
  /* Spread. OP_ARRAY_SPREAD builds an array from `count` stack values where    \
   * any of them may itself be an array to splice in; the marker distinguishes  \
   * a spread element from a plain one at run time. */                           \
  X(OP_SPREAD_MARK)       /*          tag the value on top as spread        */ \
  X(OP_ARRAY_SPREAD)      /* [count]  build, splicing any marked elements   */ \
  /* Slices `count` elements off an array from a starting index — the rest      \
   * element of an array pattern. */                                            \
  /* Object literals and patterns that mention `...`. A literal with a spread  \
   * is built up one entry at a time rather than from a run of stack pairs,    \
   * because `{ ...a, b: 1, ...c }` has to apply them in order — a later key   \
   * wins over an earlier spread, and a later spread wins over both. */         \
  /* Private fields. A separate pair of instructions rather than a name the    \
   * ordinary ones would accept, because the storage is separate: they never    \
   * take a shape slot, so nothing that walks an object's properties can see    \
   * them. */                                                                   \
  X(OP_GET_PRIVATE)       /* [const16] pop object, push its private field     */ \
  X(OP_SET_PRIVATE)       /* [const16] pop value and object, push the value   */ \
  X(OP_DELETE_PROPERTY)   /* [const16] pop object, push whether it had it    */ \
  X(OP_DELETE_INDEX)      /*          pop key and object, same answer        */ \
  X(OP_OBJECT_SET)        /*          pop value and key, set on the object    */ \
  /* `{ get x() { … } }`. The accessor goes on a class the object gets for      \
   * itself, which is where the property paths already look for one — so an     \
   * object without accessors pays nothing at all for these existing. */         \
  X(OP_OBJECT_ACCESSOR)   /* [isGetter] pop a closure and a name             */ \
  X(OP_OBJECT_MERGE)      /*          pop a source, copy its keys in          */ \
  X(OP_SET_PROTOTYPE)     /*          pop a value, link it as the prototype   */ \
  X(OP_OBJECT_REST)       /* [count]  pop `count` keys and a source, push the \
                           *          rest of it as a new object              */ \
  /* `tag`a${x}b`` — hangs the unescaped pieces off the cooked ones as `raw`,   \
   * which is the only thing `String.raw` reads. Two arrays go in, one comes    \
   * out. */                                                                    \
  X(OP_TEMPLATE_STRINGS)                                                        \
  X(OP_ARRAY_REST)        /* [index]  push a copy from `index` onward       */ \
  /* A call whose arguments were spread: they arrive as one array, because      \
   * their number is only known at run time. Unpacks it and calls. */            \
  X(OP_CALL_SPREAD)       /*          call with the argument array on top    */ \
  X(OP_CALL)              /* [argc]   call the value below the arguments    */ \
  /* Method call: looks the name up on the receiver and calls it in one step.  \
   * The receiver stays on the stack below the arguments, so no bound-method   \
   * object has to be allocated per call. */                                    \
  X(OP_INVOKE)            /* [const16][argc]  receiver.name(args...)        */ \
  X(OP_INVOKE_INDEX)      /* [argc]   receiver[key](args...)                 */ \
                                                                              \
  /* Classes. A class is built once, at the point of declaration: OP_CLASS      \
   * makes it, OP_INHERIT links it to its base, and the four member opcodes     \
   * hang closures off it while it sits on the stack.                           \
   *                                                                            \
   * Instances are ordinary objects with a class pointer, so fields go in slots \
   * and every inline cache already works on them. A method that misses the     \
   * shape falls back to the class chain. */                                    \
  X(OP_CLASS)             /* [const16]  push a new class                    */ \
  X(OP_INHERIT)           /*          link peek(0) to peek(1), pop it       */ \
  X(OP_METHOD)            /* [const16]  pop a closure onto the class        */ \
  X(OP_STATIC_METHOD)     /* [const16]  ... onto its statics                */ \
  X(OP_STATIC_FIELD)      /* [const16]  pop a value onto its statics        */ \
  X(OP_GETTER)            /* [const16]  pop a closure onto its getters      */ \
  X(OP_SETTER)            /* [const16]  pop a closure onto its setters      */ \
  /* The same five, with the name taken from the stack rather than the         \
   * constant pool — `class C { [key]() {} }` does not know it until the class \
   * is being built. `kind` picks which table. */                              \
  X(OP_CLASS_MEMBER)      /* [kind]  pop a value and a name onto the class    */ \
  X(OP_CONSTRUCTOR)       /*          pop a closure as the constructor      */ \
  /* The field initialisers, kept apart from the constructor so a class with    \
   * no constructor still runs them and a subclass never has to. */             \
  X(OP_FIELD_INIT)        /*          pop a closure as the field initialiser*/ \
  X(OP_NEW)               /* [argc]   construct, leaving the instance       */ \
  /* `super` is reached through a hidden local holding the superclass, which a  \
   * method captures as an upvalue — so it is resolved lexically, by the class  \
   * the method was written in rather than the class of the receiver. */        \
  X(OP_GET_SUPER)         /* [const16]  bind a superclass method            */ \
  X(OP_SUPER_INVOKE)      /* [const16][argc]  super.name(args...)           */ \
  X(OP_SUPER_CALL)        /* [argc]   run the superclass constructor        */ \
  X(OP_IN)                /*          pop key and target, push membership   */ \
  X(OP_INSTANCEOF)        /*          pop class and value, push a boolean   */ \
                                                                              \
  /* Modules. An import reads through to the exporting module's global rather   \
   * than copying it, so a binding stays live and nothing has to be written     \
   * back when a module finishes running. The module itself is a constant in    \
   * the importing chunk, resolved when it was compiled. */                     \
  X(OP_IMPORT_NAME)       /* [const16]  pop a module, push that export      */ \
  X(OP_IMPORT_NAMESPACE)  /*          pop a module, push all its exports    */ \
                                                                              \
  /* Suspends the running fiber until the promise on top settles, then leaves  \
   * its value in the promise's place — or throws, if it rejected. Only ever   \
   * emitted inside an async function, which is the only thing that has a      \
   * fiber to suspend. */                                                       \
  /* `yield v`. Hands the value straight back to whoever called `next`, and    \
   * leaves the frame exactly where it is: the fiber's stack is its own, so     \
   * nothing has to be copied to pause here. What `next(x)` passes back arrives \
   * in the yielded value's place, which is why this leaves one value behind.  */ \
  X(OP_YIELD)                                                                   \
  X(OP_AWAIT)             /*          pop a promise, resume with its value  */ \
                                                                              \
  /* Closures. OP_CLOSURE is followed by one (isLocal, index) pair per         \
   * upvalue, describing where each capture comes from. */                     \
  X(OP_CLOSURE)           /* [const16] then 2 bytes per upvalue               */ \
  X(OP_GET_UPVALUE)       /* [slot]   push the captured variable            */ \
  X(OP_SET_UPVALUE)       /* [slot]   assign to it, leaving the value       */ \
  X(OP_CLOSE_UPVALUE)     /*          move the top local off the stack      */ \
                                                                              \
  X(OP_ADD)               /* numeric add, or string concat if either is a string */ \
  /* Emitted only where the type checker proved both operands are numbers, so \
   * it needs no type dispatch at all. This is the first place a type          \
   * annotation changes the code that runs rather than only what compiles. */  \
  X(OP_ADD_NUM)           /* both operands statically number                */ \
  X(OP_SUBTRACT)                                                              \
  X(OP_MULTIPLY)                                                              \
  X(OP_DIVIDE)                                                                \
  X(OP_MODULO)                                                                \
  X(OP_EXPONENT)          /* **, right-associative                          */ \
  X(OP_NEGATE)                                                                \
                                                                              \
  X(OP_NOT)               /* logical negation, using JS truthiness         */ \
  X(OP_TYPEOF)                                                                \
                                                                              \
  X(OP_EQUAL)             /* === — CScript has no coercing equality        */ \
  X(OP_NOT_EQUAL)         /* !==                                           */ \
  X(OP_GREATER)                                                               \
  X(OP_GREATER_EQUAL)                                                         \
  X(OP_LESS)                                                                  \
  X(OP_LESS_EQUAL)                                                            \
                                                                              \
  X(OP_JUMP)              /* [hi][lo] jump forward unconditionally         */ \
  X(OP_JUMP_IF_FALSE)     /* [hi][lo] jump if top is falsy, leaving it      */ \
  X(OP_JUMP_IF_TRUE)      /* [hi][lo] jump if top is truthy, leaving it     */ \
  X(OP_POP_JUMP_IF_FALSE) /* [hi][lo] pop, then jump if it was falsy        */ \
                                                                              \
  /* Nullish tests, for `??` and `?.`. Neither is a truthiness test: 0 and ""  \
   * are falsy but present, and the whole point of both operators is to tell   \
   * the difference. */                                                        \
  X(OP_JUMP_IF_NULLISH)     /* [hi][lo] jump if top is null/undefined, leaving it */ \
  X(OP_JUMP_IF_NOT_NULLISH) /* [hi][lo] jump if it is not, leaving it        */ \
  /* `o.m?.()`. Asks the receiver on top whether it has the method, without    \
   * producing it: a built-in lives in a per-type table that OP_GET_PROPERTY   \
   * does not read, so testing the value would answer "absent" for every       \
   * `xs.map?.()`. The receiver is left in place for the invoke. */            \
  X(OP_JUMP_IF_NO_METHOD) /* [const16][hi][lo] */                              \
  X(OP_LOOP)              /* [hi][lo] jump backward                         */ \
                                                                              \
  /* Fused compare-and-branch. A loop condition otherwise materialises a       \
   * boolean only to pop it one instruction later; these test the operands     \
   * directly and jump when the comparison is false. Each is the negation of   \
   * its name, because the jump is taken when the branch is *not* entered. */  \
  X(OP_JUMP_IF_NOT_LESS)          /* [hi][lo] */                              \
  X(OP_JUMP_IF_NOT_LESS_EQUAL)    /* [hi][lo] */                              \
  X(OP_JUMP_IF_NOT_GREATER)       /* [hi][lo] */                              \
  X(OP_JUMP_IF_NOT_GREATER_EQUAL) /* [hi][lo] */                              \
  X(OP_JUMP_IF_NOT_EQUAL)         /* [hi][lo] */                              \
  X(OP_JUMP_IF_EQUAL)             /* [hi][lo] */                              \
                                                                              \
  /* Exception handling. OP_TRY installs a handler recording where to resume,   \
   * how deep the frame stack was and how deep the value stack was; unwinding   \
   * restores all three, which is what lets a throw cross call boundaries.      \
   * OP_END_TRY removes it again on the normal path. */                          \
  X(OP_TRY)               /* [hi][lo] install a handler at this offset      */ \
  X(OP_END_TRY)           /*          remove the innermost handler          */ \
  X(OP_THROW)             /*          unwind to the innermost handler       */ \
                                                                              \
  X(OP_RETURN)
/* clang-format on */

typedef enum {
#define CS_DECLARE_OPCODE(name) name,
  CS_OPCODE_LIST(CS_DECLARE_OPCODE)
#undef CS_DECLARE_OPCODE
      OP_COUNT /* number of opcodes; not itself an instruction */
} OpCode;

/* Name of an opcode, generated from the same list as the enum. */
const char *csOpcodeName(OpCode opcode);

#endif /* CSCRIPT_OPCODE_H */
