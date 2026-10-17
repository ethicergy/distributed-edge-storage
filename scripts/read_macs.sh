#!/usr/bin/env bash
#
# Read the Wi-Fi STA MAC of every attached ESP32-C3 and print it in the
# format of the MAC table in src/platform/transport.cpp (MAC_A..MAC_D),
# ready to paste.
#
#   scripts/read_macs.sh                      every /dev/ttyACM*, in order
#   scripts/read_macs.sh /dev/ttyACM2 ...     these ports, in this order
#
# Letters are given in port order: plug the boards in (or list the
# ports) in the order A, B, C, D, and label the boards to match.
# Diagnostics go to stderr, so stdout is exactly the table.
#
# Needs PlatformIO (PATH, $PIO, or .venv/bin/pio of this repo).
# $ESPTOOL overrides the esptool command (e.g. "esptool.py").

set -u

repo_root="$(cd "$(dirname "$0")/.." && pwd)"

if [ -n "${ESPTOOL:-}" ]; then
    read -r -a esptool <<< "$ESPTOOL"
else
    pio="${PIO:-$(command -v pio || echo "$repo_root/.venv/bin/pio")}"
    if [ ! -x "$pio" ]; then
        echo "read_macs: PlatformIO not found (install it or set PIO=/path/to/pio)" >&2
        exit 2
    fi
    esptool=("$pio" pkg exec -p tool-esptoolpy -- esptool.py)
fi

if [ $# -gt 0 ]; then
    ports=("$@")
else
    shopt -s nullglob
    ports=(/dev/ttyACM*)
    shopt -u nullglob
    if [ ${#ports[@]} -gt 0 ]; then
        mapfile -t ports < <(printf '%s\n' "${ports[@]}" | sort -V)
    fi
fi

if [ ${#ports[@]} -eq 0 ]; then
    echo "read_macs: no /dev/ttyACM* found (WSL: usbipd attach --wsl --busid <id>)" >&2
    exit 2
fi

if [ ${#ports[@]} -gt 4 ]; then
    echo "read_macs: ${#ports[@]} ports, only the first 4 get letters A..D" >&2
fi

letters=(A B C D)
index=0
failed=0

for port in "${ports[@]}"; do
    output="$("${esptool[@]}" --chip esp32c3 --port "$port" read_mac 2>&1)"
    mac="$(printf '%s\n' "$output" | sed -n 's/^MAC: *\([0-9A-Fa-f:]\{17\}\).*/\1/p' | tail -n 1)"

    if [ -z "$mac" ]; then
        echo "read_macs: $port: no MAC read; esptool said:" >&2
        printf '%s\n' "$output" | tail -n 5 | sed 's/^/    /' >&2
        failed=1
        continue
    fi

    if [ $index -ge 4 ]; then
        echo "read_macs: $port: $mac (no letter left)" >&2
        continue
    fi

    by_id=""
    for link in /dev/serial/by-id/*; do
        if [ "$(readlink -f "$link" 2>/dev/null)" = "$(readlink -f "$port")" ]; then
            by_id=" ($(basename "$link"))"
        fi
    done

    letter="${letters[$index]}"
    bytes="$(printf '%s' "$mac" | tr 'a-f' 'A-F' | sed 's/^/0x/; s/:/, 0x/g')"

    [ $index -gt 0 ] && echo
    echo "// node $letter: $port$by_id"
    echo "constexpr std::uint8_t MAC_$letter[] = {"
    echo "    $bytes"
    echo "};"

    index=$((index + 1))
done

exit $failed
