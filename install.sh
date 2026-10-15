#!/bin/sh
# Salivo SDK installer for Linux and macOS.
#
# Installs the Salivo toolchain for the current user into ~/.salivo, adds it to PATH in the shell
# profile, installs the VS Code extension (syntax highlighting, snippets, run/build/check commands) and
# checks that a C toolchain is present: Salivo links programs with clang (Linux: clang plus the system
# C library headers; macOS: the Xcode command line tools).
#
#   SALIVO_TARGET_HOME=<dir>  install below <dir> instead of $HOME (testing)
#   SALIVO_NO_SYSTEM_CHANGES=1  skip shell profile and VS Code changes (testing)
set -eu

SRC=$(cd "$(dirname "$0")" && pwd)
TARGET_HOME=${SALIVO_TARGET_HOME:-$HOME}
ROOT="$TARGET_HOME/.salivo"
BIN="$ROOT/bin"
NO_SYSTEM=${SALIVO_NO_SYSTEM_CHANGES:-0}
OS=$(uname -s)

step() { printf '\033[36m[%s/7] %s\033[0m\n' "$1" "$2"; }
ok() { printf '      \033[32m%s\033[0m\n' "$1"; }
warn() { printf '      \033[33m%s\033[0m\n' "$1"; }

printf '\n  ============================================\n'
printf '        Salivo SDK - %s Setup\n' "$OS"
printf '  ============================================\n'
printf '  Installing into %s\n\n' "$ROOT"

if [ ! -x "$SRC/bin/sf" ]; then
    echo "The SDK files were not found next to this installer. Extract the whole archive first." >&2
    exit 1
fi

# 1. Prerequisites ----------------------------------------------------------------------------
# Salivo emits LLVM 16 IR, which clang 15 or newer reads. Distributions whose default clang is older
# (Debian 12, Ubuntu 22.04) ship newer ones as clang-15, clang-16, ...; the newest one found is used.
step 1 'Checking prerequisites (clang; 15 or newer recommended)'
clang_major() { "$1" --version 2>/dev/null | sed -n 's/.*clang version \([0-9][0-9]*\).*/\1/p' | head -n 1; }
CLANG=''
for candidate in clang-20 clang-19 clang-18 clang-17 clang-16 clang-15 clang; do
    path=$(command -v "$candidate" || true)
    if [ -n "$path" ]; then
        major=$(clang_major "$path")
        if [ -n "$major" ] && [ "$major" -ge 15 ]; then
            CLANG=$path
            break
        fi
    fi
done
if [ -n "$CLANG" ]; then
    ok "clang: $CLANG (version $(clang_major "$CLANG"))"
else
    OLD=$(command -v clang || true)
    if [ -n "$OLD" ]; then warn "clang $(clang_major "$OLD") is older than the tested clang 15+; it usually works, but a newer one is recommended:"; else warn 'clang was not found. Salivo needs clang (15 or newer recommended):'; fi
    if [ "$OS" = "Darwin" ]; then
        warn '  xcode-select --install'
    else
        warn '  Debian 12 / Ubuntu 22.04: sudo apt install clang-16'
        warn '  Ubuntu 24.04 and newer:   sudo apt install clang'
        warn '  Fedora:                   sudo dnf install clang'
        warn '  Arch:                     sudo pacman -S clang'
    fi
fi

# 2. Toolchain binaries -----------------------------------------------------------------------
step 2 'Installing the compiler and tools (sf, Stage 3 compiler, spm, formatter, linter, docs, LSP)'
mkdir -p "$BIN"
cp -f "$SRC"/bin/* "$BIN"/
chmod +x "$BIN"/*
# A versioned clang (clang-16, ...) becomes plain `clang` for Salivo; ~/.salivo/bin comes first on PATH
rm -f "$BIN/clang"
case "$CLANG" in
    */clang) ;;
    ?*) ln -s "$CLANG" "$BIN/clang" ;;
esac
ok "$(ls "$BIN" | wc -l | tr -d ' ') files in $BIN"

# 3. Standard library -------------------------------------------------------------------------
step 3 'Installing the standard library'
for dir in "$ROOT/lib/salivo/std" "$ROOT/std"; do
    rm -rf "$dir"
    mkdir -p "$dir"
    cp -R "$SRC"/std/. "$dir"/
done
ok "$(ls "$ROOT/std"/*.sal | wc -l | tr -d ' ') modules"
# Runtime objects for sf's Rust pipeline (packages and fallback builds)
rm -rf "$ROOT/lib/salivo/rt"
cp -R "$SRC/lib/salivo/rt" "$ROOT/lib/salivo/rt"

# 4. Stage 3 runtime --------------------------------------------------------------------------
step 4 'Installing the Stage 3 compiler runtime'
rm -rf "$ROOT/stage3"
cp -R "$SRC/stage3" "$ROOT/stage3"
ok "runtime sources and prebuilt objects in $ROOT/stage3"

# 5. PATH -------------------------------------------------------------------------------------
step 5 'Adding Salivo to your PATH'
if [ "$NO_SYSTEM" = "1" ]; then
    ok 'skipped (SALIVO_NO_SYSTEM_CHANGES=1)'
else
    LINE='export PATH="$HOME/.salivo/bin:$PATH"'
    for profile in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
        if [ -f "$profile" ] || [ "$profile" = "$HOME/.profile" ]; then
            if ! grep -qs '.salivo/bin' "$profile"; then
                printf '\n# Salivo\n%s\n' "$LINE" >> "$profile"
                ok "added to $profile"
            fi
        fi
    done
fi
PATH="$BIN:$PATH"
export PATH

# 6. VS Code extension ------------------------------------------------------------------------
step 6 'Installing the Salivo VS Code extension (syntax highlighting, snippets, run and build commands)'
VSIX=$(ls "$SRC"/vscode/salivo-*.vsix 2>/dev/null | head -n 1)
if [ "$NO_SYSTEM" = "1" ]; then
    ok 'skipped (SALIVO_NO_SYSTEM_CHANGES=1)'
else
    CODE=$(command -v code || true)
    MAC_CODE='/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code'
    if [ -z "$CODE" ] && [ -x "$MAC_CODE" ]; then CODE=$MAC_CODE; fi
    if [ -n "$CODE" ] && [ -n "$VSIX" ]; then
        if "$CODE" --install-extension "$VSIX" --force >/dev/null 2>&1; then
            ok "installed $(basename "$VSIX")"
        else
            warn "VS Code could not install $(basename "$VSIX"); use Extensions > ... > Install from VSIX."
        fi
    else
        warn "VS Code was not found. After installing it, use Extensions > ... > Install from VSIX with vscode/$(basename "$VSIX")."
    fi
fi

# 7. Self-test --------------------------------------------------------------------------------
step 7 'Testing the installation: compiling and running a program'
ok "$("$BIN/sf" version)"
TEST_DIR=$(mktemp -d)
cat > "$TEST_DIR/hello.sal" <<'SAL'
module hello;

><salivo.std.core::{outln};

func main() -> int {
    let name = "Salivo";
    outln($"Hello from {name}!");
    return 0;
}
SAL
OUT=$(cd "$TEST_DIR" && "$BIN/sf" run --verbose hello.sal 2>&1 || true)
rm -rf "$TEST_DIR"
STATUS=0
case "$OUT" in
    *"Hello from Salivo!"*)
        case "$OUT" in
            *"with Stage 3"*) ok 'compiled with the Stage 3 compiler and ran: Hello from Salivo!' ;;
            *) warn 'the test program ran, but not through the Stage 3 compiler:'; echo "$OUT" ;;
        esac ;;
    *)
        warn 'the test program did not build or run. Output:'
        echo "$OUT"
        warn 'This almost always means clang is missing (see step 1).'
        STATUS=2 ;;
esac

printf '\n  ============================================\n'
if [ "$STATUS" = "0" ]; then
    printf '   Salivo is installed.\n'
else
    printf '   Salivo is installed, but building programs needs clang (step 1).\n'
fi
printf '   Open a NEW terminal and try:  sf run hello.sal\n'
printf '   In VS Code: open a .sal file and press Ctrl+F5 to run it.\n'
printf '  ============================================\n'
exit "$STATUS"
