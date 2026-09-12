/* compiler_node_value.c — the expressions, from the compiler's node dispatcher.
 *
 * Each leaves exactly one value on the stack, which is the contract the whole
 * stack machine rests on: a statement discards one, a binary operator consumes
 * two and leaves one, and a compiler that emits the wrong number is a bug the
 * disassembler will not show you. Answers false for a node it does not handle,
 * so the dispatcher can ask the statement half instead.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cscript/compiler.h"
#include "cscript/memory.h"
#include "cscript/module.h"
#include "cscript/object.h"
#include "cscript/opcode.h"
#include "cscript/type.h"
#include "cscript/vm.h"
#include "compiler/compiler_internal.h"

bool compileValueNode(const AstNode *node, int line) {
  switch (node->type) {
    case AST_NUMBER_LITERAL: emitConstant(NUMBER_VAL(node->as.number), line); break;

    case AST_STRING_LITERAL: {
      ObjString *string = csStringCopy(node->as.string.chars, node->as.string.length);
      emitConstant(OBJ_VAL(string), line);
      break;
    }

    case AST_BIGINT_LITERAL: {
      /* Parsed once, here, and stored in the constant pool: the digits never
       * have to be read again however often the literal is evaluated. */
      BigInt parsed;
      csBigInit(&parsed);
      if (!csBigFromText(&parsed, node->as.string.chars, node->as.string.length)) {
        csBigFree(&parsed);
        errorAt(node->line, "'%.*s' is not a whole number", node->as.string.length, node->as.string.chars);
        break;
      }
      emitConstant(OBJ_VAL(csBigIntNew(parsed)), line);
      break;
    }

    case AST_BOOL_LITERAL: emitByte(node->as.boolean ? OP_TRUE : OP_FALSE, line); break;

    case AST_NULL_LITERAL: emitByte(OP_NULL, line); break;

    case AST_UNDEFINED_LITERAL: emitByte(OP_UNDEFINED, line); break;

    case AST_IDENTIFIER: compileIdentifierLoad(node->as.identifier.name, node->as.identifier.length, line); break;

    case AST_ASSIGN: compileAssign(node, false); break;

    case AST_UPDATE: compileUpdate(node); break;

    case AST_PROPERTY: {
      /* `this.x` and `local.x` fuse the load of the receiver into the read.
       * The pair profile puts this at 8.5% of a class-heavy program. */
      const AstNode *object = node->as.property.object;

      if (isPrivateName(node->as.property.name, node->as.property.length)) {
        compileNode(object);
        emitConstantOp(OP_GET_PRIVATE, identifierConstant(node->as.property.name, node->as.property.length, line), line);
        break;
      }

      /* The fusion loads the receiver and reads the property in one
       * instruction, which leaves nowhere to test the receiver for `?.`. */
      int slot = object->type == AST_IDENTIFIER && !node->as.property.optional ? resolveLocal(current, object->as.identifier.name, object->as.identifier.length) : -1;
      if (slot != -1) {
        emitByte(OP_GET_LOCAL_PROPERTY, line);
        emitByte((uint8_t)slot, line);
        emitConstantOperand(identifierConstant(node->as.property.name, node->as.property.length, line), line);
        int cache = csChunkAddPropertyCache(currentChunk());
        if (cache > UINT16_MAX) errorAt(line, "too many property sites in one function");
        emitConstantOperand(cache, line);
        break;
      }
      compileNode(object);
      if (node->as.property.optional) emitOptionalGuard(line);
      emitPropertyOp(OP_GET_PROPERTY, identifierConstant(node->as.property.name, node->as.property.length, line), line);
      break;
    }

    case AST_OPTIONAL_CHAIN: compileOptionalChain(node); break;

    case AST_TEMPLATE_STRINGS:
      compileNode(node->as.templateStrings.cooked);
      compileNode(node->as.templateStrings.raw);
      emitByte(OP_TEMPLATE_STRINGS, line);
      break;

    case AST_SEQUENCE:
      /* The first operand is evaluated only for what it does. */
      compileForEffect(node->as.sequence.first);
      compileNode(node->as.sequence.second);
      break;

    case AST_YIELD:
      if (node->as.yield.isDelegate) {
        /* Delegating needs somewhere to keep its position across each
         * suspension, and a local's slot is the stack height it was declared
         * at — which only holds where nothing else is part-way evaluated. */
        errorAt(line,
                "'yield*' is only supported as a statement of its own, "
                "not inside a larger expression");
        break;
      }
      if (node->as.yield.value != NULL) {
        compileNode(node->as.yield.value);
      } else {
        emitByte(OP_UNDEFINED, line);
      }
      emitByte(OP_YIELD, line);
      break;

    case AST_DELETE: {
      const AstNode *target = node->as.deleteTarget;
      if (target->type == AST_PROPERTY) {
        compileNode(target->as.property.object);
        emitConstantOp(OP_DELETE_PROPERTY, identifierConstant(target->as.property.name, target->as.property.length, line), line);
      } else {
        compileNode(target->as.index.target);
        compileNode(target->as.index.index);
        emitByte(OP_DELETE_INDEX, line);
      }
      break;
    }

    case AST_INDEX:
      compileNode(node->as.index.target);
      /* Before the subscript: `a?.[f()]` must not call `f` when `a` is
       * absent. */
      if (node->as.index.optional) emitOptionalGuard(line);
      compileNode(node->as.index.index);
      emitByte(OP_GET_INDEX, line);
      break;

    case AST_OBJECT_LITERAL: {
      if (node->as.objectLiteral.count > UINT8_MAX) {
        errorAt(line, "too many properties in one object literal (limit %d)", UINT8_MAX);
        break;
      }
      /* A spread or an accessor means the entries have to be applied one at a
       * time, in order — the run-of-pairs form can express neither. */
      bool oneAtATime = false;
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        if (node->as.objectLiteral.kinds[i] != OBJECT_ENTRY_VALUE) oneAtATime = true;
      }

      /* Without a spread the whole literal is one instruction over a run of
       * stack pairs. With one it is built up entry by entry, because the
       * entries have to be applied in source order for the last mention of a
       * key to win. */
      if (!oneAtATime) {
        /* Keys and values alternate on the stack; OP_OBJECT consumes the pairs. */
        for (int i = 0; i < node->as.objectLiteral.count; i++) {
          compileNode(node->as.objectLiteral.keys[i]);
          compileNode(node->as.objectLiteral.values[i]);
        }
        emitBytes(OP_OBJECT, (uint8_t)node->as.objectLiteral.count, line);
        break;
      }

      emitBytes(OP_OBJECT, 0, line);
      for (int i = 0; i < node->as.objectLiteral.count; i++) {
        ObjectEntryKind kind = (ObjectEntryKind)node->as.objectLiteral.kinds[i];
        if (kind == OBJECT_ENTRY_SPREAD) {
          compileNode(node->as.objectLiteral.values[i]);
          emitByte(OP_OBJECT_MERGE, line);
          continue;
        }

        if (kind == OBJECT_ENTRY_PROTO) {
          /* The key is not needed: the name was the whole instruction. */
          compileNode(node->as.objectLiteral.values[i]);
          emitByte(OP_SET_PROTOTYPE, line);
          continue;
        }

        compileNode(node->as.objectLiteral.keys[i]);
        compileNode(node->as.objectLiteral.values[i]);
        if (kind == OBJECT_ENTRY_VALUE) {
          emitByte(OP_OBJECT_SET, line);
        } else {
          emitBytes(OP_OBJECT_ACCESSOR, kind == OBJECT_ENTRY_GETTER ? 1 : 0, line);
        }
      }
      break;
    }

    case AST_ARRAY_LITERAL: {
      if (node->as.arrayLiteral.count > UINT8_MAX) {
        errorAt(line, "too many elements in one array literal (limit %d)", UINT8_MAX);
        break;
      }

      /* A literal with no spread uses the cheaper builder that copies straight
       * across without inspecting each element. */
      bool hasSpread = false;
      for (int i = 0; i < node->as.arrayLiteral.count; i++) {
        if (node->as.arrayLiteral.elements[i]->type == AST_SPREAD) hasSpread = true;
        compileNode(node->as.arrayLiteral.elements[i]);
      }
      emitBytes(hasSpread ? OP_ARRAY_SPREAD : OP_ARRAY, (uint8_t)node->as.arrayLiteral.count, line);
      break;
    }

    case AST_SPREAD:
      compileNode(node->as.spread);
      emitByte(OP_SPREAD_MARK, line);
      break;

    case AST_DESTRUCTURE: compileDestructure(node); break;

    case AST_CALL: {
      if (node->as.call.argCount > UINT8_MAX) {
        errorAt(line, "too many arguments (limit %d)", UINT8_MAX);
        break;
      }
      const AstNode *callee = node->as.call.callee;

      /* `super(...)` and `super.m(...)`. Both leave the receiver below the
       * arguments and the superclass on top, which is the shape the two super
       * opcodes read. */
      if (callee->type == AST_SUPER) {
        if (!compileThisLoad(line)) break;
        for (int i = 0; i < node->as.call.argCount; i++) {
          compileNode(node->as.call.arguments[i]);
        }
        if (!compileSuperLoad(line)) break;

        if (callee->as.super.name == NULL) {
          if (current->kind != FUNCTION_CONSTRUCTOR) {
            errorAt(line, "'super()' can only be called from a constructor");
            break;
          }
          emitBytes(OP_SUPER_CALL, (uint8_t)node->as.call.argCount, line);
        } else {
          emitConstantOp(OP_SUPER_INVOKE, identifierConstant(callee->as.super.name, callee->as.super.length, line), line);
          emitByte((uint8_t)node->as.call.argCount, line);
        }
        break;
      }

      if (node->as.call.isNew) {
        compileNode(callee);
        for (int i = 0; i < node->as.call.argCount; i++) {
          compileNode(node->as.call.arguments[i]);
        }
        emitBytes(OP_NEW, (uint8_t)node->as.call.argCount, line);
        break;
      }

      bool hasSpread = false;
      for (int i = 0; i < node->as.call.argCount; i++) {
        if (node->as.call.arguments[i]->type == AST_SPREAD) hasSpread = true;
      }

      /* Spread arguments are packed into one array, because how many there are
       * is only known at run time. That costs the receiver, so a built-in
       * method cannot be called this way — `Math.max(...xs)` works because it
       * ignores its receiver, while `xs.push(...ys)` reports that it cannot. */
      if (hasSpread) {
        if (callee->type == AST_PROPERTY) {
          compileNode(callee->as.property.object);
          if (callee->as.property.optional) emitOptionalGuard(line);
          emitPropertyOp(OP_GET_PROPERTY, identifierConstant(callee->as.property.name, callee->as.property.length, line), line);
        } else {
          compileNode(callee);
        }

        if (node->as.call.optional) emitOptionalGuard(line);
        for (int i = 0; i < node->as.call.argCount; i++) {
          compileNode(node->as.call.arguments[i]);
        }
        emitBytes(OP_ARRAY_SPREAD, (uint8_t)node->as.call.argCount, line);
        emitByte(OP_CALL_SPREAD, line);
        break;
      }

      /* `x.name(...)` becomes one instruction instead of a property load
       * followed by a call, which also keeps the receiver available so a
       * built-in method can see what it was called on. */
      /* `o.m?.()` tests the method but still calls it on `o`, so the property
       * is read twice: once to see whether it is there, and once by the invoke
       * that keeps the receiver. Loading it as a plain value and calling that
       * would be one lookup, but it would also silently bind `this` to the
       * function — wrong quietly, which is worse than not compiling. */
      if (node->as.call.optional && callee->type == AST_PROPERTY) {
        compileNode(callee->as.property.object);
        if (callee->as.property.optional) emitOptionalGuard(line);

        int nameConstant = identifierConstant(callee->as.property.name, callee->as.property.length, line);
        emitConstantOp(OP_JUMP_IF_NO_METHOD, nameConstant, line);
        int missing = emitJump16(line);

        for (int i = 0; i < node->as.call.argCount; i++) {
          compileNode(node->as.call.arguments[i]);
        }
        emitConstantOp(OP_INVOKE, nameConstant, line);
        emitByte((uint8_t)node->as.call.argCount, line);
        int over = emitJump(OP_JUMP, line);

        /* Absent: the receiver is still on top, and the chain's landing site
         * turns exactly one value into undefined. */
        patchJump(missing, line);
        emitOptionalJump(line);
        patchJump(over, line);
        break;
      }

      bool isMethodCall = callee->type == AST_PROPERTY;
      /* `o[name](...)` is a method call too, and has to keep its receiver for
       * the same reason `o.name(...)` does: reading the method first and
       * calling the value would bind `this` to the function. The name is not
       * known until run time, so it travels on the stack rather than in the
       * instruction. */
      bool isComputedCall = callee->type == AST_INDEX && !node->as.call.optional;
      if (isMethodCall) {
        compileNode(callee->as.property.object);
        if (callee->as.property.optional) emitOptionalGuard(line);
      } else if (isComputedCall) {
        compileNode(callee->as.index.target);
        if (callee->as.index.optional) emitOptionalGuard(line);
        compileNode(callee->as.index.index);
      } else {
        compileNode(callee);
      }
      /* Before the arguments: `f?.(g())` must not call `g` when `f` is
       * absent. Only a plain callee reaches here; the property form is
       * handled above. */
      if (node->as.call.optional) emitOptionalGuard(line);

      for (int i = 0; i < node->as.call.argCount; i++) {
        compileNode(node->as.call.arguments[i]);
      }

      if (isMethodCall) {
        emitConstantOp(OP_INVOKE, identifierConstant(callee->as.property.name, callee->as.property.length, line), line);
        emitByte((uint8_t)node->as.call.argCount, line);
      } else if (isComputedCall) {
        emitBytes(OP_INVOKE_INDEX, (uint8_t)node->as.call.argCount, line);
      } else {
        emitBytes(OP_CALL, (uint8_t)node->as.call.argCount, line);
      }
      break;
    }

    case AST_UNARY:
      compileNode(node->as.unary.operand);
      switch (node->as.unary.op) {
        case UNARY_NEGATE: emitByte(OP_NEGATE, line); break;
        case UNARY_NOT: emitByte(OP_NOT, line); break;
        case UNARY_TYPEOF: emitByte(OP_TYPEOF, line); break;
        /* `void x` runs x for whatever it does and answers undefined. */
        case UNARY_VOID:
          emitByte(OP_POP, line);
          emitByte(OP_UNDEFINED, line);
          break;
      }
      break;

    case AST_BINARY: compileBinary(node); break;

    case AST_LOGICAL: compileLogical(node); break;

    case AST_GROUPING:
      /* Parentheses only affect parsing; they emit nothing of their own. */
      compileNode(node->as.grouping);
      break;
    case AST_CONDITIONAL: {
      int elseJump = emitConditionJump(node->as.conditional.condition, line);
      compileNode(node->as.conditional.thenValue);
      int endJump = emitJump(OP_JUMP, line);
      patchJump(elseJump, line);
      compileNode(node->as.conditional.elseValue);
      patchJump(endJump, line);
      break;
    }

    case AST_AWAIT:
      compileNode(node->as.unary.operand);
      emitAwait(line);
      break;

    case AST_REGEX_LITERAL:
      emitByte(OP_REGEX, line);
      emitConstantOperand(identifierConstant(node->as.regex.source, node->as.regex.sourceLength, line), line);
      emitConstantOperand(identifierConstant(node->as.regex.flags, node->as.regex.flagsLength, line), line);
      break;

    case AST_THIS: compileThisLoad(line); break;

    case AST_DYNAMIC_IMPORT:
      compileNode(node->as.unary.operand);
      emitByte(OP_DYNAMIC_IMPORT, line);
      break;

    case AST_NEW_TARGET:
      /* Rejected in an arrow rather than answered with undefined. An arrow
       * borrows `this` from what encloses it and JavaScript has `new.target`
       * do the same, but an arrow here pushes a frame of its own and the
       * answer lives on the frame — so the honest thing is to name the gap
       * rather than quietly give the wrong answer. */
      if (current->kind == FUNCTION_ARROW) {
        errorAt(line,
                "'new.target' is not available inside an arrow function; "
                "read it in the enclosing function and capture it");
        break;
      }
      if (current->kind == FUNCTION_SCRIPT) {
        errorAt(line, "'new.target' is only valid inside a function");
        break;
      }
      emitByte(OP_NEW_TARGET, line);
      break;

    case AST_SUPER:
      if (node->as.super.name == NULL) {
        errorAt(line, "'super' can only be called or used with a property");
        break;
      }
      /* `super.m` read without calling: the bound method has to carry the
       * receiver with it. */
      if (!compileThisLoad(line)) break;
      if (!compileSuperLoad(line)) break;
      emitConstantOp(OP_GET_SUPER, identifierConstant(node->as.super.name, node->as.super.length, line), line);
      break;

    default: return false;
  }
  return true;
}
