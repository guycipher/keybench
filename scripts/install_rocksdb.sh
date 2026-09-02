#!/bin/bash

# RocksDB Installation Script for Ubuntu (GCC 12 Compatible)
# This script installs RocksDB from source
# Usage: ./install_rocksdb.sh [--with-jemalloc] [--branch <branch_name>] [--prefix <dir>]
#
# --prefix installs into a directory of your choosing rather than /usr/local, which
# is what lets several versions live side by side and be benchmarked against each
# other. Without it every build overwrites the last and the previous one is gone.
# A conventional layout is one directory per version:
#
#   ./install_rocksdb.sh --branch v11.8.1 --prefix /opt/engines/rocksdb-11.8.1
#   ./install_rocksdb.sh --branch v10.7.5 --prefix /opt/engines/rocksdb-10.7.5
#
# then build keybench against whichever one you want to measure (the script prints
# the exact command when it finishes).

set -e  # Exit on error

# Parse command line arguments
USE_JEMALLOC=false
BRANCH=""
PREFIX=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --with-jemalloc)
            USE_JEMALLOC=true
            shift
            ;;
        --branch)
            BRANCH="$2"
            shift 2
            ;;
        --prefix)
            PREFIX="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [--with-jemalloc] [--branch <branch_name>] [--prefix <dir>]"
            echo ""
            echo "  --with-jemalloc  build against jemalloc"
            echo "  --branch REF     git branch, tag, or commit (default: latest release tag)"
            echo "  --prefix DIR     install here instead of /usr/local, so versions coexist"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--with-jemalloc] [--branch <branch_name>] [--prefix <dir>]"
            exit 1
            ;;
    esac
done

# /usr/local needs root; a prefix you own does not. Only reach for sudo when the
# install target actually requires it, so a per-version prefix under your home
# needs no privileges at all.
# These scripts live in keybench/scripts, so the keybench root is one level up.
# That is what the build hints below are printed against.
KB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

INSTALL_PREFIX="${PREFIX:-/usr/local}"
SYSTEM_INSTALL=false
case "$INSTALL_PREFIX" in
    /usr/local|/usr|/usr/local/) SYSTEM_INSTALL=true ;;
esac

echo "========================================="
echo "RocksDB Installation Script"
echo "(prefix: $INSTALL_PREFIX)"
if [ -n "$BRANCH" ]; then
    echo "(branch: $BRANCH)"
else
    echo "(latest release tag)"
fi
if [ "$USE_JEMALLOC" = true ]; then
    echo "(with jemalloc support)"
else
    echo "(without jemalloc)"
fi
echo "========================================="

# Root is needed to install into a system prefix and to apt-get the build
# dependencies, but not to install into a prefix you own.
if [ "$EUID" -ne 0 ]; then
    SUDO="sudo"
else
    SUDO=""
fi
INSTALL_SUDO="$SUDO"
if [ "$SYSTEM_INSTALL" = false ]; then
    mkdir -p "$INSTALL_PREFIX" 2>/dev/null || true
    if [ -w "$INSTALL_PREFIX" ]; then
        INSTALL_SUDO=""
        echo "Installing into $INSTALL_PREFIX without sudo (you own it)"
    fi
fi

# Detect number of CPU cores
CPU_CORES=$(nproc)
echo "Detected $CPU_CORES CPU cores - will use all for compilation"

# Update package list
echo "Updating package list..."
$SUDO apt-get update

# Install build dependencies
echo "Installing build dependencies..."
DEPENDENCIES=(
    git
    build-essential
    cmake
    libgflags-dev
    libsnappy-dev
    zlib1g-dev
    libbz2-dev
    liblz4-dev
    libzstd-dev
)

# Add jemalloc if requested
if [ "$USE_JEMALLOC" = true ]; then
    DEPENDENCIES+=(libjemalloc-dev)
    echo "Including jemalloc in dependencies..."
fi

$SUDO apt-get install -y "${DEPENDENCIES[@]}"

# Clone RocksDB repository
echo "Cloning RocksDB repository..."
cd /tmp
if [ -d "rocksdb" ]; then
    echo "Removing existing rocksdb directory..."
    rm -rf rocksdb
fi

git clone https://github.com/facebook/rocksdb.git
cd rocksdb

# Checkout the requested branch or latest release tag
if [ -n "$BRANCH" ]; then
    echo "Checking out branch: $BRANCH"
    git checkout $BRANCH
    ROCKSDB_VERSION="branch-$BRANCH"
else
    echo "Fetching latest release..."
    LATEST_TAG=$(git describe --tags `git rev-list --tags --max-count=1`)
    echo "Latest RocksDB version: $LATEST_TAG"
    git checkout $LATEST_TAG
    ROCKSDB_VERSION="$LATEST_TAG"
fi

# Build RocksDB with GCC 12 compatibility flags
echo "Building RocksDB using $CPU_CORES parallel jobs..."
echo "(this may take a while, but will be faster with parallel compilation)"
mkdir -p build
cd build

# Configure CMake options
CMAKE_OPTIONS=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
    -DWITH_GFLAGS=ON
    -DWITH_SNAPPY=ON
    -DWITH_LZ4=ON
    -DWITH_ZLIB=ON
    -DWITH_BZ2=ON
    -DWITH_ZSTD=ON
    -DPORTABLE=ON
    -DCMAKE_CXX_FLAGS="-Wno-restrict"
)

# Add jemalloc option if requested
if [ "$USE_JEMALLOC" = true ]; then
    CMAKE_OPTIONS+=(-DWITH_JEMALLOC=ON)
    echo "Enabling jemalloc support..."
else
    CMAKE_OPTIONS+=(-DWITH_JEMALLOC=OFF)
fi

# Run CMake with configured options
CXXFLAGS="-Wno-restrict" cmake .. "${CMAKE_OPTIONS[@]}"

# Use all available cores for compilation
make -j${CPU_CORES}

# Install RocksDB
echo "Installing RocksDB into $INSTALL_PREFIX..."
$INSTALL_SUDO make install

# The loader cache only covers system directories. A custom prefix is found
# through the rpath keybench links with, so there is nothing to refresh.
if [ "$SYSTEM_INSTALL" = true ]; then
    echo "Updating library cache..."
    $SUDO ldconfig
fi

# Verify installation
echo ""
echo "========================================="
echo "Installation Complete!"
echo "========================================="
echo "RocksDB version: $ROCKSDB_VERSION"
if [ "$USE_JEMALLOC" = true ]; then
    echo "Built with jemalloc support: YES"
else
    echo "Built with jemalloc support: NO"
fi
echo "Compiled using: $CPU_CORES parallel jobs"
echo ""
echo "Library location: $INSTALL_PREFIX/lib"
echo "Header location: $INSTALL_PREFIX/include/rocksdb"
echo ""
echo "Build keybench against this install with:"
echo "  make -C $KB_ROOT ROCKSDB=1 \\"
echo "    ROCKSDB_CFLAGS=-I$INSTALL_PREFIX/include \\"
echo "    ROCKSDB_LIBS=\"-L$INSTALL_PREFIX/lib -Wl,-rpath,$INSTALL_PREFIX/lib -lrocksdb\" \\"
echo "    ROCKSDB_VERSION_H=$INSTALL_PREFIX/include/rocksdb/version.h"
echo "or, equivalently:"
echo "  make -C $KB_ROOT ROCKSDB=1 ROCKSDB_PREFIX=$INSTALL_PREFIX"
echo "then confirm what it actually loads with:  make -C $KB_ROOT verify"
echo ""
echo "To verify the installation, you can check:"
echo "  ls -l $INSTALL_PREFIX/lib/librocksdb.so*"
if [ "$USE_JEMALLOC" = true ]; then
    echo "  ldd $INSTALL_PREFIX/lib/librocksdb.so | grep jemalloc"
fi
echo ""

# Clean up
cd /tmp
echo "Cleaning up temporary files..."
rm -rf rocksdb

echo "Installation finished successfully!"
echo ""
echo "Usage tips:"
echo "  $0 --with-jemalloc                      build against jemalloc"
echo "  $0 --branch v11.8.1                     build a specific tag or branch"
echo "  $0 --branch v11.8.1 --prefix ~/engines/rocksdb-11.8.1"
echo "                                          keep versions side by side, then"
echo "                                          make -C $KB_ROOT ROCKSDB=1 ROCKSDB_PREFIX=<dir>"