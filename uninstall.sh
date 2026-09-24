#!/bin/sh
# Salivo SDK uninstaller for Linux and macOS: removes ~/.salivo, the PATH line install.sh added and the
# VS Code extension. clang and other system packages are left installed.
set -u
for profile in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
    if [ -f "$profile" ]; then
        grep -v -e '^# Salivo$' -e '.salivo/bin' "$profile" > "$profile.salivo-tmp" && mv "$profile.salivo-tmp" "$profile"
    fi
done
CODE=$(command -v code || true)
if [ -n "$CODE" ]; then "$CODE" --uninstall-extension salivo.salivo >/dev/null 2>&1 || true; fi
rm -rf "$HOME/.salivo"
echo 'Salivo has been removed.'
