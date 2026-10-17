#!/usr/bin/env bash
#
# After 'make firmware': fault injection must be compiled out of the
# normal images (node-a..d) and present in the -fi images. Checked on
# the console texts that only exist in one kind of build.
#
#   scripts/check_images.sh [build dir, default .pio/build]

set -u

build="${1:-.pio/build}"
status=0

check() {
    local env="$1" want_fi="$2" elf="$build/$1/firmware.elf"

    if [ ! -f "$elf" ]; then
        echo "check_images: $elf missing" >&2
        status=1
        return
    fi

    local fi_text off_text
    fi_text=$(strings "$elf" | grep -c "frame loss %u")
    off_text=$(strings "$elf" | grep -c "fault injection not compiled in")

    if [ "$want_fi" = yes ] && [ "$fi_text" -ge 1 ] && [ "$off_text" -eq 0 ]; then
        echo "  ok  $env: fault injection compiled in"
    elif [ "$want_fi" = no ] && [ "$fi_text" -eq 0 ] && [ "$off_text" -ge 1 ]; then
        echo "  ok  $env: fault injection compiled out"
    else
        echo "  BAD $env: expected fault injection $( [ "$want_fi" = yes ] && echo in || echo out )" >&2
        status=1
    fi
}

for node in a b c d; do
    check "node-$node" no
    check "node-$node-fi" yes
done

exit $status
