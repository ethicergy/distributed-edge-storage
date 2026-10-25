#!/usr/bin/env bash
#
# After 'make firmware': every image carries the current git revision,
# fault injection must be compiled out of the normal images (node-a..d)
# and present in the -fi images, and no image powers the EEPROM from a
# GPIO (D10, superseded). Checked on the
# console texts that only exist in one kind of build.
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

# D10 (superseded Tue 20): no image powers the EEPROM from a GPIO any
# more. If the workaround comes back, pass yes for the node that uses it.
check_power() {
    local env="$1" want="$2" elf="$build/$1/firmware.elf"

    [ -f "$elf" ] || return

    local power
    power=$(strings "$elf" | grep -c "EEPROM power: GPIO")

    if [ "$want" = yes ] && [ "$power" -ge 1 ]; then
        echo "  ok  $env: EEPROM power from a GPIO"
    elif [ "$want" = no ] && [ "$power" -eq 0 ]; then
        echo "  ok  $env: EEPROM power on 3V3"
    else
        echo "  BAD $env: expected EEPROM power $( [ "$want" = yes ] && echo "from a GPIO" || echo "on 3V3" )" >&2
        status=1
    fi
}

# Every image must carry the revision of the current firmware sources
# (EDGE_GIT_REV, scripts/git_rev.py): an image built from other sources
# would be flashed under a misleading banner.
rev=$(python3 "$(dirname "$0")/git_rev.py")

check_rev() {
    local env="$1" elf="$build/$1/firmware.elf"

    [ -f "$elf" ] || return

    if strings "$elf" | grep -Fxq "$rev"; then
        echo "  ok  $env: git $rev"
    else
        echo "  BAD $env: not built from git $rev (run make firmware)" >&2
        status=1
    fi
}

case "$rev" in
    *-dirty) echo "  WARN firmware sources differ from the commit ($rev): commit before flashing for the presentation" >&2 ;;
esac

for node in a b c d; do
    check_rev "node-$node"
    check_rev "node-$node-fi"
    check "node-$node" no
    check "node-$node-fi" yes
    check_power "node-$node" no
    check_power "node-$node-fi" no
done

exit $status
