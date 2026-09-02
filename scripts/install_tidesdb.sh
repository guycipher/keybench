#!/bin/bash

# TidesDB Installation Script
# This script installs TidesDB from source
# Usage: ./install_tidesdb.sh [--with-mimalloc] [--with-tcmalloc] [--with-sanitizer]
#                             [--prefix <dir>] [--ref <branch|tag|commit>]
#
# TidesDB stamps every build as the same soname, so installing into /usr/local
# overwrites the previous one and it cannot be recovered or re-measured. --prefix
# gives each build its own directory so they coexist and can be benchmarked
# against each other, and --ref pins which revision gets built:
#
#   ./install_tidesdb.sh --prefix /opt/engines/tidesdb-2026-09-02
#   ./install_tidesdb.sh --ref v10.0.0 --prefix /opt/engines/tidesdb-10.0.0
#
# then build keybench against whichever one you want (the script prints the
# command when it finishes).

set -e  # Exit on error

# Parse command line arguments
USE_MIMALLOC=false
USE_TCMALLOC=false
USE_JEMALLOC=false
USE_SANITIZER=false
PREFIX=""
REF=""

while [[ $# -gt 0 ]]; do
    arg="$1"
    case $arg in
        --with-mimalloc)
            USE_MIMALLOC=true
            shift
            ;;
        --with-tcmalloc)
            USE_TCMALLOC=true
            shift
            ;;
        --with-jemalloc)
            USE_JEMALLOC=true
            shift
            ;;
        --with-sanitizer)
            USE_SANITIZER=true
            shift
            ;;
        --prefix)
            PREFIX="$2"
            shift 2
            ;;
        --ref)
            REF="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: ./install_tidesdb.sh [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --with-mimalloc    Build with mimalloc memory allocator"
            echo "  --with-tcmalloc    Build with tcmalloc memory allocator (Google perftools)"
            echo "  --with-jemalloc    Build with jemalloc memory allocator"
            echo "  --with-sanitizer   Build with AddressSanitizer and UBSan"
            echo "  --prefix DIR       Install here instead of /usr/local, so builds coexist"
            echo "  --ref REF          Build this branch, tag, or commit (default: latest tag)"
            echo "  --help, -h         Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# These scripts live in keybench/scripts, so the keybench root is one level up.
# That is what the build hints below are printed against.
KB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Each of these overrides the malloc family process-wide, so linking two is
# undefined at best. Pick one.
ALLOC_COUNT=0
[ "$USE_MIMALLOC" = true ] && ALLOC_COUNT=$((ALLOC_COUNT+1))
[ "$USE_TCMALLOC" = true ] && ALLOC_COUNT=$((ALLOC_COUNT+1))
[ "$USE_JEMALLOC" = true ] && ALLOC_COUNT=$((ALLOC_COUNT+1))
if [ "$ALLOC_COUNT" -gt 1 ]; then
    echo "error: choose at most one of --with-mimalloc, --with-tcmalloc, --with-jemalloc"
    exit 1
fi

INSTALL_PREFIX="${PREFIX:-/usr/local}"
SYSTEM_INSTALL=false
case "$INSTALL_PREFIX" in
    /usr/local|/usr|/usr/local/) SYSTEM_INSTALL=true ;;
esac

echo "========================================="
echo "TidesDB Installation Script"
echo "(prefix: $INSTALL_PREFIX)"
if [ "$USE_MIMALLOC" = true ]; then
    echo "(with mimalloc support)"
fi
if [ "$USE_TCMALLOC" = true ]; then
    echo "(with tcmalloc support)"
fi
if [ "$USE_JEMALLOC" = true ]; then
    echo "(with jemalloc support)"
fi
if [ "$USE_SANITIZER" = true ]; then
    echo "(with AddressSanitizer/UBSan)"
fi
echo "========================================="

# Root is needed for a system prefix and for apt-get, but not for a prefix you own.
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
    libsnappy-dev
    zlib1g-dev
    liblz4-dev
    libzstd-dev
)

# Add mimalloc if requested
if [ "$USE_MIMALLOC" = true ]; then
    DEPENDENCIES+=(libmimalloc-dev)
    echo "Including mimalloc in dependencies..."
fi

# Add tcmalloc if requested
if [ "$USE_TCMALLOC" = true ]; then
    DEPENDENCIES+=(libgoogle-perftools-dev)
    echo "Including tcmalloc (google-perftools) in dependencies..."
fi

# Add jemalloc if requested. Its cmake path goes through pkg_check_modules, as
# tcmalloc's does, so pkg-config has to be present for either.
if [ "$USE_JEMALLOC" = true ]; then
    DEPENDENCIES+=(libjemalloc-dev)
    echo "Including jemalloc in dependencies..."
fi
if [ "$USE_JEMALLOC" = true ] || [ "$USE_TCMALLOC" = true ]; then
    DEPENDENCIES+=(pkg-config)
fi

$SUDO apt-get install -y "${DEPENDENCIES[@]}"

# Clone TidesDB repository
echo "Cloning TidesDB repository..."
cd /tmp
if [ -d "tidesdb" ]; then
    echo "Removing existing tidesdb directory..."
    rm -rf tidesdb
fi

git clone https://github.com/tidesdb/tidesdb.git
cd tidesdb

# Check out the requested revision, or the latest release tag
if [ -n "$REF" ]; then
    echo "Checking out: $REF"
    git checkout "$REF"
    LATEST_TAG="$REF"
else
    echo "Fetching latest release..."
    LATEST_TAG=$(git describe --tags `git rev-list --tags --max-count=1`)
    echo "Latest TidesDB version: $LATEST_TAG"
    git checkout $LATEST_TAG
fi
# The soname is the same for every build, so record what this one actually is.
BUILT_FROM="$(git rev-parse --short HEAD)"

# Build TidesDB
echo "Building TidesDB using $CPU_CORES parallel jobs..."
echo "(this may take a while)"
mkdir -p build
cd build

# Configure CMake options
CMAKE_OPTIONS=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
    -DTIDESDB_BUILD_TESTS=OFF
)

# Add mimalloc option if requested
if [ "$USE_MIMALLOC" = true ]; then
    CMAKE_OPTIONS+=(-DTIDESDB_WITH_MIMALLOC=ON)
    echo "Enabling mimalloc support..."
fi

# Add tcmalloc option if requested
if [ "$USE_TCMALLOC" = true ]; then
    CMAKE_OPTIONS+=(-DTIDESDB_WITH_TCMALLOC=ON)
    echo "Enabling tcmalloc support..."
fi

# Add jemalloc option if requested
if [ "$USE_JEMALLOC" = true ]; then
    CMAKE_OPTIONS+=(-DTIDESDB_WITH_JEMALLOC=ON)
    echo "Enabling jemalloc support..."
fi

# Add sanitizer option if requested
if [ "$USE_SANITIZER" = true ]; then
    CMAKE_OPTIONS+=(-DTIDESDB_WITH_SANITIZER=ON)
    CMAKE_OPTIONS+=(-DCMAKE_BUILD_TYPE=RelWithDebInfo)
    echo "Enabling AddressSanitizer and UBSan..."
fi

# Run CMake with configured options
cmake .. "${CMAKE_OPTIONS[@]}"

# Use all available cores for compilation
make -j${CPU_CORES}

# Install TidesDB
echo "Installing TidesDB into $INSTALL_PREFIX..."
$INSTALL_SUDO make install

# Record the revision beside the library, since the soname cannot distinguish builds.
echo "$LATEST_TAG $BUILT_FROM $(date -Is)" | $INSTALL_SUDO tee "$INSTALL_PREFIX/lib/tidesdb.build-info" >/dev/null 2>&1 || true

if [ "$SYSTEM_INSTALL" = true ]; then
    echo "Updating library cache..."
    $SUDO ldconfig
fi

# Verify installation
echo ""
echo "========================================="
echo "Installation Complete!"
echo "========================================="
echo "TidesDB version: $LATEST_TAG"
if [ "$USE_MIMALLOC" = true ]; then
    echo "Built with mimalloc: YES"
else
    echo "Built with mimalloc: NO"
fi
if [ "$USE_TCMALLOC" = true ]; then
    echo "Built with tcmalloc: YES"
else
    echo "Built with tcmalloc: NO"
fi
if [ "$USE_JEMALLOC" = true ]; then
    echo "Built with jemalloc: YES"
else
    echo "Built with jemalloc: NO"
fi
if [ "$USE_SANITIZER" = true ]; then
    echo "Built with sanitizers: YES (ASan + UBSan)"
else
    echo "Built with sanitizers: NO"
fi
echo "Compiled using: $CPU_CORES parallel jobs"
echo ""
echo "Library location: $INSTALL_PREFIX/lib"
echo "Header location: $INSTALL_PREFIX/include/tidesdb"
echo "Built from: $LATEST_TAG ($BUILT_FROM)"
echo ""
echo "Build keybench against this install with:"
echo "  make -C $KB_ROOT TIDESDB=1 \\"
echo "    TIDESDB_CFLAGS=-I$INSTALL_PREFIX/include \\"
echo "    TIDESDB_LIBS=\"-L$INSTALL_PREFIX/lib -Wl,-rpath,$INSTALL_PREFIX/lib -ltidesdb\""
echo "or, equivalently:"
echo "  make -C $KB_ROOT TIDESDB=1 TIDESDB_PREFIX=$INSTALL_PREFIX"
echo "then confirm what it actually loads with:  make -C $KB_ROOT verify"
echo ""
echo "To verify the installation, you can check:"
echo "  ls -l $INSTALL_PREFIX/lib/libtidesdb.so*"
if [ "$USE_SANITIZER" = true ]; then
    echo "  nm $INSTALL_PREFIX/lib/libtidesdb.so | grep asan"
fi
if [ "$USE_MIMALLOC" = true ]; then
    echo "  ldd $INSTALL_PREFIX/lib/libtidesdb.so | grep mimalloc"
fi
if [ "$USE_TCMALLOC" = true ]; then
    echo "  ldd $INSTALL_PREFIX/lib/libtidesdb.so | grep tcmalloc"
fi
if [ "$USE_JEMALLOC" = true ]; then
    echo "  ldd $INSTALL_PREFIX/lib/libtidesdb.so | grep jemalloc"
fi
echo ""

# Clean up
cd /tmp
echo "Cleaning up temporary files..."
rm -rf tidesdb

echo "Installation finished successfully!"
echo ""
echo "Usage tips:"
echo "  $0 --prefix ~/engines/tidesdb-$(date +%Y-%m-%d)"
echo "                                    every build carries the same soname, so a"
echo "                                    prefix per build is what keeps the previous"
echo "                                    one runnable and re-measurable"
echo "  $0 --ref v10.0.0 --prefix ~/engines/tidesdb-10.0.0"
echo "                                    pin a tag, branch, or commit"
echo "  make -C $KB_ROOT TIDESDB=1 TIDESDB_PREFIX=<dir> && make -C $KB_ROOT verify"
