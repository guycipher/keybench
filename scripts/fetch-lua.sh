#!/bin/sh
# fetch-lua.sh - download and unpack the vendored Lua source into vendor/.
#
# Default builds (`make`) compile Lua from vendor/*.c into a static liblua.a, so
# the repo stays self-contained and reproducible. This script populates vendor/.
# (Alternatively, skip vendoring entirely and build against a system Lua with
#  `make LUA_SYS=1`.)
#
# Usage:  scripts/fetch-lua.sh [VERSION]
#         scripts/fetch-lua.sh 5.4.7
set -eu

VERSION="${1:-5.4.7}"
URL="https://www.lua.org/ftp/lua-${VERSION}.tar.gz"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR="$ROOT/vendor"

echo "fetching Lua $VERSION -> vendor/"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

curl -fsSL "$URL" -o "$tmp/lua.tar.gz"
tar -xzf "$tmp/lua.tar.gz" -C "$tmp"

# Copy just the library sources/headers (the src/ dir of the tarball).
# liblua.a is assembled by the Makefile, which already excludes lua.c/luac.c.
cp "$tmp/lua-${VERSION}/src/"*.c "$tmp/lua-${VERSION}/src/"*.h "$VENDOR/"

echo "done. vendored files:"
ls "$VENDOR"/*.c "$VENDOR"/*.h | wc -l | sed 's/^/  /'
echo "now run: make"
