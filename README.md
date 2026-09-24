<div align="center">

<img src="salivo_logo.png" alt="Salivo Logo" width="160" />

# Salivo Native SDK & Toolchain

**High-Performance, Memory-Safe Systems Programming Language**  
*Stream Imports (><) | Deterministic RAII | Native LLVM Codegen | Zero-Cost Collections | Universal AI-Ready*

[![Version](https://img.shields.io/badge/version-v1.0.9-blue.svg)](https://github.com/Serion89/salivo-sdk-public)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-brightgreen.svg)](https://github.com/Serion89/salivo-sdk-public)
[![License](https://img.shields.io/badge/license-MIT-purple.svg)](LICENSE)
[![LLVM](https://img.shields.io/badge/backend-LLVM%2016-red.svg)](https://llvm.org)
[![AI-Ready](https://img.shields.io/badge/AI-Universal%20Ready-orange.svg)](#universal-ai-integration)

</div>

---

## Overview

**Salivo** is a compiled, statically-typed systems programming language engineered for high throughput, predictable microsecond latencies, and modern developer ergonomics. It compiles directly to bare-metal native machine code via LLVM while featuring a clean, readable syntax.

This repository provides the official standalone **Salivo SDK & Toolchain for Windows x64**, containing the compiler, package manager, standard library, language server (LSP), static analysis tools, VS Code extension, and pre-configured rulebooks for all major AI coding agents.

---

## Quick Start (1-Click Install)

1. Download **`salivo-sdk-1.0.9-windows-x64.zip`** from the
   [latest release](https://github.com/Serion89/salivo-sdk-public/releases/latest) and extract it.
2. Double-click **`Salivo Setup.exe`** (it carries the Salivo logo). Windows may say "Windows protected
   your PC" because the installer is not code-signed: click **More info > Run anyway**.
3. Open a new terminal or restart VS Code, then run `sf run examples/hello.sal`.

Setup (no admin rights needed):
- Installs the toolchain into `%USERPROFILE%\.salivo` and adds `sf` to your user `PATH`.
- Installs the **self-hosted Stage 3 compiler** that `sf` uses by default, with its runtime.
- Installs the **VS Code extension** (syntax highlighting, snippets, file icons, Ctrl+F5 run, Ctrl+Shift+B build).
- Gives `.sal` files the Salivo icon in File Explorer.
- Finishes by compiling and running a test program.

**Prerequisites.** Salivo produces native executables with clang and the Microsoft C++ libraries.
Setup detects both and offers to install whichever is missing:
- LLVM (clang): `winget install -e --id LLVM.LLVM`
- Visual Studio 2022 C++ build tools:
  `winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`

Cloning this repository also works (`install.bat` runs the same installer), but the release zip is the
complete SDK: it also contains `bin/LLVM-C.dll`, which is too large to keep in git.

To uninstall: `powershell -ExecutionPolicy Bypass -File uninstall.ps1`.

---

## Toolchain & Package Contents

| Tool | Executable | Description |
| :--- | :--- | :--- |
| **Compiler Driver** | `bin/sf.exe` | Builds and runs programs; checks them and hands code generation to Stage 3 |
| **Stage 3 Compiler** | `bin/salivoc.exe` | The self-hosted Salivo compiler (written in Salivo) that generates native code |
| **Package Manager** | `bin/spm.exe` | Dependency resolver, workspace coordinator, and project scaffold |
| **Language Server** | `bin/salivo-lsp.exe` | Real-time diagnostics, autocomplete, definitions, and hover docs |
| **Linter** | `bin/salivolint.exe` | Static safety checks, dead code analysis, and style enforcement |
| **Formatter** | `bin/salivofmt.exe` | Canonical AST-aware code formatting |
| **Documentation** | `bin/salivodoc.exe` | Automated markdown & HTML documentation generator |
| **Standard Library** | `std/` | Production standard library (Collections, IO, Net, Concurrency, etc.) |
| **Stage 3 Runtime** | `stage3/` | Runtime sources and prebuilt objects linked into every program |
| **VS Code Extension** | `vscode/salivo-1.0.9.vsix` | Syntax highlighting, snippets, file icons, and run/build/check commands |
| **AI Rulebooks** | `.cursorrules`, `CLAUDE.md`, `AGENTS.md` | Pre-trained guidelines for any AI pair-programmer |

---

## Language Tour & Code Examples

### 1. Hello World with Stream Imports (><)

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

### 4. Zero-Cost Collections (Vec, HashMap, Deque)

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

## Universal AI Integration

Salivo is designed from day one to be directly understood by all modern AI pair programmers. This repository includes dedicated context rules for every major AI coding tool:

* **Cursor IDE**: Pre-configured `.cursorrules` teaches Claude 3.7 and GPT-4o Salivo syntax rules.
* **Claude Code (CLI)**: Native `CLAUDE.md` provides build commands and API conventions.
* **GitHub Copilot**: `.github/copilot-instructions.md` provides instruction rules for VS Code.
* **Windsurf**: `.windsurfrules` guides Cascade on idiomatic Salivo code.
* **Antigravity IDE**: Global rules and knowledge items are automatically installed via `install.bat`.
* **Cline / Roo Code / Aider / Devin**: Automatically pick up `AGENTS.md` at repository root.
* **ChatGPT / Claude Web / Gemini**: Drag and drop `docs/salivo_quick_reference.md` into any chat session to give the model full fluency in Salivo.

---

## CLI Reference

### sf (Compiler Driver)

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

### spm (Package Manager)

```bash
# Create a new Salivo project
spm new my_project

# Build project dependencies and binaries
spm build

# Run tests
spm test
```

### salivofmt & salivolint (Code Quality)

```bash
# Format source files in-place
salivofmt --write src/

# Run static analysis and linting
salivolint src/
```

---

## Repository Structure

```
salivo-sdk-public/
|-- .github/              # Copilot rules
|-- assets/               # Official brand graphics & icons
|-- bin/                  # Precompiled 64-bit Windows executables
|   |-- sf.exe            # Compiler driver
|   |-- salivoc.exe       # Self-hosted Stage 3 compiler
|   |-- spm.exe           # Package manager
|   |-- salivofmt.exe     # Canonical code formatter
|   |-- salivolint.exe    # Linter and static analyzer
|   |-- salivodoc.exe     # Documentation generator
|   `-- salivo-lsp.exe    # Language Server Protocol engine
|-- commands/             # Universal agent slash commands
|-- docs/                 # Language guides, references & manuals
|-- examples/             # hello.sal
|-- mcp/                  # Model Context Protocol server
|-- skills/               # Reusable AI agent skills
|-- stage3/               # Stage 3 runtime sources and prebuilt objects
|-- std/                  # Official Salivo Standard Library
|-- vscode/               # VS Code extension (.vsix)
|-- .cursorrules          # Cursor AI configuration
|-- .windsurfrules        # Windsurf configuration
|-- AGENTS.md             # Autonomous agent instruction set
|-- CLAUDE.md             # Anthropic Claude Code configuration
|-- Salivo Setup.exe      # 1-click installer (runs install.ps1)
|-- install.ps1           # Installer
|-- install.bat           # Runs install.ps1
|-- uninstall.ps1         # Uninstaller
|-- salivo_logo.png       # Official emblem
`-- README.md             # This document
```

---

## License

The Salivo SDK and Standard Library are distributed under the open-source **MIT License**.  
See the [LICENSE](LICENSE) file for details.

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
