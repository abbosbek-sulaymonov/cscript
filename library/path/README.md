# `std:path`

File paths as strings, with no filesystem involved.

```ts
import { join, dirname, extname, relative } from "std:path";

join("src", "../lib", "mod.cx");   // "lib/mod.cx"
extname("a.tar.gz");               // ".gz"
```

Every function here is pure text. Nothing touches the disk, nothing asks where
the program is running, and `resolve` takes the base it should resolve against
rather than reaching for the working directory — which is what makes the module
testable and keeps it independent of [`std:os`](../os) and
[`std:io`](../io).

POSIX separators. A Windows path comes in with `fromWindows` and goes back out
with `toWindows`, so the middle of a program only ever deals with one shape.

## Specification

### Taking one apart

| Export | Signature | Answers |
| --- | --- | --- |
| `split` | `(path)` | the parts, empty ones dropped |
| `isAbsolute` | `(path)` | whether it starts at the root |
| `dirname` | `(path)` | everything before the last separator, or `"."` |
| `basename` | `(path, suffix?)` | the last part, with that suffix removed if present |
| `extname` | `(path)` | the extension **including the dot**, or `""` |
| `stem` | `(path)` | the last part without its extension |

### Putting one together

| Export | Signature | Answers |
| --- | --- | --- |
| `join` | `(...parts)` | one separator between parts, then normalised |
| `normalize` | `(path)` | `"."` removed and `".."` resolved |
| `resolve` | `(base, path)` | `path` if absolute, else joined onto `base` |
| `relative` | `(from, to)` | how to get from one to the other, using `".."` |

### Changing one

| Export | Signature | Answers |
| --- | --- | --- |
| `withExtension` | `(path, extension)` | the extension replaced; the dot is optional |
| `withName` | `(path, name)` | the last part replaced |
| `isInside` | `(parent, child)` | whether the child is under the parent |
| `fromWindows` / `toWindows` | `(path)` | backslashes swapped for slashes, and back |

## Implementation

**`dirname` answers `"."` when there is no separator**, which is what every
other implementation does and means "the directory I am in".

**A leading dot is not an extension.** `.bashrc` is a name, not a file with a
`bashrc` extension — `extname` answers `""` for it, as every other
implementation does.

**Normalisation is textual, and that is worth knowing.** A symbolic link is
followed differently here than by the kernel, because nothing here asks the
kernel anything. A `".."` that would climb past the root of an absolute path is
dropped — there is nothing above `/` — and on a relative path it is kept,
because `"../x"` means something the text alone cannot resolve.

**`resolve` takes its base as an argument.** A `resolve` that reached for the
working directory would make every test depend on where it was run from, and
would put a dependency on [`std:os`](../os) in a module that is otherwise pure
string work. A caller who wants the working directory passes `os.cwd()`.

**`relative` refuses to mix kinds.** A relative path from an absolute one to a
relative one has no meaning without knowing where the relative one starts, so
it says so rather than answering something.

**`isInside` is about the names**, not the filesystem: a symbolic link out of a
directory is still inside it by this test. A path is not inside itself.

## Source

[`path.cx`](path.cx). `stripTrailing` and `stripLeading` are private: the two
trims every other function is built on.
