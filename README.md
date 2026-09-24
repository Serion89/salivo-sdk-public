<div align="center">

<img src="salivo_logo.png" alt="Salivo Logo" width="160" />

# Salivo SDK

**A compiled systems language with a self-hosted compiler, deterministic cleanup and tiny native binaries**
*Stream imports (><) | Drop-based cleanup | Native code through LLVM | Built-in async runtime*

[![Version](https://img.shields.io/badge/version-v1.0.9-blue.svg)](https://github.com/Serion89/salivo-sdk-public/releases/latest)
[![Platforms](https://img.shields.io/badge/platforms-Windows%20x64%20%7C%20Linux%20x64-brightgreen.svg)](#platform-support)
[![License](https://img.shields.io/badge/license-BSL%201.1-purple.svg)](LICENSE)
[![LLVM](https://img.shields.io/badge/backend-LLVM%2016-red.svg)](https://llvm.org)

</div>

---

## Contents

- [What Salivo is](#what-salivo-is)
- [Why use Salivo](#why-use-salivo)
- [Performance](#performance)
- [Install](#install)
- [Your first program](#your-first-program)
- [Language tour](#language-tour)
- [Standard library](#standard-library)
- [Tooling](#tooling)
- [How compilation works](#how-compilation-works)
- [Platform support](#platform-support)
- [Known limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)
- [Repository structure](#repository-structure)
- [License](#license)

---

## What Salivo is

Salivo is a statically typed, compiled language that produces native executables through LLVM. Its
syntax is small and readable: modules are imported with stream headers (`><`), resources are released
deterministically through the `Drop` trait, and strings support inline interpolation with modifiers
such as `{name:trim:upper}`.

The compiler that builds your programs is itself written in Salivo. This SDK ships that self-hosted
**Stage 3 compiler** together with the `sf` driver, the standard library, the package manager, the
formatter, the linter, the documentation generator, the language server and a VS Code extension.

---

## Why use Salivo

**It is self-hosting, and the build is reproducible.** The Stage 3 compiler compiles its own source.
The compiler built by the first generation and the one built by the second are bit-for-bit identical
(a verified fixed point), on both Windows and Linux. A language whose compiler can rebuild itself
exactly is one whose semantics are stable enough to trust.

**Builds are fast and light.** A typical program builds in about a quarter of a second, using a
fraction of the memory other native toolchains need (see [Performance](#performance)). That keeps the
edit-run loop short, which matters more day to day than peak benchmark numbers.

**Executables are tiny.** Programs are usually 10 to 20 KB. They are easy to ship, copy and attach, and
they start quickly.

**Memory is managed without a garbage collector.** There is no collector pausing your program. Values
with a destructor are released when they go out of scope, and the compiler also frees strings and
small structs it can prove are no longer shared. A loop that builds three million strings peaks at
about 1 MB of memory.

**Cleanup is deterministic.** Implement `Drop` for a type and its cleanup code runs at a predictable
point, when the value goes out of scope. File handles, connections and locks are released without
manual `close()` calls.

**It comes with a real async runtime.** Tasks, channels with backpressure, timers, cancellation,
TCP/UDP networking and asynchronous file I/O are part of the standard library (`salivo.std.runtime`),
on a work-stealing scheduler. The Windows implementation runs on I/O completion ports and is certified
against 21 correctness gates, including leak detection and shutdown under load.

**The tooling is complete from day one.** One install gives you a build tool, a package manager with
workspaces, a formatter, a linter, a documentation generator, a language server and a VS Code
extension with syntax highlighting, snippets and one-key run and build.

**It works offline and without admin rights.** Everything installs into your user profile. Nothing
needs to be downloaded at build time, so it works on locked-down and offline machines.

**Diagnostics come from one front end.** `sf` checks every program with its own parser and type
checker before code generation, so errors are reported consistently with source locations, whichever
back end compiles the program.

**It is AI-assistant friendly.** The repository includes ready-made rule files for Cursor, Claude
Code, GitHub Copilot, Windsurf and other assistants, so they write idiomatic Salivo from the start.

---

## Performance

Measured on a 16-core Windows 11 machine. Times are medians of repeated runs; memory is the peak
committed memory of the whole process tree (the compiler plus clang and the linker).

**Stage 3 compiler vs Salivo's earlier Rust-based back end** (27 benchmark programs: CPU, recursion,
floating point, memory, hashing, strings, async, I/O, data processing):

| Metric | Rust back end | Stage 3 (default) |
| :--- | ---: | ---: |
| Build time | 0.31 s | **0.25 s** |
| Compiler CPU time | 0.29 s | **0.21 s** |
| Executable size | 188 KB | **15 KB** |
| First run of a new executable | 0.12 s | **0.10 s** |
| Run time (warm) | 0.039 s | **0.038 s** |

**Compared with Zig 0.13 (`-OReleaseFast`)** on the eight benchmarks that exist in both languages:

| Metric | Zig | Salivo |
| :--- | ---: | ---: |
| Build time | 0.48 s | **0.25 s** |
| Compiler memory | 306 MB | **71 MB** |
| Executable size | 151 KB | **12 KB** |
| Program memory | **3.9 MB** | 7.1 MB |
| Run time | **faster on 4 of 5 comparable programs** (up to 1.9x on array-heavy code) | faster on the prime sieve |

Salivo builds faster, lighter and smaller than Zig; Zig currently generates faster code for
array-heavy loops. Closing that gap is on the roadmap.

---

## Install

### Windows (x64)

1. Download **`salivo-sdk-1.0.9-windows-x64.zip`** from the
   [latest release](https://github.com/Serion89/salivo-sdk-public/releases/latest) and extract it.
2. Double-click **`Salivo Setup.exe`**. Windows may show "Windows protected your PC" because the
   installer is not code-signed yet: click **More info > Run anyway**.
3. Open a new terminal (or restart VS Code).

Setup installs into `%USERPROFILE%\.salivo`, adds `sf` to your user PATH, installs the VS Code
extension, gives `.sal` files the Salivo icon, and ends by compiling and running a test program.

Prerequisites (setup detects them and offers to install whatever is missing through `winget`):

- **LLVM** (clang): `winget install -e --id LLVM.LLVM`
- **Visual Studio 2022 C++ build tools**:
  `winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`

Cloning this repository and running `install.bat` also works, but the release zip is the complete
SDK: it also contains `bin/LLVM-C.dll`, which is too large to keep in git.

### Linux (x64)

1. Download **`salivo-sdk-1.0.9-linux-x64.tar.gz`** from the
   [latest release](https://github.com/Serion89/salivo-sdk-public/releases/latest).
2. Install clang if you do not have it:
   - Ubuntu 24.04 and newer: `sudo apt install clang`
   - Debian 12 or Ubuntu 22.04: `sudo apt install clang-16` (setup finds it automatically)
   - Fedora: `sudo dnf install clang` / Arch: `sudo pacman -S clang`
3. Extract and run the installer:
   ```bash
   tar xzf salivo-sdk-1.0.9-linux-x64.tar.gz
   sh salivo-sdk-1.0.9-linux-x64/install.sh
   ```
4. Open a new terminal.

Setup installs into `~/.salivo` (no sudo), adds it to PATH in your shell profile and installs the VS
Code extension when the `code` command is available. Tested on clean Ubuntu 24.04 and Debian 12.

### macOS

A macOS SDK is not available yet; see [Platform support](#platform-support).

---

## Your first program

Save as `hello.sal`:

```salivo
module hello;

><salivo.std.core::{outln};

func main() -> int {
    let name = "  salivo  ";
    let stage = 3;
    outln($"Hello from {name:trim:upper}, stage {stage}!");
    return 0;
}
```

Run it:

```bash
sf run hello.sal
```

```
Hello from SALIVO, stage 3!
```

In VS Code, open the file and press **Ctrl+F5**. Every example below is in the `examples/` folder and
was compiled and run with this SDK.

---

## Language tour

### Structs and deterministic cleanup

Implement `Drop` and the cleanup code runs when the value goes out of scope, here when `serve`
returns:

```salivo
module shapes;

><salivo.std.core::{outln, Drop};

pub struct Rect {
    int w;
    int h;
}

pub struct Connection {
    int id;
}

impl Drop for Connection {
    func drop(self) {
        outln($"closing connection {self.id}");
    }
}

func area(r: Rect) -> int {
    return r.w * r.h;
}

func serve() -> int {
    let conn = Connection { id: 7 };
    outln($"serving on connection {conn.id}");
    return 0;
}

func main() -> int {
    let r = Rect { w: 3, h: 4 };
    let a = area(r);
    outln($"area = {a}");
    serve();
    outln("done");
    return 0;
}
```

```
area = 12
serving on connection 7
closing connection 7
done
```

### Error handling with Result

Functions that can fail return `Result<T, E>`; callers check `is_ok` and read `value` or `error`:

```salivo
module errors;

><salivo.std.core::{outln, Result, ok, err};

func parseAge(age: int) -> Result<int, string> {
    if (age < 0) {
        return err<int, string>("age cannot be negative");
    }
    if (age > 150) {
        return err<int, string>("age is not realistic");
    }
    return ok<int, string>(age);
}

func report(age: int) -> int {
    let r = parseAge(age);
    if (r.is_ok) {
        let next = r.value + 1;
        outln($"valid, next birthday: {next}");
        return 0;
    }
    let reason = r.error;
    outln($"rejected: {reason}");
    return 1;
}

func main() -> int {
    report(29);
    report(-4);
    return 0;
}
```

```
valid, next birthday: 30
rejected: age cannot be negative
```

### Collections and pattern matching

`match` is an expression, and `_` matches anything else:

```salivo
module inventory;

><salivo.std.core::{outln};
><salivo.std.collections::{vecNew, push, get, veclen, mapNew, put, lookupOr};

func describe(count: int) -> string {
    return match count {
        0 => "out of stock",
        1 => "last one",
        _ => "in stock",
    };
}

func main() -> int {
    let mut items = vecNew<string>();
    items = push<string>(items, "apple");
    items = push<string>(items, "pear");
    items = push<string>(items, "plum");

    let mut stock = mapNew<string, int>();
    stock = put<string, int>(stock, "apple", 12);
    stock = put<string, int>(stock, "pear", 1);

    let mut i = 0;
    while (i < veclen<string>(items)) {
        let name = get<string>(items, i);
        let count = lookupOr<string, int>(stock, name, 0);
        let state = describe(count);
        outln($"{name}: {state}");
        i = i + 1;
    }
    return 0;
}
```

```
apple: in stock
pear: last one
plum: out of stock
```

### Async tasks

The runtime runs tasks on a pool of worker threads; `spawn` starts a task and `join` waits for its
result:

```salivo
module tasks;

><salivo.std.core::{outln};
><salivo.std.runtime::{runtimeCreate, runtimeRun, runtimeShutdown, runtimeJoin, runtimeDestroy, spawn, join};

func square(x: int) -> int {
    return x * x;
}

func root(n: int) -> int {
    let a = spawn(square, n).value;
    let b = spawn(square, n + 1).value;
    return join(a).value + join(b).value;
}

func main() -> int {
    let rt = runtimeCreate(0).value;
    let total = runtimeRun(rt, root, 6).value;
    let s = runtimeShutdown(rt);
    let j = runtimeJoin(rt, 5000);
    let d = runtimeDestroy(rt);
    outln($"6*6 + 7*7 = {total}");
    return 0;
}
```

```
6*6 + 7*7 = 85
```

Channels, timers, cancellation, TCP and UDP networking and asynchronous files work the same way; see
`salivo.std.runtime` in `std/runtime.sal`.

### Syntax at a glance

| Feature | Example |
| :--- | :--- |
| Import | `><salivo.std.fs::{pathJoin, pathExists};` |
| Bindings | `let name = "x";`, `let mut n = 0;`, `const MAX = 8192;`, `global let PORT = 8080;` |
| Loops | `while (cond) { ... }`, `for (let mut i = 0; i < 10; i = i + 1) { ... }` |
| Interpolation | `$"User: {name:trim:upper}, id {id}"` |
| Modifiers | `:int`, `:float`, `:str`, `:trim`, `:upper`, `:lower`, `:len` |
| Types | `int`, `float`, `bool`, `string`, `Vec<T>`, `HashMap<K, V>`, `Option<T>`, `Result<T, E>` |
| Naming | types in PascalCase; functions and fields in camelCase or snake_case |

---

## Standard library

| Module | Highlights |
| :--- | :--- |
| `salivo.std.core` | `outln`, `out`, `panic`, `assert`, `assertEq`, `Option`, `Result`, `Drop` |
| `salivo.std.collections` | `Vec`, `HashMap`, `HashSet`, `Deque`, heaps, B-trees |
| `salivo.std.strings` | `String`, slices, `startsWith`, `endsWith`, `padStart`, `pushStr` |
| `salivo.std.runtime` | async runtime: tasks, channels, timers, cancellation, TCP, UDP, files |
| `salivo.std.fs` / `salivo.std.io` | files, directories, paths, console I/O |
| `salivo.std.net` | sockets, addresses, name resolution |
| `salivo.std.sync` | mutexes, read-write locks, atomics, channels |
| `salivo.std.time` | clocks, durations, sleep, formatting |
| `salivo.std.math` / `salivo.std.random` | arithmetic helpers, trigonometry, square root, random numbers |
| `salivo.std.crypto` / `salivo.std.hash` | SHA-256, SHA-512, BLAKE3, AES, hashing helpers |
| `salivo.std.mem` | allocation, raw memory access, layout queries (`sizeof`, `alignof`) |

---

## Tooling

### sf (compiler driver)

```bash
sf run main.sal              # build and run
sf build main.sal            # build build/main.exe (Windows) or build/main
sf check main.sal            # type-check without generating code
sf build -O 3 main.sal       # optimization level 0 to 3 (default 2)
sf build --compiler rust main.sal   # use the Rust back end instead of Stage 3
sf tokens | ast | hir | mir | ssa | ir main.sal   # inspect each compiler stage
sf version
```

### spm (package manager)

```bash
spm new my_app      # create a package
spm build           # build the package or workspace
spm run             # build and run its binary
spm test            # run its tests
```

### Code quality

```bash
salivofmt --write src/     # format in place (--check for CI, --diff to preview)
salivolint src/            # static analysis
salivodoc src/             # generate documentation
```

### VS Code extension

Installed by setup (`vscode/salivo-1.0.9.vsix`): syntax highlighting, snippets (`main`, `func`,
`struct`, `impl`, `drop`, `match`, `for` and more), file icons, and commands: **Ctrl+F5** run,
**Ctrl+Shift+B** build, **Ctrl+Shift+K** check, **Ctrl+Shift+L** lint, **Shift+Alt+F** format.

---

## How compilation works

`sf` parses and type-checks every program with its own front end, so diagnostics are consistent. The
self-hosted Stage 3 compiler then lowers the program through HIR, MIR and SSA, runs its ownership
analysis and optimizer, and emits LLVM IR, which clang compiles and links against the Salivo runtime.

Each program is compiled as a whole, so functions are inlined across your code and the runtime, and
unused library code is removed before optimization. That is what keeps builds fast and executables
small.

Programs that use features Stage 3 does not support yet (for example trait method calls) are compiled
by the Rust back end automatically, with a `note:` saying so. Packages built with `spm` use the Rust
back end.

---

## Platform support

| Platform | Status |
| :--- | :--- |
| Windows 10/11 x64 | Supported. Installer with setup program, VS Code extension, file icons. |
| Linux x64 (glibc 2.35 or newer) | Supported. `install.sh`; tested on clean Ubuntu 24.04 and Debian 12. Other current distributions (Ubuntu 22.04, Fedora, Arch) should work. |
| macOS (Apple Silicon and Intel) | Not yet. Mac binaries must be built on a Mac; a macOS release will follow. |

The async runtime is certified on Windows. Its Linux back end (epoll) is included and used, but not
yet certified to the same standard.

---

## Known limitations

- The postfix `?` operator (early return from `Result`/`Option`) does not compile correctly yet; use
  explicit `is_ok` / `is_some` checks as in the examples above.
- String interpolation accepts variables and modifiers, not function calls: bind the call to a
  variable first (`let a = area(r);` then `{a}`).
- The Windows installer is not code-signed, so Windows asks for confirmation the first time.
- Some programs fall back to the Rust back end (see [How compilation works](#how-compilation-works));
  those build correctly but more slowly and larger.

---

## Troubleshooting

- **`sf` is not recognized**: open a new terminal after installing; PATH changes apply to new windows.
- **Linker errors when building**: install the prerequisites for your platform (see [Install](#install)).
- **`.sal` files lost the Salivo icon** (another program registered itself for `.sal`): run
  `powershell -ExecutionPolicy Bypass -File install.ps1 -RepairFileIcons` from the SDK folder. It
  restores the icon and the VS Code "open" action and leaves the other program's handler in place.
- **Uninstall**: `powershell -ExecutionPolicy Bypass -File uninstall.ps1` (Windows) or
  `sh uninstall.sh` (Linux).

---

## Universal AI integration

The repository includes rule files so AI coding assistants write idiomatic Salivo:

- **Cursor**: `.cursorrules`
- **Claude Code**: `CLAUDE.md`
- **GitHub Copilot**: `.github/copilot-instructions.md`
- **Windsurf**: `.windsurfrules`
- **Cline, Roo Code, Aider and other agents**: `AGENTS.md`
- **Chat assistants**: attach `docs/salivo_quick_reference.md` to the conversation.

---

## Repository structure

```
salivo-sdk-public/
|-- assets/              Official logo and icons
|-- bin/                 Windows executables (sf, salivoc Stage 3 compiler, spm, salivofmt,
|                        salivolint, salivodoc, salivo-lsp)
|-- docs/                Language guide (PDF, HTML, Markdown) and quick reference
|-- examples/            The programs from this README
|-- lib/salivo/rt/       Runtime objects for the Rust back end
|-- stage3/              Stage 3 runtime sources and prebuilt objects
|-- std/                 Standard library
|-- vscode/              VS Code extension (.vsix)
|-- commands/, mcp/, skills/   AI assistant commands, MCP server and skills
|-- Salivo Setup.exe     Windows installer (runs install.ps1)
|-- install.ps1 / install.bat / uninstall.ps1   Windows install scripts
|-- install.sh / uninstall.sh                   Linux install scripts
`-- AGENTS.md, CLAUDE.md, .cursorrules, .windsurfrules   AI assistant rules
```

---

## License

Salivo is distributed under the **Business Source License 1.1**. See [LICENSE](LICENSE) for the terms.

---

## Author & Maintainer

<table border="0">
  <tr>
    <td width="100" align="center" valign="middle">
      <a href="https://github.com/Serion89">
        <img src="https://avatars.githubusercontent.com/u/205588838?v=4" width="85" height="85" style="border-radius: 50%;" alt="Sahil Bhatt" />
      </a>
    </td>
    <td valign="middle">
      <h3>Sahil Bhatt</h3>
      <p>Software Developer | Compiler & Systems Engineer | AI</p>
      <p>Founder, <strong>Salivo Enterprises Pvt, Ltd.</strong></p>
      <p>
        <a href="https://github.com/Serion89">
          <img src="https://img.shields.io/badge/GitHub-Serion89-181717?style=for-the-badge&logo=github" alt="GitHub Profile" />
        </a>
        &nbsp;
        <a href="https://github.com/Serion89">
          <img src="https://img.shields.io/github/followers/Serion89?label=Follow&style=for-the-badge&logo=github&color=24292e" alt="Followers" />
        </a>
      </p>
    </td>
  </tr>
</table>
