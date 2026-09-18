# The Complete Salivo Language Guide & Quick Reference

Welcome to the definitive guide for **Salivo** — a high-performance systems programming language featuring native data stream headers (`><`), deterministic RAII memory management (`pub trait Drop`), and clean modern syntax.

---

## 1. Stream Header Imports (`><`)

Salivo replaces legacy `import` statements with the **Stream Header Import** operator: `><`.

### Why `><`?
The `><` operator visually represents data streams flowing into your module header. It ensures consistent import semantics across the entire compiler, standard library, and application modules.

### Syntax & Usage
```salivo
module myapp.main;

// Standard library core imports
><salivo.std.core::{out, outln, assert, assertEq, Option, some, none, Result, ok, err, Drop};

// Collections imports
><salivo.std.collections::{Vec, vecNew, push, insert, remove, get, veclen, mapNew, put, lookup};

// Math built-ins
><salivo.std.math::{min, max, abs};

// Filesystem module imports
><salivo.std.fs::{open, close, pathJoin, pathExists};

// Crypto module imports
><salivo.std.crypto::{sha256Hex, secSeed};
```

---

## 2. Type System Integrity & Data Types

Salivo features a strict static type system governed by **Type System Laws**.

### The Zero-Underscore Type Law
- Structs, Enums, Traits, and Type Annotations must **never** contain underscores (`_`). They strictly use clean `PascalCase` (e.g. `Point`, `Option`, `Result`, `FileMetadata`, `AuthToken`).
- `_` is **never** a datatype (no `let x: _ = 10;`).
- Function names, variable bindings, and struct fields **can** freely use `camelCase` or `snake_case` (e.g. `pathJoin` or `path_join`, `user_id` or `userId`).

### Complete Data Types Reference Table

| Type Category | Type Name | Description | Example |
| :--- | :--- | :--- | :--- |
| **Primitives** | `int` | 64-bit signed integer | `let x: int = 42;` |
| | `float` | 64-bit IEEE-754 floating point | `let pi: float = 3.14159;` |
| | `bool` | Boolean truth value (`true` / `false`) | `let active: bool = true;` |
| | `string` | Owned heap-allocated UTF-8 string | `let s: string = "hello";` |
| | `StrSlice` | Borrowed immutable string slice | `let slice: StrSlice = "world";` |
| | `ByteSlice` | Raw byte slice | `let bytes: ByteSlice = ...;` |
| | `byte` | 8-bit unsigned byte | `let b: byte = 0xFF;` |
| | `char` | Single Unicode character | `let c: char = 'A';` |
| **Monads** | `Option<T>` | Optional value: `some(val)` or `none()` | `let opt: Option<int> = some(10);` |
| | `Result<T, E>` | Fallible result: `ok(val)` or `err(error)` | `let res: Result<int, string> = ok(1);` |
| **Collections** | `Vec<T>` | Dynamic resizable array | `let mut v = vecNew<int>();` |
| | `Map<K, V>` | Hash map dictionary | `let mut m = mapNew<string, int>();` |
| | `Heap<T>` | Binary min/max priority heap | `let mut h = heapNew<int>();` |
| | `BTree<K, V>` | Sorted B-Tree map | `let mut t = btreeNew<string, int>();` |
| | `Deq<T>` | Double-ended queue | `let mut q = deqNew<int>();` |
| | `BtSet<T>` | B-Tree set | `let mut s = btSetNew<int>();` |
| **System I/O** | `File` | File handle wrapper | `let f: File = open(path, "r")?;` |
| | `Directory` | Directory iterator handle | `let d: Directory = dirList(path)?;` |
| | `FileMetadata` | File metadata struct | `let meta: FileMetadata = meta(path)?;` |
| | `IoError` | Standard I/O error enum | `enum IoError { NotFound, PermissionDenied, ... }` |

---

## 3. User Input & Conversions

Salivo provides `input()` for reading keyboard lines from the terminal, and built-in type conversions in `salivo.std.prelude` (automatically loaded):

- `strint(string s) -> int`: converts a string into an integer.
- `strfloat(string s) -> float`: converts a string into a float.
- `floatstr(float f) -> string`: converts a float into a string.
- `str(int v) -> string`: converts an integer into a string.

```salivo
><salivo.std.core::{out, outln};

pub func main() -> int {
    out("Enter your name: ");
    let name = input();

    out("Enter your age: ");
    let age = strint(input());

    out("Enter your hourly rate: ");
    let rate = strfloat(input());

    outln($"Welcome {name:trim:upper}, Age: {age:str}, Rate: ${floatstr(rate)}");
    return 0;
}
```

---

## 4. String Interpolation & Postfix Modifiers

Template string interpolation uses `$"..."`:
- `{val:str}`: formats integer as string.
- `{val:trim}`: trims leading and trailing whitespace.
- `{val:upper}`: converts string to uppercase.
- `{val:lower}`: converts string to lowercase.
- `{val:len}`: gets string length as integer.

```salivo
let clean = $"User: {name:trim:upper}, ID: {id:str}";
```

---

## 5. Dynamic Arrays (`Vec<T>`)

Dynamic arrays in Salivo are provided by `salivo.std.collections::{Vec, vecNew, push, insert, remove, get, veclen}`:

```salivo
><salivo.std.core::{outln};
><salivo.std.collections::{Vec, vecNew, push, insert, remove, get, veclen};

pub func main() -> int {
    let mut numbers = vecNew<int>();

    // 1. Push elements (adds to end)
    numbers = push<int>(numbers, 10);
    numbers = push<int>(numbers, 20);
    numbers = push<int>(numbers, 40);

    // 2. Insert element at index (e.g. index 2)
    numbers = insert<int>(numbers, 2, 30);

    // 3. Remove element at index (e.g. index 0)
    numbers = remove<int>(numbers, 0);

    // 4. Iterate and read elements
    let count = veclen<int>(numbers);
    for (let mut i = 0; i < count; i = i + 1) {
        let val = get<int>(numbers, i);
        outln($"Index {i:str}: {val:str}");
    }
    return 0;
}
```

---

## 6. Control Flow: Conditionals, Loops, & Pattern Matching

### Conditionals (`if`, `else if`, `else`)
Note: `if` in Salivo is a statement, not an expression.
```salivo
let mut status = "unknown";
if (score >= 90) {
    status = "Grade A";
} else if (score >= 80) {
    status = "Grade B";
} else {
    status = "Grade C";
}
```

### 3-Clause C-Style `for` Loop
```salivo
for (let mut i = 0; i < 5; i = i + 1) {
    if (i == 2) { continue; }
    outln($"Iteration: {i:str}");
}
```

### `while` Loop
```salivo
let mut count = 3;
while (count > 0) {
    outln($"Countdown: {count:str}");
    count = count - 1;
}
```

### `match` Pattern Matching (Exhaustive)
```salivo
let role_name = match role {
    Role::Admin => "Administrator",
    Role::Member => "Standard Member",
    _ => "Guest User",
};
```

---

## 7. Functions, Structs, Enums, Traits & Drop

```salivo
pub enum Status { Active, Pending, Inactive }

pub struct User {
    int id;
    string name;
    Status status;
}

// Traits are declared with 'trait TraitName'
trait Describable {
    func describe(self) -> string;
}

impl Describable for User {
    func describe(self) -> string {
        return $"[User #{self.id:str}] {self.name}";
    }
}

// Deterministic RAII cleanup
impl Drop for User {
    func drop(self) {
        outln($"Cleaning up resources for User {self.name}");
    }
}
```

---

## 8. Compiler & Tooling Commands

- **Compiler Binary:** `C:\Users\sahil\.salivo\bin\sf.exe`
- **Package Manager:** `C:\Users\sahil\.salivo\bin\spm.exe`
- **Run File:** `sf run myfile.sal`
- **Check Syntax & Types:** `sf check myfile.sal`
- **Build Native Binary:** `sf build myfile.sal`
- **Initialize Project:** `spm init project_name`
- **Format Source:** `salivofmt myfile.sal`
- **Lint Source:** `salivolint myfile.sal`
- **One-Click Run in VS Code:** Click **▶ Run** button or press <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>N</kbd>.
