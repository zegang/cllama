#!/bin/bash
#
# Cross-platform build script for CLLaMA.
# Delegates to CMakePresets.json — no duplicated platform args.
#
# Usage:
#   ./scripts/build.sh [options] [command]
#
# Options:
#   --preset <name>     Build preset (default: auto-detect host)
#   --extra-cmake-args  Extra -D flags passed to cmake configure
#
# Commands:
#   build       Configure + build (default)
#   test        Build + run tests
#   install     Build + install
#   reconfigure Delete build dir and rebuild from scratch
#   list        List available presets
#   help        Show this message
#
# Environment:
#   ANDROID_NDK       Required for android preset
#   CLLAMA_TARGET=ios Selects iOS preset on macOS

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Colors ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
print_info()  { echo -e "${GREEN}[INFO]${NC}  $1"; }
print_warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# ── Auto-detect host preset ─────────────────────────────────────────
detect_host_preset() {
    case "$(uname -s)" in
        Linux*)  echo "linux" ;;
        Darwin*)
            [[ "$CLLAMA_TARGET" == "ios" ]] && { echo "ios"; return; }
            echo "macos" ;;
        MINGW*|MSYS*|CYGWIN*) echo "windows" ;;
        *) echo "linux" ;;
    esac
}

# ── Resolve binary dir from preset name ─────────────────────────────
# Keep in sync with CMakePresets.json binaryDir entries.
preset_build_dir() {
    case "$1" in
        default|linux) echo "${PROJECT_DIR}/build" ;;
        windows-static) echo "${PROJECT_DIR}/build_windows_static" ;;
        *)              echo "${PROJECT_DIR}/build_${1}" ;;
    esac
}

# ── Platform requirements ───────────────────────────────────────────
check_platform_reqs() {
    local p="$1"
    print_info "Checking requirements for preset \"${p}\"..."
    command -v cmake &>/dev/null || { print_error "cmake not found"; exit 1; }
    case "$p" in
        linux)
            command -v g++ &>/dev/null || command -v clang++ &>/dev/null \
                || { print_error "No C++ compiler found"; exit 1; } ;;
        macos)
            xcode-select -p &>/dev/null \
                || { print_error "Xcode CLT not installed. Run: xcode-select --install"; exit 1; } ;;
        ios)
            xcodebuild -version &>/dev/null \
                || { print_error "Xcode not found. iOS builds require Xcode."; exit 1; } ;;
        android)
            { [[ -n "$ANDROID_NDK" ]] && [[ -d "$ANDROID_NDK" ]]; } \
                || { print_error "ANDROID_NDK must point to a valid NDK directory"; exit 1; }
            command -v ninja &>/dev/null \
                || { print_error "ninja not found (required for Android)"; exit 1; }
            print_info "  ANDROID_NDK = $ANDROID_NDK" ;;
        windows|windows-static)
            command -v cl.exe &>/dev/null \
                || print_warn "cl.exe not in PATH. Run from a Visual Studio developer prompt." ;;
    esac
}

# ── Configure (delegates to CMakePresets.json) ──────────────────────
configure_cmake() {
    local preset="$1" extra="$2"
    print_info "Configuring: cmake --preset ${preset}..."
    # shellcheck disable=SC2086
    cmake --preset "$preset" $extra
    print_info "Configuration done."
}

# ── Build ───────────────────────────────────────────────────────────
build_project() {
    local dir="$1"
    print_info "Building (dir: ${dir})..."
    cmake --build "$dir" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    print_info "Build completed."
}

# ── Test / Install ──────────────────────────────────────────────────
run_tests()     { cd "$1" && ctest --output-on-failure; print_info "Tests done."; }
install_project() {
    local dir="$1" prefix="${CLLAMA_PREFIX:-/usr/local}"
    read -p "Install prefix (default: ${prefix}): " input
    prefix="${input:-$prefix}"
    DESTDIR="$prefix" cmake --install "$dir" --prefix "$prefix"
    print_info "Installed to ${prefix}."
}

# ── List / Help ─────────────────────────────────────────────────────
list_presets() {
    cmake --list-presets 2>/dev/null \
        || echo "Run from project root to see presets."
}
show_help() {
    cat <<EOF
Usage: $0 [options] [command]

Options:
  --preset <name>     Build preset (default: auto-detect host)
  --extra-cmake-args  Extra -D flags passed to cmake configure

Commands:
  build       Configure + build (default)
  test        Build + run tests
  install     Build + install
  reconfigure Delete build dir and rebuild from scratch
  list        List available presets
  help        Show this message

Environment:
  ANDROID_NDK       Required for android preset
  CLLAMA_TARGET=ios Selects iOS preset on macOS

Examples:
  $0                                          # auto-detect, configure + build
  $0 --preset android test                    # Android build + test
  $0 --preset windows                         # Windows (VS 2022)
  CLLAMA_TARGET=ios $0 --preset ios build     # iOS
EOF
}

# ── Parse args ──────────────────────────────────────────────────────
PRESET=""; COMMAND="build"; EXTRA=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)          PRESET="$2"; shift 2 ;;
        --extra-cmake-args) EXTRA="$2"; shift 2 ;;
        build|test|install|reconfigure|list|help) COMMAND="$1"; shift ;;
        *) print_error "Unknown: $1"; show_help; exit 1 ;;
    esac
done

[[ "$COMMAND" == "help" ]] && { show_help; exit 0; }
[[ "$COMMAND" == "list" ]] && { list_presets; exit 0; }

# Determine preset & build dir
[[ -z "$PRESET" ]] && PRESET="$(detect_host_preset)" && print_info "Auto-detected: ${PRESET}"
BUILD_DIR="$(preset_build_dir "$PRESET")"
check_platform_reqs "$PRESET"

# Reconfigure = wipe + rebuild
if [[ "$COMMAND" == "reconfigure" ]]; then
    print_info "Removing ${BUILD_DIR}..."
    rm -rf "$BUILD_DIR"; COMMAND="build"
fi

configure_cmake "$PRESET" "$EXTRA"
build_project "$BUILD_DIR"
[[ "$COMMAND" == "test" ]]    && run_tests "$BUILD_DIR"
[[ "$COMMAND" == "install" ]] && install_project "$BUILD_DIR"
