"""Command line interface: python3 -m edgekv --port /dev/serial0 <command>"""

from __future__ import annotations

import argparse
import csv
import os
import sys
import time
from datetime import datetime
from typing import List, Optional

from .client import EdgeKV, EdgeKVError
from .reading import Reading


def _format_reading(node: str, sample: Optional[Reading], kv: EdgeKV) -> str:
    if sample is None:
        return f"{node}  (no reading yet)"
    when = kv.wall_time(sample.hlc)
    stamp = when.strftime("%H:%M:%S") if when else f"hlc {sample.hlc_ms}"
    sim = "  simulated" if sample.simulated else ""
    return f"{node}  {sample.kind:<9}{sample.value:>11.3f} {sample.unit:<5} seq {sample.sequence:<6} at {stamp}{sim}"


def _value_text(value: bytes) -> str:
    try:
        text = value.decode()
        if text.isprintable():
            return text
    except UnicodeDecodeError:
        pass
    return "0x" + value.hex()


def cmd_ping(kv: EdgeKV, args) -> int:
    started = time.monotonic()
    node = kv.ping()
    print(f"bridge is node {chr(ord('A') + node - 1)}, round trip {(time.monotonic() - started) * 1000:.1f} ms")
    return 0


def cmd_set(kv: EdgeKV, args) -> int:
    result = kv.set(args.key.encode(), " ".join(args.value).encode())
    print("OK (under-replicated: replica will be repaired)" if result.under_replicated else "OK")
    return 0


def cmd_get(kv: EdgeKV, args) -> int:
    value = kv.get(args.key.encode())
    if value is None:
        print("(not found)")
        return 1
    print(_value_text(value))
    return 0


def cmd_del(kv: EdgeKV, args) -> int:
    kv.delete(args.key.encode())
    print("OK")
    return 0


def cmd_sensors(kv: EdgeKV, args) -> int:
    for node, sample in kv.sensors().items():
        print(_format_reading(node, sample, kv))
    return 0


def cmd_history(kv: EdgeKV, args) -> int:
    for sample in kv.history(args.node.upper()):
        print(_format_reading(args.node.upper(), sample, kv))
    return 0


def cmd_watch(kv: EdgeKV, args) -> int:
    writer = None
    handle = None

    if args.csv:
        new_file = not os.path.exists(args.csv)
        handle = open(args.csv, "a", newline="")
        writer = csv.writer(handle)
        if new_file:
            writer.writerow(["read_at", "node", "kind", "value", "unit", "sequence", "sampled_at", "simulated"])

    last_sequence = {}

    try:
        while True:
            now = datetime.now().astimezone()
            print(f"--- {now.strftime('%Y-%m-%d %H:%M:%S')}")

            for node, sample in kv.sensors().items():
                print(_format_reading(node, sample, kv))

                if writer and sample is not None and last_sequence.get(node) != sample.sequence:
                    last_sequence[node] = sample.sequence
                    when = kv.wall_time(sample.hlc)
                    writer.writerow([
                        now.isoformat(timespec="seconds"), node, sample.kind, f"{sample.value:.3f}",
                        sample.unit, sample.sequence, when.isoformat(timespec="seconds") if when else "",
                        int(sample.simulated),
                    ])

            if handle:
                handle.flush()

            if args.count and args.count > 0:
                args.count -= 1
                if args.count == 0:
                    return 0

            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0
    finally:
        if handle:
            handle.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="edgekv", description="Raspberry Pi client for the edge-kv cluster")
    parser.add_argument("--port", default=os.environ.get("EDGEKV_PORT", "/dev/serial0"),
                        help="serial port of the bridge node (default $EDGEKV_PORT or /dev/serial0)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=10.0, help="seconds to wait for an answer")

    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("ping", help="check the bridge link").set_defaults(func=cmd_ping)

    p = sub.add_parser("set", help="store a value")
    p.add_argument("key")
    p.add_argument("value", nargs="+")
    p.set_defaults(func=cmd_set)

    p = sub.add_parser("get", help="read a value")
    p.add_argument("key")
    p.set_defaults(func=cmd_get)

    p = sub.add_parser("del", help="delete a key")
    p.add_argument("key")
    p.set_defaults(func=cmd_del)

    sub.add_parser("sensors", help="latest reading of every node").set_defaults(func=cmd_sensors)

    p = sub.add_parser("history", help="recent readings of one node")
    p.add_argument("node", choices=["A", "B", "C", "D", "a", "b", "c", "d"])
    p.set_defaults(func=cmd_history)

    p = sub.add_parser("watch", help="poll all sensors, optionally log to CSV")
    p.add_argument("--interval", type=float, default=10.0)
    p.add_argument("--csv", help="append readings to this CSV file")
    p.add_argument("--count", type=int, default=0, help="stop after N polls (0 = forever)")
    p.set_defaults(func=cmd_watch)

    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)

    try:
        with EdgeKV(args.port, baudrate=args.baud, timeout=args.timeout) as kv:
            return args.func(kv, args)
    except EdgeKVError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except TimeoutError as error:
        print(f"error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
