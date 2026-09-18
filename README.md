<div align="center">

<img src="salivo_logo.png" alt="Salivo Logo" width="160" />

# Salivo Native SDK & Toolchain

**High-Performance, Memory-Safe Systems Programming Language**  
*Stream Imports (`><`) • Deterministic RAII • Native LLVM Codegen • Zero-Cost Collections • Universal AI-Ready*

[![Version](https://img.shields.io/badge/version-v0.1.0-blue.svg)](https://github.com/Serion89/salivo-sdk-public)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-brightgreen.svg)](https://github.com/Serion89/salivo-sdk-public)
[![License](https://img.shields.io/badge/license-MIT-purple.svg)](LICENSE)
[![LLVM](https://img.shields.io/badge/backend-LLVM%2018-red.svg)](https://llvm.org)
[![AI-Ready](https://img.shields.io/badge/AI-Universal%20Ready-orange.svg)](#universal-ai-integration)

</div>

---

## 🚀 Overview

**Salivo** is a compiled, statically-typed systems programming language engineered for high throughput, predictable microsecond latencies, and modern developer ergonomics. It compiles directly to bare-metal native machine code via LLVM while featuring a clean, readable syntax.

This repository provides the official standalone **Salivo SDK & Toolchain for Windows x64**, containing the compiler, package manager, standard library, language server (LSP), static analysis tools, VS Code extension, and pre-configured rulebooks for all major AI coding agents.

---

## ⚡ Quick Start (1-Click Install)

### Option 1: Automated 1-Click Installer (Recommended)

1. Clone or download this repository.
2. Double-click **`install.bat`**.
3. Reopen your terminal or VS Code.

`install.bat` automatically:
- Installs the Salivo toolchain to `%USERPROFILE%\.salivo\bin\`.
- Adds Salivo permanently to your Windows User `PATH`.
- Deploys the official standard library to `%USERPROFILE%\.salivo\lib\salivo\std\`.
- Installs the official Salivo VS Code extension into `%USERPROFILE%\.vscode\extensions\`.
- Globally trains your local AI coding assistants (Antigravity, Cursor, Copilot, Windsurf, Claude Code).

### Option 2: Manual Installation (30 Seconds)

1. Add the `bin/` directory from this repository to your system `PATH`:
   ```powershell
   [Environment]::SetEnvironmentVariable("Path", $env:Path + ";C:\path\to\salivo-sdk-public\bin", "User")
   ```
2. Verify the installation:
   ```bash
   sf version
   ```

---

## 📦 What's in the Box?

| Tool | Executable | Description |
| :--- | :--- | :--- |
| **Compiler Driver** | `bin/sf.exe` | Multi-stage optimizing compiler, JIT runner, and LLVM codegen |
| **Package Manager** | `bin/spm.exe` | Dependency resolver, workspace coordinator, and project scaffold |
| **Language Server** | `bin/salivo-lsp.exe` | Real-time diagnostics, autocomplete, definitions, and hover docs |
| **Linter** | `bin/salivolint.exe` | Static safety checks, dead code analysis, and style enforcement |
| **Formatter** | `bin/salivofmt.exe` | Canonical AST-aware code formatting |
| **Documentation** | `bin/salivodoc.exe` | Automated markdown & HTML documentation generator |
| **Standard Library** | `std/` | Production standard library (Collections, IO, Net, Concurrency, etc.) |
| **VS Code Extension** | `vscode-extension/` | Official syntax highlighting, snippets, and 1-click execution |
| **AI Rulebooks** | `.cursorrules`, `CLAUDE.md`, `AGENTS.md` | Pre-trained guidelines for any AI pair-programmer |

---

## 💻 Language Tour & Code Examples

### 1. Hello World with Stream Imports (`><`)

Salivo uses explicit stream headers (`><`) for importing modules and binding specific items into scope:

```salivo
module main;

><salivo.std.core::{outln};

func main() -> int {
    outln("Hello, Salivo!");
    return 0;
}
```

Compile and run in one command:
```bash
sf run hello.sal
```

---

### 2. Deterministic RAII & Drop Trait

Salivo manages memory deterministically without a garbage collector. Types can implement `Drop` to automatically reclaim resources when they go out of scope:

```salivo
module demo.raii;

><salivo.std.core::{outln};

struct FileBuffer {
    handle: int,
    size: int
}

trait Drop {
    func drop(mut this) -> void;
}

impl Drop for FileBuffer {
    func drop(mut this) -> void {
        outln($"[RAII] Automatically closing buffer handle {this.handle}");
    }
}

func process() -> void {
    let buf = FileBuffer { handle: 42, size: 1024 };
    // 'buf' is automatically dropped here with zero runtime overhead
}
```

---

### 3. Lightweight Concurrency & Channels

Salivo features native OS thread pools and typed channel communication:

```salivo
module demo.concurrency;

><salivo.std.core::{outln};
><salivo.std.sync::{Channel, channel_create, channel_send, channel_recv};
><salivo.std.threads::{spawn, join};

func worker(ch: Channel<int>) -> void {
    channel_send(ch, 1337);
}

func main() -> int {
    let ch = channel_create<int>();
    let handle = spawn(func() { worker(ch); });
    
    let value = channel_recv(ch);
    outln($"Received from worker: {value}");
    
    join(handle);
    return 0;
}
```

---

### 4. Zero-Cost Collections (`Vec`, `HashMap`, `Deque`)

```salivo
module demo.collections;

><salivo.std.core::{outln};
><salivo.std.collections::{Vec, vecNew, push, get, len};

func main() -> int {
    let mut numbers = vecNew<int>();
    numbers = push<int>(numbers, 10);
    numbers = push<int>(numbers, 20);
    numbers = push<int>(numbers, 30);

    let mut i = 0;
    while (i < len<int>(numbers)) {
        outln($"Item {i} = {get<int>(numbers, i)}");
        i = i + 1;
    }
    return 0;
}
```

---

## 🤖 Universal AI Integration

Salivo is the first programming language designed from day one to be **100% understood by all modern AI pair programmers**. This repository includes dedicated context rules for every major AI coding tool:

* **Cursor IDE**: Pre-configured `.cursorrules` teaches Claude 3.7 / GPT-4o Salivo syntax rules.
* **Claude Code (CLI)**: Native `CLAUDE.md` provides build commands and API conventions.
* **GitHub Copilot**: `.github/copilot-instructions.md` provides instruction rules for VS Code.
* **Windsurf**: `.windsurfrules` guides Cascade on idiomatic Salivo code.
* **Antigravity IDE**: Global rules and knowledge items are automatically installed via `install.bat`.
* **Cline / Roo Code / Aider / Devin**: Automatically pick up `AGENTS.md` at repository root.
* **ChatGPT / Claude Web / Gemini**: Drag and drop `docs/salivo_quick_reference.md` into any chat session to give the model full fluency in Salivo.

---

## 🛠️ CLI Reference

### `sf` (Compiler Driver)

```bash
# Build a native executable
sf build main.sal

# Build with maximum LLVM optimizations (-O3, native target)
sf build -O3 --native main.sal

# Build and immediately execute
sf run main.sal

# Inspect compiler intermediate representations
sf tokens main.sal    # Token stream
sf ast main.sal       # Abstract Syntax Tree
sf hir main.sal       # High-Level IR
sf mir main.sal       # Mid-Level IR
sf ssa main.sal       # Single Static Assignment IR
sf ir main.sal        # LLVM IR
```

### `spm` (Package Manager)

```bash
# Create a new Salivo project
spm new my_project

# Build project dependencies and binaries
spm build

# Run tests
spm test
```

### `salivofmt` & `salivolint` (Code Quality)

```bash
# Format source files in-place
salivofmt --write src/

# Run static analysis and linting
salivolint src/
```

---

## 📂 Repository Structure

```
salivo-sdk-public/
├── .github/              # GitHub workflows & Copilot rules
├── assets/               # Official brand graphics & icons
├── bin/                  # Precompiled 64-bit Windows executables
│   ├── sf.exe            # Compiler driver & JIT runner
│   ├── spm.exe           # Package manager
│   ├── salivofmt.exe     # Canonical code formatter
│   ├── salivolint.exe    # Linter and static analyzer
│   ├── salivodoc.exe     # Documentation generator
│   └── salivo-lsp.exe    # Language Server Protocol engine
├── commands/             # Universal agent slash commands
├── docs/                 # Language guides, references & manuals
│   ├── SALIVO_GUIDE.pdf  # Comprehensive language book (PDF)
│   ├── SALIVO_GUIDE.html # Interactive web guide
│   └── salivo_quick_reference.md # Complete syntax cheat sheet
├── mcp/                  # Model Context Protocol server
├── skills/               # Reusable AI agent skills
├── std/                  # Official Salivo Standard Library
│   ├── core.sal          # Primitives, formatting, exit, assertions
│   ├── collections.sal   # Vec, HashMap, Deque, HashSet
│   ├── fs.sal            # File system, directories, paths
│   ├── io.sal            # Console I/O, buffered readers
│   ├── net.sal           # TCP/UDP sockets, client/server
│   ├── sync.sal          # Mutex, RWLock, Channels, Atomics
│   ├── crypto.sal        # SHA-256, hashing routines
│   └── time.sal          # High-resolution timers and sleep
├── vscode-extension/     # Official VS Code extension package
├── .cursorrules          # Cursor AI configuration
├── .windsurfrules        # Windsurf configuration
├── AGENTS.md             # Autonomous agent instruction set
├── CLAUDE.md             # Anthropic Claude Code configuration
├── install.bat           # 1-click Windows automated installer
├── salivo_logo.png       # Official emblem
└── README.md             # This document
```

---

## 📜 License

The Salivo SDK and Standard Library are distributed under the open-source **MIT License**.  
See the [LICENSE](LICENSE) file for details.

Developed with ❤️ by **Sahil Bhatt** ([@Serion89](https://github.com/Serion89)).
