#!/bin/sh
# Validate the locally extracted ColorOS daemon and its dynamic dependencies.
set -eu
if [ "$#" -ne 1 ]; then
    printf 'Usage: %s <absolute path to extracted ColorOS runtime>\n' "$0" >&2
    exit 2
fi
runtime=$1
case "$runtime" in
    /*) ;;
    *) printf 'Use an absolute runtime path.\n' >&2; exit 2 ;;
esac
exec env LD_LIBRARY_PATH="$runtime/lib" "$runtime/linker64" --list "$runtime/ufcsd"
