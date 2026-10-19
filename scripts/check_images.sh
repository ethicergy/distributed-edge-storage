#!/usr/bin/env bash
#
# After 'make firmware': fault injection must be compiled out of the
# normal images (node-a..d) and present in the -fi images, and only
# node B's images power the EEPROM from a GPIO (D10). Checked on the
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

# D10: only B's images power the EEPROM from a GPIO.
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

for node in a b c d; do
    want_power=no
    [ "$node" = b ] && want_power=yes

    check "node-$node" no
    check "node-$node-fi" yes
    check_power "node-$node" "$want_power"
    check_power "node-$node-fi" "$want_power"
done

exit $status
