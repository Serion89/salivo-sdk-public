# Salivo SDK

Everything needed to write, build and run Salivo programs: the `sf` compiler driver with the
self-hosted Stage 3 compiler, the standard library, the package manager (`spm`), the formatter,
linter, doc generator and language server, and the VS Code extension.

## Install on Linux or macOS

1. Extract the archive: `tar xzf salivo-sdk-<version>-<platform>.tar.gz`
2. Run `sh salivo-sdk-<version>-<platform>/install.sh`
3. Open a **new** terminal.

Setup installs into `~/.salivo` (no sudo needed), adds it to PATH in your shell profile, installs
the VS Code extension if the `code` command is available, and finishes by compiling and running a
small program. Salivo needs clang to build programs:

- **macOS**: `xcode-select --install`
- **Debian/Ubuntu**: `sudo apt install clang lld build-essential`
- **Fedora**: `sudo dnf install clang lld` / **Arch**: `sudo pacman -S clang lld`

Uninstall with `sh uninstall.sh`.

## Install on Windows

1. Extract this zip to any folder.
2. Double-click **Salivo Setup.exe**. Windows may show "Windows protected your PC" because the
   installer is not code-signed; click **More info > Run anyway**.
3. When setup finishes, open a **new** terminal or restart VS Code.

Setup installs into `%USERPROFILE%\.salivo` (no admin rights needed), adds `sf` to your PATH,
installs the VS Code extension and gives `.sal` files the Salivo icon. At the end it compiles and
runs a small program to confirm everything works.

### Prerequisites

Salivo produces native executables with clang and the Microsoft C++ libraries. Setup checks for
both and offers to install whichever is missing (with `winget`):

- **LLVM** (clang): `winget install -e --id LLVM.LLVM`
- **Visual Studio 2022 C++ build tools**:
  `winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`

## First program

Save as `hello.sal`:

```salivo
module hello;

><salivo.std.core::{outln};

func main() -> int {
    let name = "Salivo";
    outln($"Hello from {name}!");
    return 0;
}
```

Then run `sf run hello.sal`, or open the file in VS Code and press **Ctrl+F5**.

| Command | What it does |
| --- | --- |
| `sf run file.sal` | Build and run |
| `sf build file.sal` | Build `build\file.exe` |
| `sf check file.sal` | Type-check only |
| `spm new app` | Create a package |

In VS Code: **Ctrl+F5** runs the file, **Ctrl+Shift+B** builds it, **Ctrl+Shift+K** checks it and
**Shift+Alt+F** formats it.

## Troubleshooting

- **`.sal` files lost the Salivo icon** (another program registered itself for `.sal`): run
  `powershell -ExecutionPolicy Bypass -File install.ps1 -RepairFileIcons` from this folder. It restores
  the official icon and the VS Code "open" action, and leaves the other program's own handler in place.
- **"sf is not recognized"**: open a new terminal after installing (PATH changes apply to new windows).
- **Builds fail with a linker error**: install LLVM and the Visual Studio C++ build tools (see above).

## Uninstall

Run `powershell -ExecutionPolicy Bypass -File uninstall.ps1` from this folder.
