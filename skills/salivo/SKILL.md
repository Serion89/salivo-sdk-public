---
name: salivo
description: The single source of truth and master architecture guidelines for the Salivo programming language. Enforces stream imports (><), PascalCase types with zero underscores, flexible camelCase/snake_case functions and fields, no runtime prefix leakage, postfix try operator (?), RAII Drop trait, loops, template interpolation, and standard library APIs.
---

# Salivo AI Agent Master Guidelines & Architecture Laws

> **CRITICAL DIRECTIVE FOR ALL AI AGENTS / ASSISTANTS:**
> This document is the **single source of truth** for all automated agents, language models, and developers working on the Salivo codebase. Adhere strictly to every rule, syntax standard, and environment protocol defined here.

---

## 1. What Was Changed & Why (Recent Language Modernizations)

The Salivo compiler, standard library, type system, and developer tooling were upgraded with 6 core architectural improvements. All AI agents must know **what changed** and the **technical rationale (why)**:

### 1.1 Exclusive Stream Header Imports (`><`)
- **What Changed:** Standardized exclusively on `><` as the sole import operator in the language (e.g. `><salivo.std.core::{assert, assertEq};` and `><salivo.std.fs::{pathJoin, pathExists};`). The legacy `import` keyword has been deprecated across the parser, semantic analyzer, linter, and standard library.
- **Why:** Salivo's identity centers on clean, expressive data streams. Having dual import syntaxes (`import` vs `><`) caused fragmentation. Standardizing on `><` provides visual consistency and unique language character.

### 1.2 Standard Library Naming & Identifier Flexibility
- **What Changed:** Standard library functions support readable `camelCase` (`pathJoin`, `pathExists`, `assertEq`, `assertStrEq`, `sha256Hex`, `pushStr`, `stringNew`, `readByte`, `secSeed`, `randBytes`, `vecNew`, `mapNew`, `heapNew`, `btreeNew`) alongside snake_case / single-word aliases (`path_join`, `pathjoin`, `assert_eq`, `asserteq`, `sha256hex`, etc.) for 100% backwards compatibility with all test suites.
- **Why:** Identifiers like `assertstringeq` or `pathjoin` reduced readability. Modern `camelCase` and snake_case offer clear readability and smooth IDE subword navigation (`Ctrl + Left/Right`).

### 1.3 String Literal Auto-Coercion (`"str"` $\rightarrow$ `StrSlice` / `String`)
- **What Changed:** Raw string literals (`"hello"`, `SemanticType::String`) automatically coerce into borrowed `StrSlice` or owned `String` in variable declarations, struct instantiations, and function parameters during semantic analysis and HIR lowering.
- **Why:** Previously, developers had to manually wrap string literals with `cStringView("hello")` or `fromCString("hello")` when calling standard library functions expecting `StrSlice` or `String`. Auto-coercion eliminates boilerplate and makes string handling seamless.

### 1.4 Postfix `?` Error Propagation / Try Operator
- **What Changed:** Added `Expression::Try` (`expr?`) to the AST, Pratt parser, semantic analyzer, HIR lowering, and CLI formatter. `expr?` unwraps `Result::Ok(v)` / `Option::Some(v)` or early-returns `Result::Err(e)` / `Option::None` from the enclosing function.
- **Why:** Drastically reduces nested `match` boilerplate for fallible I/O, file system operations, and network calls (modern Rust/Swift/Kotlin ergonomics).

### 1.5 Deterministic Resource Cleanup (`pub trait Drop`)
- **What Changed:** Added `pub trait Drop { func drop(self); }` to `salivo.std.core` and integrated scope-based destruction hooks into the SSA optimizer and ARC runtime.
- **Why:** Guarantees deterministic RAII cleanup for file descriptors, memory buffers, mutex guards, and socket handles upon scope exit without manual `.close()` or `.free()` calls.

### 1.6 Syntax Highlighting & Tooling Refresh
- **What Changed:** Updated TextMate grammar (`syntaxes/salivo.tmLanguage.json` and `editors/vscode/syntaxes/salivo.tmLanguage.json`) to color `><`, `Drop`, `StrSlice`, `ByteSlice`, `String`, `Directory`, `FileMetadata`, and modern `camelCase` methods.
- **Why:** First-class IDE developer experience with vivid token highlighting.

---

## 2. Type System Integrity & Identifier Guidelines (Code Laws)

1. **Zero-Underscore Standard on Data Types:**
   - Structs, enums, traits, and type annotations must **never** contain underscores (`_`). They strictly use clean PascalCase (`Point`, `Option`, `Result`, `FileMetadata`, `AuthToken`, `UserProfile`).
   - `_` is strictly **never** a datatype (e.g., no `let x: _ = 10;`, no `func foo(a: _) -> _`).
   - Struct fields are member bindings governed by Rule 2.2 below (supporting both `userId` and `user_id`).
   - ❌ **Banned in Types:** `struct user_profile`, `enum error_code`, `let x: _ = 5`.
   - ✅ **Correct in Types:** `struct UserProfile`, `enum ErrorCode`, `let x: int = 5`.

2. **Flexible Function, Variable, Struct Field, & Expression Naming:**
   - Functions, method names, variable bindings, and struct fields **can** freely use underscores or camelCase (`path_join` or `pathJoin`, `assert_eq` or `assertEq`, `user_id` or `userId`, `max_line_width` or `maxLineWidth`, `read_byte`). Both forms are first-class and interoperable across the standard library, tooling, and user code.
   - Unused discards (`let _ = expr;` or `_unused`) and pattern match wildcards (`match val { _ => ... }`) are fully supported.
   - Numeric literals support digit separators with underscores for readability (e.g., `1_000_000`, `0xFF_AA_00`).

3. **No Internal Prefix Leakage:**
   - Never expose internal runtime prefixes (`salivo_`, `sal_`, `crt_`, `__`) to user-facing Salivo code, standard library public APIs, or user documentation.
   - **Boundary Definition:** Low-level LLVM symbols (e.g. `salivo_alloc`, `salivo_deallocate`), runtime C headers, and compiler HIR/SSA lowering routines legitimately use internal prefixes. However, public Salivo APIs and stdlib exports must expose clean, un-prefixed interfaces (`alloc`, `deallocate`, `pathJoin`, `sha256Hex`).

---

## 3. Environment & Binary Location Law

- **Active Shell Execution Path:** The user's system and VS Code execute `sf` and related tools directly from:
  ```
  C:\Users\sahil\.salivo\bin\sf.exe
  ```
- **Mandatory Synchronization Protocol:**
  Whenever you build or update the compiler or CLI (`cargo build --release` or `cargo build`):
  1. ALWAYS copy `sf.exe`, `spm.exe`, `salivofmt.exe`, `salivolint.exe`, `salivodoc.exe`, and `salivo-lsp.exe` from `target/release/` to `C:\Users\sahil\.salivo\bin\`.
  2. ALWAYS copy updated standard library files from `c:\salivo\std\` to `C:\Users\sahil\.salivo\lib\salivo\std\` and `C:\Users\sahil\.salivo\std\`.
- **Debugging Protocol:**
  If `sf run` reports missing built-ins or runtime discrepancies, verify and refresh the active binaries in `C:\Users\sahil\.salivo\bin\` before modifying compiler source files.

---

## 4. Salivo Language Quick Reference for Agents

### 4.1 Module & Imports
```salivo
module myapp.services.auth;

// Stream Header Import Syntax (Canonical)
><salivo.std.core::{assert, assertEq, assertStrEq, Option, Result, some, none, ok, err};
><salivo.std.fs::{pathJoin, pathExists, open, close};
><salivo.std.io::{readStr, writeStr};
><salivo.std.crypto::{sha256Hex, secSeed};
><salivo.std.strings::{stringNew, pushStr, asStr};
><salivo.std.collections::{vecNew, push, get, mapNew, put, lookup};
```

### 4.2 Variables & Mutability
```salivo
let name = "Salivo";        // Immutable binding
let mut counter = 0;        // Mutable binding
counter += 1;

const MAX_BUFFER = 8192;    // Compile-time constant
global let PORT = 8080;     // Package-scoped global
```

### 4.3 Control Flow & Loops
```salivo
// 3-clause C-style For Loop
for (let mut i = 0; i < 10; i = i + 1) {
    if (i == 5) { continue; }
    outln($"Index: {i}");
}

// While Loop
while (condition) { ... }

// Pattern Matching (Strictly Exhaustive)
match status {
    0 => "initial",
    1 => "running",
    _ => "unknown",
}
```

### 4.4 Functions & Error Propagation (`?`)
```salivo
pub func readFileSafe(path: string) -> Result<string, IoError> {
    let handle = open(path, "r")?; // Auto error propagation
    let content = readStr(path)?;
    return ok<string, IoError>(content);
}
```

### 4.5 Structs, Enums, Traits & Drop
```salivo
pub struct Point { x: int, y: int }

pub enum Status { Pending, Active, Done }

pub trait Show {
    func show(self) -> string;
}

pub trait Drop {
    func drop(self);
}

impl Show for Point {
    func show(self) -> string {
        return $"Point({self.x}, {self.y})";
    }
}

impl Drop for Point {
    func drop(self) {
        outln("Cleaning up Point resources");
    }
}
```

### 4.6 Postfix Modifiers & String Interpolation
- Postfix converters: `:int`, `:float`, `:str`, `:trim`, `:upper`, `:lower`, `:slice(start, end)`, `:len`
- Template interpolation: `$"User: {name:trim:upper}, ID: {id:str}, Score: {score:float}"`

---

## 5. Standard Library API Reference Map (`salivo.std.*`)

| Module | Primary Modern APIs (`camelCase`) | Backwards-Compatible Aliases |
| :--- | :--- | :--- |
| **`salivo.std.core`** | `assertEq`, `assertStrEq`, `assertNe`, `assertStrNe`, `panic`, `assert`, `out`, `outln`, `Option`, `Result`, `some`, `none`, `ok`, `err`, `trait Drop` | `asserteq`, `assertstreq`, `assertne`, `assertstrne` |
| **`salivo.std.fs`** | `pathJoin`, `pathExists`, `isOpen`, `readByte`, `dirExists`, `dirList`, `open`, `close`, `flush`, `meta`, `read`, `write` | `pathjoin`, `pathexists`, `isopen`, `readbyte`, `direxists`, `dirlist` |
| **`salivo.std.io`** | `readStr`, `writeStr`, `print`, `println`, `flush` | `readstr`, `writestr` |
| **`salivo.std.crypto`**| `sha256Hex`, `secSeed`, `randBytes`, `sha256`, `sha512`, `blake3`, `hmac`, `aes128`, `aes256`, `entropy` | `sha256hex`, `secseed`, `randbytes` |
| **`salivo.std.strings`**| `stringNew`, `stringWithCap`, `asStr`, `fromStr`, `fromCString`, `toCString`, `cStringView`, `pushStr`, `pushByte`, `startsWith`, `endsWith`, `padStart`, `padEnd`, `assertStringEq`, `assertSliceEq` | `stringnew`, `stringwithcap`, `asstr`, `fromstr`, `pushstr`, `pushbyte`, `assertstringeq`, `assertsliceeq` |
| **`salivo.std.collections`**| `vecNew`, `vecCap`, `push`, `pop`, `get`, `set`, `veclen`, `mapNew`, `put`, `has`, `lookup`, `maplen`, `heapNew`, `heapPush`, `heapPop`, `deqNew`, `pushBack`, `popFront`, `btreeNew`, `btSetNew` | `vecnew`, `veccap`, `mapnew`, `heapnew`, `deqnew`, `pushback`, `popfront`, `btreenew`, `btsetnew` |
| **`salivo.std.net`**  | `ipv4`, `ipv6`, `sockaddr`, `parseip`, `resolve`, `tcpsendstr`, `tcprecvstr`, `shutdown`, `socket`, `bind`, `listen`, `accept`, `connect` | Low-level socket intrinsics |
| **`salivo.std.sync`** | `mutex`, `lock`, `unlock`, `rwlock`, `readlock`, `unlockread`, `atomic`, `load`, `store`, `swap`, `fetchadd`, `cas`, `channel`, `send`, `recv` | Single-word built-ins |
| **`salivo.std.time`** | `epoch`, `now`, `fromms`, `duration`, `addduration`, `sleep`, `format`, `parse` | Single-word built-ins |
| **`salivo.std.math`** | `abs`, `min`, `max`, `clamp`, `pow`, `sqrt`, `seed`, `rng`, `range`, `sin`, `cos`, `tan` | Single-word built-ins |
| **`salivo.std.mem`**  | `alloc`, `dealloc`, `realloc`, `alloczero`, `isallocated`, `arcnew`, `readi64`, `writei64`, `readbyte`, `writebyte`, `slicelen`, `sliceget`, `sliceset` | Single-word built-ins |

---

## 6. Compiler Architecture & Testing Protocols

- **Compiler Pipeline:**
  `Source (.sal)` $\rightarrow$ `Lexer` $\rightarrow$ `Pratt Parser (AST)` $\rightarrow$ `Semantic Analyzer (Types & Scopes)` $\rightarrow$ `HIR Lowering` $\rightarrow$ `Optimization Passes (ARC, DCE, Bounds)` $\rightarrow$ `MIR` $\rightarrow$ `LLVM Native / JIT`.
- **Testing Verification:**
  - Full workspace test command: `cargo test --workspace`
  - Integration suite command: `cargo test -p salivo_cli`
  - All modifications must maintain **100% test pass rate with 0 failures**.
- **Documentation Build:**
  - Run `python c:\salivo\scratch\build_docs.py` to regenerate `SALIVO_BOOK.html`, `index.html`, and `SALIVO_BOOK.pdf`.

---

## 7. Backend, Networking & Microservices Guide

### 7.1 Networking: `salivo.std.net`
```salivo
><salivo.std.net::{ipv4, sockaddr, parseip, tcpsendstr, tcprecvstr, TcpStream, NetworkError};

let ip = parseip("127.0.0.1");
let addr = sockaddr(ip, 8080);
```

### 7.2 Web Framework: `@salivo/web`
```salivo
><salivoweb;

let mut router = salivoweb.router();
router = salivoweb.get(router, "/health", "OK");
router = salivoweb.getjson(router, "/users/:id", "{\"id\": {id}}");
router = salivoweb.post(router, "/api/v1/items", "{\"status\": \"created\"}");
salivoweb.listen(router, 8080);
```

### 7.3 Embedded Database: `@salivo/sqlite`
```salivo
><salivosqlite;

let conn = salivosqlite.openmem()?.unwrap();
let _ = salivosqlite.execute(conn, "CREATE TABLE users (id INT, name TEXT);");
```

### 7.4 Package Management: `spm`
- `spm new <app>`: Create new Salivo package
- `spm add <pkg>`: Add package dependency (e.g. `@salivo/web`, `@salivo/sqlite`)
- `spm build`: Compile package
- `spm test`: Run package unit & integration tests
- `spm run`: Execute package main binary

### 7.5 Claude MCP Backend Tools
Claude has native tools available in chat:
- `spm`: Run package management operations
- `httpRequest`: Test local and remote HTTP/REST endpoints directly
- `scaffoldBackend`: Generate `web_api`, `tcp_echo`, or `sqlite_crud` starters
- `backendDocs`: Detailed API docs for `web`, `http`, `sqlite`, `json`, `async`, `net`
- `run`, `check`, `fmt`, `lint`, `version`, `stdlibInfo`

