#!/bin/sh -e

# shellcheck disable=SC2086

dir=$(dirname "$(readlink -f "$0")")
cd "$dir" || exit

# shellcheck source=./cbase/common.sh
. ./cbase/common.sh

program=$(common_get_program "$0")
program_path="bin/$program"
library_path="bin/$program.so"
cd "$dir" || exit

script=$(basename "$0")
common_build_parse_args "$@"

case "$mode" in
all|build|check|clean|debug|debug-fast|fast_feedback|install|lib|run|test|uninstall)
    ;;
*)
    common_build_unknown_mode
    ;;
esac

common_build_print_invocation "$script"

CC=$(common_get_compiler "$mode")

if [ "$#" -gt 0 ]; then
    shift
fi

PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}

CPPFLAGS="${CPPFLAGS:-}"

CFLAGS="${CFLAGS:-}"
LDFLAGS="${LDFLAGS:-}"
LDFLAGS="$LDFLAGS -lm"

CPPFLAGS="$CPPFLAGS -I. -Icbase -Isrc"

case "$OS" in
*Linux*)
    LDFLAGS="$LDFLAGS -ldl"
    ;;
*Darwin*)
    CPPFLAGS="$CPPFLAGS -D_DARWIN_C_SOURCE"
    ;;
esac

CFLAGS="$CFLAGS -std=c11"
CFLAGS="$CFLAGS -Wfatal-errors"

case "$mode" in
build|all|run|lib)
    CFLAGS="$CFLAGS -O2 -g"
    ;;
debug-fast)
    CFLAGS="$CFLAGS -g2 -O2"
    CFLAGS="$CFLAGS -fsanitize=undefined"
    CPPFLAGS="$CPPFLAGS -DDEBUGGING=1"
    ;;
fast_feedback)
    ;;
debug)
    CFLAGS="$CFLAGS -g3 -Og"
    CPPFLAGS="$CPPFLAGS -DDEBUGGING=1"
    ;;
test)
    CFLAGS="$CFLAGS -g3 -Og -DDEBUGGING=1"
    ;;
check)
    ;;
all|build|check|clean|debug|debug-fast|fast_feedback|install|lib|run|test|uninstall)
    ;;
*)
    common_build_unknown_mode
    ;;
esac

pkg_config_flags=
pkg_config_setup_done=0

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
        $CFLAGS src/main.c $LDFLAGS $pkg_config_flags \
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
        $LDFLAGS $pkg_config_flags \
        -o "$library_path"
    trace_off
}

case "$mode" in
build|all|debug-fast)
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

    common_uninstall_opt \
        "${program}.1" \
        "${DESTDIR}${PREFIX}/share/man/man1/${program}.1"
    common_uninstall_opt "etc" "${DESTDIR}/etc/${program}"
    common_uninstall_opt \
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
    common_install_opt -dm755 "models" \
        "${DESTDIR}${PREFIX}/share/${program}/models"
    common_install_opt \
        -Dm644 "${program}.1" \
        "${DESTDIR}${PREFIX}/share/man/man1/${program}.1"
    common_install_opt \
        -Dm644 "completions/${program}" \
        "${DESTDIR}${PREFIX}/share/bash-completion/completions/${program}"
    common_install_opt \
        -Dm644 "completions/_${program}" \
        "${DESTDIR}${PREFIX}/share/zsh/site-functions/_${program}"
    common_install_opt \
        -Dm644 "completions/${program}.fish" \
        "${DESTDIR}${PREFIX}/share/fish/vendor_completions.d/${program}.fish"
    common_install_opt -dm755 "etc" "${DESTDIR}/etc/${program}"
    common_install_opt -Dm755 \
        "${program}.desktop" \
        "${DESTDIR}/usr/share/applications/${program}.desktop"

    trace_off
    exit
    ;;
test)
    setup_pkg_config_flags
    LDFLAGS="$LDFLAGS $pkg_config_flags"
    common_test "$target" src cbase
    ;;
debug)
    build_program
    ;;
check)
    set +e

    if [ -n "$target" ]; then
        CC=gcc CFLAGS="-fanalyzer" "$0" debug "$target"
    else
        CC=gcc CFLAGS="-fanalyzer" "$0" debug
    fi

    analyzer_flags="--analyze -Xanalyzer -analyzer-output=text"
    analyzer_flags="$analyzer_flags -Xanalyzer -analyzer-werror"
    analyzer_flags="$analyzer_flags -Xanalyzer -analyzer-opt-analyze-headers"
    analyzer_flags="$analyzer_flags -Wno-unused-command-line-argument"

    if [ -n "$target" ]; then
        CC=clang CFLAGS="$analyzer_flags" "$0" debug "$target"
    else
        CC=clang CFLAGS="$analyzer_flags" "$0" debug
    fi
    exit
    ;;
clean)
    rm -rf bin
    ;;
esac
