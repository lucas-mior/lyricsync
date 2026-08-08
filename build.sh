#!/bin/sh -e

# shellcheck disable=SC2086

dir=$(dirname "$(readlink -f "$0")")
# shellcheck source=/dev/null
. "$dir/cbase/common.sh"

program=$(get_program "$0")
program_path="bin/$program"
library_path="bin/$program.so"
cd "$dir" || exit

script=$(basename "$0")
target=${1:-debug}

printf "\n${script} ${RED}${1:-} ${2:-}$RES\n"

case "$target" in
debug|test)
    CC="${CC:-tcc}"
    ;;
fast_feedback)
    CC="${CC:-clang}"
    ;;
*)
    CC="${CC:-cc}"
    ;;
esac

if ! command -v "$CC" > /dev/null 2>&1; then
    CC=cc
fi

if [ "$#" -gt 0 ]; then
    shift
fi

target_arg=${1:-}
DEFAULT_LDLIBS=${DEFAULT_LDLIBS:-"-lm"}
PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}
DEFAULT_MODEL_DIR=${DEFAULT_MODEL_DIR:-models}

CPPFLAGS="${CPPFLAGS:-}"

CFLAGS="${CFLAGS:-}"
LDFLAGS="${LDFLAGS:-}"

CPPFLAGS="$CPPFLAGS -D_DEFAULT_SOURCE"
CPPFLAGS="$CPPFLAGS -Icbase -I. -Isrc"

OS=$(uname -a)
GNUSOURCE=
if echo "$OS" | grep -q "Linux"; then
    if echo "$OS" | grep -q "GNU"; then
        GNUSOURCE="-D_GNU_SOURCE"
    fi
fi

case "$OS" in
*Linux*)
    CPPFLAGS="$CPPFLAGS -D_XOPEN_SOURCE=700"
    DEFAULT_LDLIBS="$DEFAULT_LDLIBS -ldl"
    ;;
*Darwin*)
    CPPFLAGS="$CPPFLAGS -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE"
    ;;
esac

CFLAGS="$CFLAGS -std=c11"
CFLAGS="$CFLAGS -Wfatal-errors"
CFLAGS="$CFLAGS -Wextra -Wall"
CFLAGS="$CFLAGS -Werror=all -Werror=extra"
CFLAGS="$CFLAGS -Werror"  # Only uncomment occasionally, keep this line
CFLAGS="$CFLAGS -Wno-cast-qual"
CFLAGS="$CFLAGS -Wno-char-subscripts"
CFLAGS="$CFLAGS -Wno-constant-logical-operand"
CFLAGS="$CFLAGS -Wno-format-pedantic"
CFLAGS="$CFLAGS -Wno-padded"
CFLAGS="$CFLAGS -Wno-unused-function"
CFLAGS="$CFLAGS -Wno-unused-macros"

if [ "$CC" = "clang" ]; then
    CFLAGS="$CFLAGS -Weverything"
    CFLAGS="$CFLAGS -Wno-assign-enum"
    CFLAGS="$CFLAGS -Wno-bad-function-cast"
    CFLAGS="$CFLAGS -Wno-c++-keyword"
    CFLAGS="$CFLAGS -Wno-cast-align"
    CFLAGS="$CFLAGS -Wno-covered-switch-default"
    CFLAGS="$CFLAGS -Wno-disabled-macro-expansion"
    CFLAGS="$CFLAGS -Wno-float-equal"
    CFLAGS="$CFLAGS -Wno-format-nonliteral"
    CFLAGS="$CFLAGS -Wno-ignored-attributes"
    CFLAGS="$CFLAGS -Wno-implicit-int-enum-cast"
    CFLAGS="$CFLAGS -Wno-implicit-void-ptr-cast"
    CFLAGS="$CFLAGS -Wno-pre-c11-compat"
    CFLAGS="$CFLAGS -Wno-unsafe-buffer-usage"
    CFLAGS="$CFLAGS -Wno-used-but-marked-unused"
fi

case "$target" in
build|all|run|lib)
    CFLAGS="$CFLAGS $GNUSOURCE -O2 -g"
    ;;
fast_feedback)
    CFLAGS="$CFLAGS $GNUSOURCE"
    ;;
debug)
    CFLAGS="$CFLAGS $GNUSOURCE -g3 -O0 -fsanitize=undefined"
    CPPFLAGS="$CPPFLAGS -DDEBUGGING=1"
    ;;
test)
    CFLAGS="$CFLAGS $GNUSOURCE -g3 -O0 -DDEBUGGING=1"
    CFLAGS="$CFLAGS -Wno-unused-variable"
    if [ "$CC" != "tcc" ]; then
        CFLAGS="$CFLAGS -fsanitize=undefined"
    fi
    ;;
check)
    ;;
*)
    ;;
esac

if command -v xsel >/dev/null 2>&1; then
    xsel="xsel -i -b"
else
    xsel="cat"
fi

pkg_config_flags=
pkg_config_setup_done=0

usage() {
    cat <<'USAGE'
usage: ./build.sh [command] [args]

commands:
    build    build the executable
    lib      build the shared library
    install  install program, library, header, models, man page, completions
    run      build and run the executable with the remaining args
    test     build and run embedded module tests
    debug    build with debug flags and UBSan
    fast_feedback build with the default feedback compiler warnings, then run
    check    build with GCC and Clang static analyzers
    clean    remove generated build outputs
    help     show this message

environment:
    CC                   C compiler, default: cc or tcc for tests
    CFLAGS               extra compiler flags
    DEFAULT_LDLIBS       default libraries, default: -lm
    DEFAULT_MODEL_DIR    compiled model directory, default: models
USAGE
}

pkg_config_add_flags() {
    pkg="$1"
    trace_on
    flags=$(pkg-config --cflags --libs "$pkg")
    trace_off

    pkg_config_flags="$pkg_config_flags $flags"
}

setup_pkg_config_flags() {
    if [ "$pkg_config_setup_done" -ne 0 ]; then
        return
    fi

    if ! command -v pkg-config >/dev/null 2>&1; then
        echo "missing required command: pkg-config" >&2
        exit 1
    fi

    if pkg-config --exists icu-i18n icu-uc; then
        trace_on
        flags=$(pkg-config --cflags --libs icu-i18n icu-uc)
        trace_off

        CPPFLAGS="$CPPFLAGS -DLRC_UNICODE_ENABLE_ICU=1"
        pkg_config_flags="$pkg_config_flags $flags"
    fi

    pkg_config_add_flags libonnxruntime
    pkg_config_add_flags fftw3f
    pkg_config_setup_done=1
}

build_program() {
    model_cppflags="-DLRC_DEFAULT_MODEL_DIR=\"$DEFAULT_MODEL_DIR\""

    setup_pkg_config_flags
    mkdir -p "$(dirname "$program_path")"

    trace_on
    $CC $CPPFLAGS $model_cppflags -DLRC_CTC_INFERENCE_ENABLE_ORT=1 \
        $CFLAGS src/main.c $LDFLAGS $pkg_config_flags $DEFAULT_LDLIBS \
        -o "$program_path"
    trace_off
}

build_library() {
    model_cppflags="-DLRC_DEFAULT_MODEL_DIR=\"$DEFAULT_MODEL_DIR\""

    setup_pkg_config_flags
    mkdir -p "$(dirname "$library_path")"

    trace_on
    $CC $CPPFLAGS $model_cppflags -DLRC_CTC_INFERENCE_ENABLE_ORT=1 \
        $CFLAGS -DLYRICS_BUILD_SHARED=1 -fPIC -shared src/main.c \
        $LDFLAGS $pkg_config_flags $DEFAULT_LDLIBS \
        -o "$library_path"
    trace_off
}

case "$target" in
build|all)
    build_program
    ;;
fast_feedback)
    build_program
    trace_on
    "$program_path"
    trace_off
    ;;
lib)
    build_library
    ;;
run)
    build_program
    trace_on
    "$program_path" "$@"
    trace_off
    ;;
uninstall)
    trace_on

    rm -f "${DESTDIR}${PREFIX}/bin/${program}"
    rm -f "${DESTDIR}${PREFIX}/lib/${program}.so"
    rm -f "${DESTDIR}${PREFIX}/include/${program}.h"

    rm -rf "${DESTDIR}${PREFIX}/share/${program}/models"
    rmdir "${DESTDIR}${PREFIX}/share/${program}" 2>/dev/null || true
    rm -f "${DESTDIR}${PREFIX}/share/bash-completion/completions/${program}"
    rm -f "${DESTDIR}${PREFIX}/share/zsh/site-functions/_${program}"
    rm -f "${DESTDIR}${PREFIX}/share/fish/vendor_completions.d/${program}.fish"

    uninstall_opt \
        "${program}.1" \
        "${DESTDIR}${PREFIX}/share/man/man1/${program}.1"
    uninstall_opt "etc" "${DESTDIR}/etc/${program}"
    uninstall_opt \
        "${program}.desktop" \
        "${DESTDIR}/usr/share/applications/${program}.desktop"

    trace_off
    exit
    ;;
install)
    trace_on

    DEFAULT_MODEL_DIR="${PREFIX}/share/${program}/models"
    build_program
    build_library

    install -Dm755 "$program_path" "${DESTDIR}${PREFIX}/bin/${program}"
    install -Dm755 "$library_path" "${DESTDIR}${PREFIX}/lib/${program}.so"
    install -Dm644 src/${program}.h "${DESTDIR}${PREFIX}/include/${program}.h"
    install_opt -dm755 "models" \
        "${DESTDIR}${PREFIX}/share/${program}/models"
    install_opt \
        -Dm644 "${program}.1" \
        "${DESTDIR}${PREFIX}/share/man/man1/${program}.1"
    install_opt \
        -Dm644 "completions/${program}" \
        "${DESTDIR}${PREFIX}/share/bash-completion/completions/${program}"
    install_opt \
        -Dm644 "completions/_${program}" \
        "${DESTDIR}${PREFIX}/share/zsh/site-functions/_${program}"
    install_opt \
        -Dm644 "completions/${program}.fish" \
        "${DESTDIR}${PREFIX}/share/fish/vendor_completions.d/${program}.fish"
    install_opt -dm755 "etc" "${DESTDIR}/etc/${program}"
    install_opt -Dm755 \
        "${program}.desktop" \
        "${DESTDIR}/usr/share/applications/${program}.desktop"

    trace_off
    exit
    ;;
test)
    setup_pkg_config_flags

    find src cbase -iname "*.c" | grep -v '/main[^/]*\.c$' | sort \
        | while read -r module; do
        name=$(basename "$module" | sed 's/\.c$//')
        test_exe="/tmp/${name}_test"

        if [ -n "$target_arg" ] \
           && [ "$target_arg" != "$name" ] \
           && [ "$target_arg" != "${name}.c" ]; then
            continue
        fi

        printf '\nTesting %s ...\n' "$module"

        flags=$(awk '/\/\/ flags:/ { $1=$2=""; print $0 }' "$module")

        trace_on
        if $CC $CPPFLAGS $CFLAGS \
              "-DTESTING_$name=1" -DTESTING=1 "$module" \
              $LDFLAGS $pkg_config_flags $DEFAULT_LDLIBS $flags \
              -o "$test_exe"; then
            if ! "$test_exe"; then
                if command -v gdb >/dev/null 2>&1; then
                    gdb --quiet \
                        -ex run -ex backtrace -ex quit \
                        "$test_exe" 2>&1 | $xsel
                fi
                exit 1
            fi
        else
            exit 1
        fi
        trace_off
    done
    ;;
debug)
    build_program
    ;;
check)
    if [ -n "$target_arg" ]; then
        CC=gcc CFLAGS="-fanalyzer" "$0" build "$target_arg"
    else
        CC=gcc CFLAGS="-fanalyzer" "$0" build
    fi

    analyzer_flags="--analyze -Xanalyzer -analyzer-output=text"
    analyzer_flags="$analyzer_flags -Xanalyzer -analyzer-werror"
    analyzer_flags="$analyzer_flags -Xanalyzer -analyzer-opt-analyze-headers"
    analyzer_flags="$analyzer_flags -Wno-unused-command-line-argument"

    if [ -n "$target_arg" ]; then
        CC=clang CFLAGS="$analyzer_flags" "$0" build "$target_arg"
    else
        CC=clang CFLAGS="$analyzer_flags" "$0" build
    fi
    ;;
clean)
    rm -rf bin
    ;;
help|-h|--help)
    usage
    ;;
*)
    usage >&2
    exit 1
    ;;
esac
