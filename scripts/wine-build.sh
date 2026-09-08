#!/usr/bin/env bash
#
# Native Wine build for Generals/Zero Hour on Linux (no Docker, no root).
#
# Runs the same Windows toolchain the Docker image uses (CMake, Ninja, MSVC 6,
# MinGit) directly under the host's Wine, in a dedicated WINEPREFIX.
#
# Usage:
#   ./scripts/wine-build.sh --setup             # download the toolchain (once)
#   ./scripts/wine-build.sh                     # build Zero Hour
#   ./scripts/wine-build.sh --game generals     # build Generals
#   ./scripts/wine-build.sh --target z_worldbuilder
#   ./scripts/wine-build.sh --cmake             # force CMake reconfiguration
#   ./scripts/wine-build.sh --clean
#
# Environment overrides:
#   TOOLS_DIR   where the Windows toolchain lives (default: <repo>/build/wine-tools)
#   WINE        wine binary to use (default: first `wine` on PATH)
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

TOOLS_DIR="${TOOLS_DIR:-$PROJECT_DIR/build/wine-tools}"
BUILD_DIR="$PROJECT_DIR/build/vc6"
WINE="${WINE:-wine}"

CMAKE_VERSION="3.31.6"
GIT_VERSION="2.49.0"
NINJA_VERSION="1.13.1"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1"; }

usage() { sed -n '3,25p' "$0" | sed 's/^# \{0,1\}//'; }

# --- Wine environment -------------------------------------------------------
# A build-only prefix, so an existing game prefix is never touched.
setup_env() {
    export WINEPREFIX="${WINEPREFIX_BUILD:-$TOOLS_DIR/prefix}"
    export WINEARCH=win64
    export WINEDEBUG="${WINEDEBUG:--all}"

    local vs="Z:$TOOLS_DIR/vs6"

    # CL.EXE is a small driver stub. Without its code-generation DLLs
    # (C1/C1XX/C2) on WINEPATH it exits with status 53 and prints nothing.
    export WINEPATH="C:\\windows\\system32;$vs\\VC98\\Bin;$vs\\Common\\MSDev98\\Bin"
    export INCLUDE="$vs\\VC98\\ATL\\INCLUDE;$vs\\VC98\\INCLUDE;$vs\\VC98\\MFC\\INCLUDE"
    export LIB="$vs\\VC98\\Lib;$vs\\VC98\\MFC\\Lib;Z:$BUILD_DIR"
    export CC="$vs\\VC98\\Bin\\CL.EXE"
    export CXX="$vs\\VC98\\Bin\\CL.EXE"

    # Give the toolchain a TEMP that exists inside the prefix's Z: mapping.
    mkdir -p "$TOOLS_DIR/tmp"
    export TMP="Z:$TOOLS_DIR/tmp" TEMP="Z:$TOOLS_DIR/tmp"
}

check_dependencies() {
    local missing=0
    for cmd in wget unzip; do
        command -v "$cmd" &>/dev/null || { print_error "$cmd is required"; missing=1; }
    done
    command -v "$WINE" &>/dev/null || {
        print_error "wine not found (set WINE=/path/to/wine to override)"
        missing=1
    }
    [ "$missing" -eq 0 ] || exit 1

    # Wine must be able to run 32-bit Windows binaries; the toolchain is i386.
    print_info "Using $("$WINE" --version 2>/dev/null || echo "$WINE")"
}

setup_toolchain() {
    mkdir -p "$TOOLS_DIR"
    cd "$TOOLS_DIR"

    if [ ! -f "$TOOLS_DIR/cmake/bin/cmake.exe" ]; then
        print_info "Downloading CMake $CMAKE_VERSION (Windows)..."
        wget -q --show-progress -O cmake.zip \
            "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-windows-x86_64.zip"
        unzip -q cmake.zip && mv "cmake-${CMAKE_VERSION}-windows-x86_64" cmake && rm cmake.zip
    fi

    if [ ! -f "$TOOLS_DIR/ninja.exe" ]; then
        print_info "Downloading Ninja $NINJA_VERSION (Windows)..."
        wget -q -O ninja-win.zip \
            "https://github.com/ninja-build/ninja/releases/download/v${NINJA_VERSION}/ninja-win.zip"
        unzip -q ninja-win.zip && rm ninja-win.zip
    fi

    # MinGit's git.exe is a launcher and needs mingw64/ and usr/ as siblings,
    # so the archive is kept intact rather than hoisting cmd/ out of it.
    if [ ! -f "$TOOLS_DIR/git/mingw64/bin/git.exe" ]; then
        print_info "Downloading MinGit $GIT_VERSION (Windows)..."
        wget -q -O mingit.zip \
            "https://github.com/git-for-windows/git/releases/download/v${GIT_VERSION}.windows.1/MinGit-${GIT_VERSION}-64-bit.zip"
        mkdir -p git && unzip -q mingit.zip -d git && rm mingit.zip
    fi

    if [ ! -f "$TOOLS_DIR/vs6/VC98/Bin/CL.EXE" ]; then
        print_info "Downloading Visual C++ 6.0 portable (~45 MB)..."
        wget -q --show-progress -O msvc600.zip \
            "https://github.com/itsmattkc/MSVC600/archive/refs/heads/master.zip"
        unzip -q msvc600.zip && mv MSVC600-master vs6 && rm msvc600.zip
    fi

    setup_env
    print_info "Initializing Wine build prefix at $WINEPREFIX"
    "$WINE"boot -i &>/dev/null || true

    # Fail early and clearly if the compiler cannot start.
    if ! "$WINE" "$TOOLS_DIR/vs6/VC98/Bin/CL.EXE" 2>&1 | grep -q "Optimizing Compiler"; then
        print_error "MSVC 6 failed to run under Wine."
        print_error "Check that Wine can execute 32-bit binaries (wine32/wine-i386)."
        exit 1
    fi
    print_success "Toolchain ready"
}

run_cmake() {
    local vs="Z:$TOOLS_DIR/vs6"
    print_info "Configuring (CMake preset vc6)..."
    cd "$PROJECT_DIR"
    "$WINE" "$TOOLS_DIR/cmake/bin/cmake.exe" \
        --preset vc6 \
        -DCMAKE_SYSTEM="Windows" \
        -DCMAKE_SYSTEM_NAME="Windows" \
        -DCMAKE_SIZEOF_VOID_P=4 \
        -DCMAKE_MAKE_PROGRAM="Z:$TOOLS_DIR/ninja.exe" \
        -DCMAKE_C_COMPILER="$vs/VC98/Bin/CL.EXE" \
        -DCMAKE_CXX_COMPILER="$vs/VC98/Bin/CL.EXE" \
        -DGIT_EXECUTABLE="Z:$TOOLS_DIR/git/mingw64/bin/git.exe" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
        -DCMAKE_DISABLE_PRECOMPILE_HEADERS=1 \
        -DCMAKE_C_COMPILER_WORKS=1 \
        -DCMAKE_CXX_COMPILER_WORKS=1 \
        -B "$BUILD_DIR"
}

# Ninja decides what to rebuild from file mtimes. A git operation that swaps the
# source tree (stash, pop, checkout, rebase) can restore files with OLDER
# timestamps than the objects built from them, so Ninja skips work it should
# redo and links a mixture of old and new objects. The result compiles, links
# and then misbehaves at runtime. Detect a changed source revision and force a
# clean rebuild rather than trusting mtimes.
check_source_revision() {
    local stamp="$BUILD_DIR/.source-revision"
    local current
    current="$(cd "$PROJECT_DIR" && git rev-parse HEAD 2>/dev/null || echo unknown)"
    # A dirty tree is normal while developing; only the committed revision is
    # tracked here, which is what a stash or checkout actually changes.
    [ -f "$BUILD_DIR/build.ninja" ] || { echo "$current" >"$stamp" 2>/dev/null; return; }

    if [ -f "$stamp" ] && [ "$(cat "$stamp")" != "$current" ]; then
        print_warning "Source revision changed since the last build."
        print_warning "Rebuilding from scratch: incremental builds are not safe across a tree switch."
        rm -rf "$BUILD_DIR"
    fi
    mkdir -p "$BUILD_DIR"
    echo "$current" >"$stamp" 2>/dev/null
}

run_build() {
    local target="$1"
    print_info "Building${target:+ target: $target}..."
    cd "$BUILD_DIR"
    # shellcheck disable=SC2086
    "$WINE" "$TOOLS_DIR/ninja.exe" $target
}

list_outputs() {
    echo ""
    print_info "Build outputs:"
    for game in GeneralsMD Generals; do
        if [ -d "$BUILD_DIR/$game" ]; then
            find "$BUILD_DIR/$game" -maxdepth 1 -name "*.exe" \
                -printf "    %f (%s bytes)\n" 2>/dev/null || true
        fi
    done
}

GAME="zh"; TARGET=""; CLEAN=false; SETUP_ONLY=false; FORCE_CMAKE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)   usage; exit 0 ;;
        -g|--game)   GAME="$2"; shift 2 ;;
        -t|--target) TARGET="$2"; shift 2 ;;
        -c|--clean)  CLEAN=true; shift ;;
        -s|--setup)  SETUP_ONLY=true; shift ;;
        --cmake)     FORCE_CMAKE=true; shift ;;
        -*)          print_error "Unknown option: $1"; usage; exit 1 ;;
        *)           TARGET="$TARGET $1"; shift ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    case "$GAME" in
        zh|zerohour) TARGET="z_generals z_worldbuilder core_particleeditor core_debugwindow z_guiedit z_imagepacker z_mapcachebuilder z_w3dview z_wdump" ;;
        generals)    TARGET="g_generals g_worldbuilder core_particleeditor core_debugwindow g_guiedit g_imagepacker g_mapcachebuilder g_w3dview" ;;
        all)         TARGET="" ;;
        *)           print_error "Unknown game: $GAME (use 'zh', 'generals', or 'all')"; exit 1 ;;
    esac
fi

check_dependencies
setup_toolchain
$SETUP_ONLY && exit 0

if [ "$CLEAN" = true ]; then
    print_info "Cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

setup_env
check_source_revision
if [ "$FORCE_CMAKE" = true ] || [ ! -f "$BUILD_DIR/build.ninja" ]; then
    run_cmake
fi
run_build "$TARGET"
list_outputs
print_success "Build completed"
