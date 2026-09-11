# `std:os`

What the program can learn about where it is running.

```ts
import { env, args, tempDir, isMac, exit } from "std:os";

if (env("DEBUG") === "1") console.log(args(), tempDir(), isMac());
if (args().length === 0) exit(64);
```

Over the `process` built-in, with the parts a script actually asks for given
names of their own.

## Specification

### The environment

| Export | Signature | Answers |
| --- | --- | --- |
| `env` | `(name, fallback?)` | the value, the fallback, or `undefined` |
| `envOption` | `(name)` | the same as an [`Option`](../result/README.md) |
| `hasEnv` | `(name)` | whether it is set at all |
| `envNames` | `()` | every name, **sorted** |
| `envAll` | `()` | a **Map** of name to value |

### Where

| Export | Signature | Answers |
| --- | --- | --- |
| `cwd` | `()` | the working directory |
| `homeDir` | `()` | `HOME`, or `USERPROFILE` on Windows |
| `tempDir` | `()` | `TMPDIR`, `TMP`, `TEMP`, then `/tmp` |

### Which platform

| Export | Signature | Answers |
| --- | --- | --- |
| `platform` | `()` | `"darwin"`, `"linux"`, `"win32"` or `"unknown"` |
| `isMac` / `isLinux` / `isWindows` | `()` | a boolean each |
| `pathSeparator` | `()` | `"\\"` on Windows, `"/"` elsewhere |
| `pathListSeparator` | `()` | `";"` on Windows, `":"` elsewhere |
| `lineEnding` | `()` | `"\r\n"` on Windows, `"\n"` elsewhere |

### This process

| Export | Signature | Answers |
| --- | --- | --- |
| `args` | `()` | what the **script** was given, without the executable and script path |
| `scriptPath` | `()` | the file being run, or `undefined` |
| `executablePath` | `()` | the interpreter |
| `pid` | `()` | the process id |
| `exit` | `(code = 0)` | ends the program **now** |

## Implementation

**`process.env` is a function, and that is deliberate.** `process.env.HOME`
reads a property that may not be there, and in CScript reading an absent
property is a runtime error rather than `undefined` — so the Node spelling
would turn every optional variable into a crash. This is the one place the
runtime's `process` differs from Node's on purpose.

**`args()` drops the first two entries.** `process.argv` carries the executable
and the script in front for Node's sake; a program parsing its own options
wants neither.

**`exit` means now.** No pending timers, no queued microtasks, no collection. A
program that wants its outstanding work finished should return from the top
level instead, which is what the event loop is for. `exit` does flush what has
been printed — the last line before an exit is usually the reason the exit is
there.

**The separators are functions, not constants**, because a constant would have
to be computed at import time and this module is imported by things that never
ask.

**`envAll` answers a Map**, so walking it is ordinary iteration and a variable
named `toString` is not a surprise.

**Absent on purpose:** the hostname, the user, the load average, memory. None
is in the runtime, and each needs a native of its own — worth adding when
something needs them, not before.

## Node cannot check this module

Its expected output was produced by CScript rather than by Node, because
`process.env` is a function here and an object there. Everything else in
`tests/cases/stdlib/` is Node-generated.

## Source

[`os.cx`](os.cx). Nothing private.
