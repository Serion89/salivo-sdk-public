<div align="center">

<img src="assets/salivo_logo.png" alt="Salivo Logo" width="140" />

# Salivo

**Write safe native code without the ceremony.**

No garbage collector. No manual `free`. No lifetime annotations. A standard library that already includes a web server, SQL databases, a UI framework and a game engine.

[![Version](https://img.shields.io/badge/version-v1.0.26-blue.svg)](https://github.com/Serion89/salivo-sdk-public/releases/latest)
[![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-brightgreen.svg)](#platform-support)
[![License](https://img.shields.io/badge/license-BSL%201.1-purple.svg)](LICENSE)

[**Download**](https://github.com/Serion89/salivo-sdk-public/releases/latest) | [Install in one line](#install) | [First program](#your-first-program) | [Language tour](#language-tour)

</div>

---

| | |
| :--- | :--- |
| **~0.3 s** | cold build of a program |
| **~40 ms** | full type check (`sf check`) |
| **12 KB** | a hello world executable |
| **0** | packages to download for a web server, a database or a game |

```salivo
pub func main() -> int {
    outln("Hello, Salivo!");
    return 0;
}
```

```
sf run hello.sal
```

---

## Why developers pick Salivo

**Memory safety without the fight.** Values free what they own when they leave scope. No garbage collector pausing your program, no borrow checker to argue with, no manual cleanup. Reading past the end of a list stops with a clear message instead of corrupting memory:

```
panic: Vec::get index 5 out of bounds (length 1)
```

**Safety checks that never get in your way.**
- Live errors as you type from the language server, with no setup.
- `sf check` type-checks a file in about 40 ms, fast enough to run on every save.
- `salivolint --deny` turns every warning into a failure: one flag for CI.
- Errors are values: `Result` and `Option`, postfix `?` to pass an error up in one character, and `match` that must cover every case.
- `SALIVO_OVERFLOW_CHECKS=1` makes unchecked integer overflow panic, and checked and saturating arithmetic is built in.
- `spm check-layout` keeps a project's structure honest.

**Cleanup you cannot forget.** Implement `Drop` once and files, sockets, database connections, textures and locks are released at a known point, every time.

**Instant feedback.** A build takes about a quarter of a second. Run an unchanged program again and it starts in milliseconds from the run cache.

**Tiny native executables.** No runtime to install and nothing to bundle. Code goes through LLVM, so it runs at native speed.

**Everything in the box.** HTTP/1.1 and HTTP/2 server, PostgreSQL, MySQL and SQLite, JSON, regex, compression, cryptography, async tasks and channels, a reactive WebAssembly UI framework, and a 2D and 3D game engine with OpenGL, physics, audio and tile maps. Nothing to download before you start.

**One language for the whole stack.** `spm new site --template fullstack` gives you a native server and a browser UI, both written in Salivo and served from one origin.

**A compiler written in Salivo.** It builds itself, and the result is verified bit for bit on Windows and Linux, so what you install is what was tested.

**Complete tooling from day one.** `sf` (build, run, check), `spm` (packages, workspaces, tasks), a formatter, a linter, a documentation generator, a language server and a VS Code extension.

---

## What it looks like

```salivo
module main;

><salivo.std.core::{Result, ok, err};
><salivo.std.collections::{Vec, vecNew, push, veclen, get};

pub struct Item { name: string, price: int }

func parsePrice(text: string) -> Result<int, string> {
    if (len(text) == 0) { return err<int, string>("empty price"); }
    return ok<int, string>(text:int);
}

func total(items: Vec<Item>) -> int {
    let mut sum = 0;
    for (let mut i = 0; i < veclen<Item>(items); i = i + 1) {
        sum = sum + get<Item>(items, i).price;
    }
    return sum;
}

func run() -> Result<int, string> {
    let mut cart = vecNew<Item>();
    cart = push<Item>(cart, Item { name: "tea", price: parsePrice("12")? });
    cart = push<Item>(cart, Item { name: "cake", price: parsePrice("30")? });
    return ok<int, string>(total(cart));
}

pub func main() -> int {
    let r = run();
    if (r.is_ok) { outln($"Total: {r.value}"); } else { outln("error: " + r.error); }
    return 0;
}
```

No framework to import, no build file, no `free`. One file, one command: `sf run cart.sal`.

---

## Latest release: 1.0.26

- **Free for any use, including commercial.** No revenue or team-size limits. Hosting Salivo itself as a service or reselling it as a competing product needs a commercial license. Each version converts to Apache 2.0 four years after its release. See [LICENSE](LICENSE) and [TRADEMARK.md](TRADEMARK.md).
- **One-line installers** for Windows and Linux that check the SHA-256 of the download.
- **Third-party notices** ship with the SDK and the VS Code extension.

All releases: [github.com/Serion89/salivo-sdk-public/releases](https://github.com/Serion89/salivo-sdk-public/releases)

---

## Performance

Measured on a 16-core Windows 11 machine. Times are medians of repeated runs; memory is the peak committed memory of the whole process tree (the compiler plus clang and the linker).

**Stage 3 compiler vs Salivo's earlier Rust-based back end** (27 benchmark programs: CPU, recursion, floating point, memory, hashing, strings, async, I/O, data processing):

| Metric | Rust back end | Stage 3 (default) |
| :--- | ---: | ---: |
| Build time | 0.31 s | **0.25 s** |
| Compiler CPU time | 0.29 s | **0.21 s** |
| Executable size | 188 KB | **15 KB** |
| First run of a new executable | 0.12 s | **0.10 s** |
| Run time (warm) | 0.039 s | **0.038 s** |

**Compared with Zig 0.13 (`-OReleaseFast`)** on benchmarks:

| Metric | Zig 0.13 | Salivo AOT Stage 3 |
| :--- | ---: | ---: |
| Build time | 0.48 s | **0.25 s** |
| Compiler memory | 306 MB | **71 MB** |
| Executable size | 151 KB | **12 KB** |
| Matrix Multiply (512x512) | 11.26 ms | **10.51 ms (🥇 #1)** |
| QuickSort (200k) | 12.12 ms | **10.37 ms (🥇 #1)** |
| String Processing (100k) | 1.94 ms | **0.75 ms (🥇 #1)** |
| Concurrency (1k tasks) | 1.84 ms (Go) | **0.16 ms (🥇 #1)** |

---

## Install

**One line** (downloads the latest release and checks its SHA-256 before installing):

```
# Windows (PowerShell)
irm https://raw.githubusercontent.com/Serion89/salivo-sdk-public/main/get.ps1 | iex

# Linux
curl -fsSL https://raw.githubusercontent.com/Serion89/salivo-sdk-public/main/get.sh | sh
```

Or download the installer for your system from the [latest release](https://github.com/Serion89/salivo-sdk-public/releases/latest).

| System | Installer | How |
| :--- | :--- | :--- |
| **Windows 10/11 x64** | [`salivo-1.0.26-windows-x64-setup.exe`](https://github.com/Serion89/salivo-sdk-public/releases/download/v1.0.26/salivo-1.0.26-windows-x64-setup.exe) | Double-click. Installs for your user (no admin rights), adds `sf` to PATH, installs the VS Code extension and the `.sal` icon, then tests itself. |
| **Ubuntu / Debian x64** | [`salivo_1.0.26_amd64.deb`](https://github.com/Serion89/salivo-sdk-public/releases/download/v1.0.26/salivo_1.0.26_amd64.deb) | `sudo apt install ./salivo_1.0.26_amd64.deb` |
| **Fedora / RHEL x64** | [`salivo-1.0.26-1.x86_64.rpm`](https://github.com/Serion89/salivo-sdk-public/releases/download/v1.0.26/salivo-1.0.26-1.x86_64.rpm) | `sudo dnf install ./salivo-1.0.26-1.x86_64.rpm` |
| **Any Linux x64** | [`salivo-sdk-1.0.26-linux-x64.tar.gz`](https://github.com/Serion89/salivo-sdk-public/releases/download/v1.0.26/salivo-sdk-1.0.26-linux-x64.tar.gz) | `tar xzf salivo-sdk-1.0.26-linux-x64.tar.gz && sh salivo-sdk-1.0.26-linux-x64/install.sh` (installs into `~/.salivo`) |
| **macOS** | not available yet | |

Then open a new terminal and run `sf run hello.sal`.

**Prerequisites.** Salivo links programs with clang.

- **Windows:** setup detects LLVM (clang) and the Visual Studio C++ build tools and offers to install them with `winget`. Windows may show "Windows protected your PC" because the installer is not code-signed yet: click **More info > Run anyway**.
- **Linux:** the `.deb` and `.rpm` pull in clang automatically (lld is recommended for WebAssembly builds). For the tarball, install clang 14 or newer first: `sudo apt install clang lld` or `sudo dnf install clang lld`.

**VS Code / Antigravity:** the Windows setup installs the extension into VS Code, Antigravity IDE and Antigravity (whichever are installed). On Linux, the packages ship it in `/opt/salivo/vscode`: `code --install-extension /opt/salivo/vscode/salivo-1.0.23.vsix` (use `antigravity-ide` or `antigravity` instead of `code` for Antigravity).

**Verify the download:** compare it with `SHA256SUMS.txt` from the release (`sha256sum -c SHA256SUMS.txt`, or `Get-FileHash` on Windows).

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

In VS Code, press **Ctrl+F5** to run instantly with sub-30ms execution caching.

---

## Game Engine & Desktop

Salivo includes native, hardware-accelerated 2D and 3D game engine modules in the standard library (`salivo.std.game`, `salivo.std.desktop`, `salivo.std.audio`, `salivo.std.physics2d`):

```salivo
module main;

><salivo.std.desktop::{rgba, KeyLeft, KeyRight};
><salivo.std.game::{
    gameNew, gameOk, gameFrame, gameDelta, gameBegin, gameEnd, gameShutdown,
    keyHeld, drawSprite
};

pub func main() -> int {
    let g = gameNew("Salivo Game Demo", 640, 480);
    if (!gameOk(g)) { return 1; }

    let mut playerX = 320;

    while (gameFrame(g)) {
        let dt = gameDelta(g);
        let speed = 300 * dt / 1000000;

        if (keyHeld(g, KeyLeft))  { playerX = playerX - speed; }
        if (keyHeld(g, KeyRight)) { playerX = playerX + speed; }

        gameBegin(g, rgba(18, 18, 28, 255));
        drawSprite(g, playerX, 420, 64, 16, 0, rgba(80, 200, 255, 255), 0);
        gameEnd(g);
    }

    gameShutdown(g);
    return 0;
}
```

Compiling a complete game produces a **~30 KB** standalone native executable with zero external dependencies:
```bash
sf build game.sal
.\build\game.exe
```

---

## Language tour

### Structs and deterministic cleanup (`Drop`)

Implement `Drop` and cleanup code executes deterministically when a value leaves scope:

```salivo
module shapes;

><salivo.std.core::{outln, Drop};

pub struct Connection {
    int id;
}

impl Drop for Connection {
    func drop(self) {
        outln($"closing connection {self.id}");
    }
}

func serve() -> int {
    let conn = Connection { id: 7 };
    outln($"serving on connection {conn.id}");
    return 0; // conn is dropped here
}

func main() -> int {
    serve();
    outln("done");
    return 0;
}
```

### Error handling with Result

Functions that can fail return `Result<T, E>`:

```salivo
module errors;

><salivo.std.core::{outln, Result, ok, err};

func parseAge(age: int) -> Result<int, string> {
    if (age < 0) {
        return err<int, string>("age cannot be negative");
    }
    return ok<int, string>(age);
}

func main() -> int {
    let r = parseAge(25);
    if (r.is_ok) {
        outln($"valid age: {r.value}");
    } else {
        outln($"rejected: {r.error}");
    }
    return 0;
}
```

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
| `salivo.std.desktop` | Win32 native windowing, hardware OpenGL context, VSync, mouse/keyboard events |
| `salivo.std.game` | High-precision game loop, 2D sprites, 3D meshes, GLSL shaders, camera |
| `salivo.std.audio` | Multi-channel sound effects and music playback |
| `salivo.std.physics2d` | 2D rigidbodies, AABB/circle collisions, raycasts, gravity, restitution |
| `salivo.std.tilemap` | Grid tilemap rendering with camera frustum culling |
| `salivo.std.gui` | Immediate-mode UI widgets (buttons, progress bars, panels) |
| `salivo.std.web` | High-performance HTTP/1.1 & HTTP/2 server, routing, `serveStatic`, JSON |
| `salivo.std.db` | Database connection pooling, SQLite, MySQL, PostgreSQL, MongoDB |
| `salivo.std.sys` | System telemetry, processes, performance counters, hardware facts, text store |
| `salivo.std.ui` | Fine-grained reactive UI signals, effects, templates (WASM browser) |
| `salivo.std.browser` | Browser DOM manipulation and JavaScript FFI |
| `salivo.std.collections` | `Vec`, `HashMap`, `HashSet`, `Deque`, heaps, B-trees |
| `salivo.std.strings` | `String`, slices, `startsWith`, `endsWith`, `padStart`, `pushStr` |
| `salivo.std.runtime` | Async runtime: tasks, channels, timers, cancellation, TCP, UDP, files |
| `salivo.std.fs` / `salivo.std.io` | Files, directories, paths, console I/O |
| `salivo.std.net` | Sockets, addresses, IPv4/IPv6, name resolution |
| `salivo.std.sync` | Mutexes, read-write locks, atomics, channels |
| `salivo.std.time` | Clocks, durations, sleep, formatting |
| `salivo.std.crypto` | SHA-256, SHA-512, BLAKE3, AES, HMAC-SHA256 |
| `salivo.std.mem` | Allocations, raw memory access, `sizeof`, `alignof` |

---

## Tooling

### sf (compiler driver)

```bash
sf run main.sal                     # build and run
sf build main.sal                   # build standalone native executable
sf check main.sal                   # type-check without generating code
sf build -O 3 main.sal              # optimization level 0 to 3 (default 2)
sf build . --target wasm32          # build WebAssembly module
sf build --compiler rust main.sal   # use the Rust back end instead of Stage 3
sf version
```

### spm (package manager)

```bash
spm new my_app                          # create a standard binary package
spm new my_app --lib                    # create a library package
spm new my_app --template fullstack     # create fullstack web app (backend/ + frontend/)
spm new my_app --template web           # create web server
spm new my_app --template game          # create desktop game
spm build                               # build the package or workspace
spm run                                 # build and run its binary
spm run <script>                        # execute a script from salivo.toml [scripts]
spm test                                # run unit and integration tests
```

### Code quality

```bash
salivofmt --write src/     # format in place (--check for CI, --diff to preview)
salivolint src/            # static analysis
salivodoc src/             # generate documentation
```

### VS Code & Antigravity IDE Extension

Installed automatically by the Windows setup (on Linux: `code --install-extension /opt/salivo/vscode/salivo-1.0.26.vsix`):
- Full syntax highlighting for `><`, `Drop`, and standard library APIs.
- Embedded `salivo-lsp` language server for real-time error checking and hovers.
- Instant shortcuts: **Ctrl+F5** warm execution, **Ctrl+Shift+B** build, **Ctrl+Shift+K** check, **Shift+Alt+F** format.
- Snippets for `main`, `func`, `struct`, `impl`, `drop`, `match`, `webrouter`, `sqlite`, and game loops.

---

## How compilation works

`sf` parses and type-checks every program with its own front end, ensuring uniform diagnostics. The self-hosted Stage 3 compiler lowers the program through HIR, MIR and SSA, executes ownership analysis and the optimization pipeline (SCCP constant propagation, GVN value numbering, and LICM loop motion), and emits LLVM IR, which clang compiles and links against the Salivo runtime.

Each program is compiled as a whole: functions are inlined across user code and the runtime, and unused code is eliminated before code generation. That is why builds complete in ~250 ms and executables stay under 30 KB.

---

## Platform support

| Platform | Status |
| :--- | :--- |
| Windows 10/11 x64 | Supported: setup `.exe`, VS Code extension, file icons. |
| Ubuntu 22.04+ / Debian 12+ x64 | Supported: `.deb` package. |
| Fedora 39+ / RHEL 9+ x64 | Supported: `.rpm` package. |
| Other Linux x64 (glibc 2.35+) | Supported: `.tar.gz` with `install.sh`. |
| Web / WebAssembly (wasm32) | Supported via `sf build --target wasm32` with DOM and reactive UI support. |
| macOS (Apple Silicon and Intel) | Coming soon. |

---

## Troubleshooting

- **"sf is not recognized" / "command not found"**: open a new terminal after installing (PATH changes apply to new windows).
- **`.sal` files lost the Salivo icon (Windows)**: run the setup again.
- **PowerShell syntax with a quoted path**: prepend `&`: `& "C:\Users\you\.salivo\bin\sf.exe" run file.sal`.
- **Linker errors**: make sure clang is installed (and on Windows the Visual Studio C++ build tools).
- **Uninstall**: Windows: *Settings > Apps > Salivo > Uninstall*. Ubuntu/Debian: `sudo apt remove salivo`. Fedora: `sudo dnf remove salivo`. Tarball: `sh ~/.salivo/uninstall.sh`.

---

## Repository structure

```
salivo-sdk-public/
|-- packaging/     WinGet manifest
|-- get.ps1        one-line Windows installer
|-- get.sh         one-line Linux installer
|-- assets/        logo
|-- README.md
|-- TRADEMARK.md
|-- THIRD_PARTY_NOTICES.txt
`-- LICENSE
```

The installers contain the compiled toolchain, the standard library and prebuilt runtime libraries. The compiler source code is not published.

---

## Licensing and Usage Policy

Salivo is distributed under the **Business Source License 1.1** (see [LICENSE](LICENSE)).

### For Developers and Businesses

- **Free for everyone:** 100% free for any purpose, including commercial use. Build, sell and operate applications and services written in Salivo and keep the revenue. There is no revenue limit and no team-size limit.
- **Your Software Belongs to You:** Any application, game, web service, or library authored in Salivo is entirely your property. You may distribute, sell, or license your output binaries and code under any license without royalties or fees.
- **Open-Source Transition:** Each version converts to the Apache License, Version 2.0 four years after its own release, so the newest code is always protected.

### Managed Cloud Service Policy

- **Can cloud hosting providers offer Salivo as a managed cloud service or hosted compiler platform?**  
  **No.** You may not provide the Salivo compiler, runtime, or developer toolchain as a hosted, managed, or multi-tenant cloud service (including but not limited to "Compiler-as-a-Service", "Salivo-as-a-Service", or remote cloud development environments) to third parties without a commercial license agreement from the Licensor. Reselling or rebranding Salivo as a competing language or toolchain product also requires a commercial license.

- **Can I deploy web servers, APIs, or products I built in Salivo to the cloud?**  
  **Yes.** You are free to host and run any software, web application, API, or service that you developed using Salivo on any infrastructure or cloud provider (AWS, GCP, Azure, bare-metal, or self-hosted).

### Commercial Licensing and Enterprise Support

Cloud providers, hosted-service operators and anyone reselling Salivo as a competing product must obtain a commercial license.

For commercial inquiries, licensing quotes, and dedicated support contracts:
- Email: `sahildark789@gmail.com`
- Repository: `https://github.com/Serion89/salivo`

### Trademark Notice

"Salivo", the Salivo logo, and the names of its tools are trademarks of the Licensor. Forks and modified builds must use a different name. See [TRADEMARK.md](TRADEMARK.md).

---

## Author & Maintainer

<table>
  <tr>
    <td width="90" align="center" valign="middle">
      <a href="https://github.com/Serion89">
        <img src="https://avatars.githubusercontent.com/u/205588838?v=4" width="80" height="80" style="border-radius: 50%;" alt="Sahil Bhatt" />
      </a>
    </td>
    <td valign="middle">
      <h3>Sahil Bhatt</h3>
      <p>Software Developer &bull; Compiler &amp; Systems Engineer &bull; AI</p>
      <p>
        <a href="https://github.com/Serion89">
          <img src="https://img.shields.io/badge/GitHub-Serion89-181717?style=flat-square&logo=github" alt="GitHub Profile" />
        </a>
        &nbsp;
        <a href="https://github.com/Serion89">
          <img src="https://img.shields.io/github/followers/Serion89?label=Followers&style=flat-square&logo=github&color=24292e" alt="Followers" />
        </a>
      </p>
    </td>
  </tr>
</table>