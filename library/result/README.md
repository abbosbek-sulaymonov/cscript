# `std:result`

A failure as a value, and a missing thing as a value.

```ts
import { Result, Option } from "std:result";

function halve(n: number): any {
  return n % 2 === 0 ? Result.ok(n / 2) : Result.err("odd");
}

console.log(halve(8).map((n: number) => n + 1).unwrapOr(-1));   // 5
console.log(halve(7).unwrapOr(-1));                             // -1
```

`throw` is right when a caller cannot sensibly carry on. It is wrong when
failing is an ordinary outcome — parsing input, looking something up — because
then every call site has to be wrapped in `try` to find out, and the function's
type says nothing about the fact that it can fail.

## Specification

### `Result` — ok, or an error

| Constructor | Signature | Answers |
| --- | --- | --- |
| `Result.ok` | `(value?)` | a successful Result |
| `Result.err` | `(error)` | a failed one |
| `Result.attempt` | `(body)` | `ok` of what the body returned, or `err` of what it threw |
| `Result.all` | `(results)` | `ok` of every value in order, or the **first** error |

| Member | Signature | Behaviour |
| --- | --- | --- |
| `isOk` / `isErr` | getters | which one it is |
| `value` | getter | the value; **throws** on an error Result |
| `error` | getter | the error; **throws** on an ok one |
| `map` | `(transform)` | transforms the value, leaves an error alone |
| `mapErr` | `(transform)` | the other way round |
| `andThen` | `(next)` | for a step that can itself fail — `next` answers a Result |
| `orElse` | `(recover)` | `recover` sees the error and answers a Result |
| `unwrapOr` | `(fallback)` | the value, or the fallback |
| `unwrapOrElse` | `(fromError)` | the value, or what the error is turned into |
| `match` | `(onOk, onErr)` | both branches, named |
| `toOption` | `()` | the value as a `some`, an error as `none` |
| `toString` | `()` | `Ok(…)` or `Err(…)` |

### `Option` — some, or none

| Constructor | Signature | Answers |
| --- | --- | --- |
| `Option.some` | `(value)` | a present Option |
| `Option.none` | `()` | an empty one |
| `Option.from` | `(value)` | `none` for `null` and `undefined`, `some` otherwise |

| Member | Signature | Behaviour |
| --- | --- | --- |
| `isSome` / `isNone` | getters | which one it is |
| `value` | getter | the value; **throws** when empty |
| `map` | `(transform)` | transforms a present value |
| `filter` | `(keep)` | a `some` that fails the test becomes `none` |
| `andThen` | `(next)` | `next` answers an Option |
| `orElse` | `(build)` | `build` answers an Option when this is empty |
| `unwrapOr` / `unwrapOrElse` | `(fallback)` / `(build)` | the value or the alternative |
| `match` | `(onSome, onNone)` | both branches |
| `toResult` | `(error)` | with the meaning the caller gives the absence |
| `[Symbol.iterator]` | `()` | empty, or one element |
| `toString` | `()` | `Some(…)` or `None` |

## Implementation

**The constructors are not the interface.** `new Result(true, v, null)` says
nothing at a call site, so `Result.ok` and `Result.err` are what callers use.

**Reading the value of an error throws.** The whole reason to hold a failure in
a value is that it cannot be walked past — answering `undefined` there would
put the mistake back.

**`map` and `andThen` are the difference between one Result and two.** `map`
wraps whatever the callback answers; `andThen` does not, because the callback
already answered a Result. Using the wrong one gives `Ok(Ok(3))`.

**An error travels untouched to the end of a chain**, which is the point of
holding one this way: a `map` on an error Result is not called at all.

**`Option` iterates.** Empty or one element, so `for...of` and spread work —
the shortest way to drop the empties out of a list of Options.

**What `attempt` can and cannot catch.** A `throw` the program made, yes; a
runtime error, no — those are not catchable in CScript. `JSON.parse("{")`
is a runtime error, so `Result.attempt(() => JSON.parse(bad))` will not save
you. That is the same line the language draws between a decision and a
mistake.

## Source

[`result.cx`](result.cx). Both classes hold their state in private fields, so
an ok Result and an error one are the same class rather than two.
