#!/bin/sh
# Salivo installer for Linux. Downloads the latest release, checks its SHA-256, and runs its installer.
#   curl -fsSL https://raw.githubusercontent.com/Serion89/salivo-sdk-public/main/get.sh | sh
set -eu

REPO=Serion89/salivo-sdk-public
if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
    echo "This installer supports Linux x86_64 only." >&2
    exit 1
fi

API="https://api.github.com/repos/$REPO/releases/latest"
URLS=$(curl -fsSL "$API" | grep -o '"browser_download_url": *"[^"]*"' | cut -d'"' -f4)
ARCHIVE_URL=$(printf '%s\n' "$URLS" | grep 'salivo-sdk-.*-linux-x64\.tar\.gz$' | head -n 1)
SUMS_URL=$(printf '%s\n' "$URLS" | grep 'SHA256SUMS\.txt$' | head -n 1)
if [ -z "$ARCHIVE_URL" ] || [ -z "$SUMS_URL" ]; then
    echo "The latest release has no Linux archive or SHA256SUMS.txt." >&2
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
NAME=${ARCHIVE_URL##*/}
echo "Downloading $NAME..."
curl -fsSL "$ARCHIVE_URL" -o "$TMP/$NAME"
EXPECTED=$(curl -fsSL "$SUMS_URL" | grep "$NAME" | head -n 1 | cut -d' ' -f1)
if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL=$(sha256sum "$TMP/$NAME" | cut -d' ' -f1)
else
    ACTUAL=$(shasum -a 256 "$TMP/$NAME" | cut -d' ' -f1)
fi
if [ -z "$EXPECTED" ] || [ "$EXPECTED" != "$ACTUAL" ]; then
    echo "Checksum mismatch for $NAME. Expected '$EXPECTED', got '$ACTUAL'." >&2
    exit 1
fi
echo "Checksum OK. Installing..."
tar -xzf "$TMP/$NAME" -C "$TMP"
INSTALLER=$(find "$TMP" -name install.sh | head -n 1)
if [ -r /dev/tty ]; then sh "$INSTALLER" </dev/tty; else sh "$INSTALLER" </dev/null; fi
