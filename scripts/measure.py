#!/usr/bin/env python3
"""Collect the numbers for docs/RESULTS.md from the node consoles.

The same script measures the hardware (USB consoles of the four boards)
and the simulator (build/host/edge_sim), so both columns of RESULTS.md
come from the same procedure.

    # simulator column (real time; about 3 minutes)
    scripts/measure.py --sim --fill docs/RESULTS.md --column simulator

    # hardware: one console per board; prompts you to unplug/replug the victim
    scripts/measure.py --ports A=/dev/serial/by-id/...,B=...,C=...,D=... \\
        --log docs/test-logs/measure-hw.log --fill docs/RESULTS.md --column hardware

Sections (--sections, default all):
  latency   SET/GET latency (min/avg/p95/max) from node A (Pi bridge) and B, C, D
  failover  power off the victim: view change and first GET served elsewhere;
            writes during the outage; power on: catch-up time, keys repaired
  loss      'fault loss 10' / '20' on every node, 'bench' from A and B
            (hardware: needs the -fi images)
  sensors   per node: samples, write success rate, read failures (status)
  wear      per node: hottest EEPROM page writes per hour -> years to 1M (stats)

Run 'sensors' and 'wear' after a long run (the overnight soak): they read
counters since boot. Needs pyserial for --ports.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

NODES = ["A", "B", "C", "D"]
BRIDGE = "A"
SECTIONS = ["latency", "failover", "loss", "sensors", "wear"]

# ---------------------------------------------------------------------------
# Parsing (console output formats: src/console.cpp, src/node_runtime.cpp)
# ---------------------------------------------------------------------------

# "   12.345 view: AB-D" (board) / "  75.502 [A] view: AB-D" (simulator)
LOG_LINE = re.compile(r"^\s*(\d+\.\d{3})\s+(?:\[([A-D])\]\s+)?(.*)$")
ECHO = re.compile(r"^> (.*)$")
OP_RESULT = re.compile(r"^(SET|GET|DEL) (\S+): (\S+) \(served by (\S), (\d+) attempt\(s\), (\d+) ms\)")
GET_SERVED = re.compile(r"^\s+\(served by (\S), (\d+) attempt\(s\), (\d+) ms\)")
REFUSED = re.compile(r"^(SET|GET|DEL) (\S+): refused \((\w+)\)")
RING_ALL = re.compile(r"^\s+all nodes up : primary (\S+), replica (\S+)")
BENCH = re.compile(r"^bench: (\d+) ok, (\d+) failed in (\d+) ms; latency min (\d+) avg (\d+) max (\d+) ms")
STATUS_HEAD = re.compile(r"^node (\S)\s+state (\S+)\s+view (\S+)\s+uptime (\d+)s")
SENSOR = re.compile(
    r"^sensor (\S+) \((simulated|hardware)\): (on|off), (\d+) samples, last (.*) seq (\d+), "
    r"(\d+) ok, (\d+) under-replicated, (\d+) failed, (\d+) read failures"
)
CLIENT = re.compile(r"^client\s+ok (\d+)\s+failed (\d+)\s+attempts (\d+)\s+local (\d+)")
SERVER = re.compile(r"^server\s+executed (\d+)\s+not-ready (\d+)\s+dedup replays (\d+)\s+suppressed (\d+)")
RESYNC = re.compile(r"resync tx (\d+) applied (\d+) stale (\d+)")
STORAGE = re.compile(r"^storage errors (\d+)\s+compactions (\d+)")
WEAR = re.compile(r"^eeprom page writes since boot: total (\d+), hottest page (\d+) with (\d+)")
FAULT_OK = re.compile(r"^frame loss (\d+) %")
FAULT_MISSING = re.compile(r"fault injection not compiled in")


def parse_log(text: str) -> Tuple[Optional[float], Optional[str], str]:
    """(timestamp in s, node letter from the simulator prefix, message)."""
    m = LOG_LINE.match(text)
    if not m:
        return None, None, text
    return float(m.group(1)), m.group(2), m.group(3)


def percentile(values: List[float], pct: float) -> float:
    """Nearest-rank percentile."""
    if not values:
        return float("nan")
    ordered = sorted(values)
    rank = max(1, int(-(-pct * len(ordered) // 100)))  # ceil
    return ordered[rank - 1]


def summarize(values: List[float]) -> Dict[str, float]:
    if not values:
        return {"n": 0}
    return {
        "n": len(values),
        "min": min(values),
        "avg": sum(values) / len(values),
        "p95": percentile(values, 95),
        "max": max(values),
    }


def fmt_latency(s: Dict[str, float]) -> str:
    if not s.get("n"):
        return "n/a"
    return f"{s['min']:.0f} / {s['avg']:.0f} / {s['p95']:.0f} / {s['max']:.0f} ms (n={s['n']})"


def years_to_rating(hottest_writes: int, uptime_s: float, rating: int = 1_000_000) -> Optional[float]:
    if hottest_writes <= 0 or uptime_s <= 0:
        return None
    per_hour = hottest_writes / (uptime_s / 3600.0)
    return rating / per_hour / (24 * 365)


# ---------------------------------------------------------------------------
# Backends
# ---------------------------------------------------------------------------


@dataclass
class Line:
    t: float          # host monotonic time of arrival
    node: Optional[str]
    text: str


class Backend:
    scale = 1.0       # simulated ms per host ms (simulator --speed)
    simulated = False

    def __init__(self) -> None:
        self.lines: "queue.Queue[Line]" = queue.Queue()

    def send(self, node: str, command: str) -> None:
        raise NotImplementedError

    def power_off(self, node: str) -> float:
        raise NotImplementedError

    def power_on(self, node: str) -> float:
        raise NotImplementedError

    def close(self) -> None:
        pass


class SimBackend(Backend):
    """build/host/edge_sim behind a pseudo-terminal (line-buffered output)."""

    simulated = True

    def __init__(self, binary: str, speed: float = 1.0, seed: Optional[int] = None) -> None:
        super().__init__()
        import pty

        self.scale = speed
        master, slave = pty.openpty()
        args = [binary, "--speed", str(speed)]
        if seed is not None:
            args += ["--seed", str(seed)]
        self._proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=slave, stderr=slave, text=True)
        os.close(slave)
        self._master = master
        self._current: Optional[str] = None
        self._lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self) -> None:
        buffer = b""
        while True:
            try:
                data = os.read(self._master, 4096)
            except OSError:
                return
            if not data:
                return
            buffer += data
            *complete, buffer = buffer.split(b"\n")
            now = time.monotonic()
            for raw in complete:
                text = raw.decode("utf-8", "replace").rstrip("\r")
                _, prefixed, _ = parse_log(text)
                with self._lock:
                    node = prefixed or self._current
                self.lines.put(Line(now, node, text))

    def _write(self, text: str) -> None:
        assert self._proc.stdin is not None
        self._proc.stdin.write(text + "\n")
        self._proc.stdin.flush()

    def send(self, node: str, command: str) -> None:
        with self._lock:
            self._current = node
        self._write(f"{node}: {command}")

    def power_off(self, node: str) -> float:
        t = time.monotonic()
        self._write(f"off {node}")
        return t

    def power_on(self, node: str) -> float:
        t = time.monotonic()
        self._write(f"on {node}")
        return t

    def close(self) -> None:
        try:
            self._write("quit")
            self._proc.wait(timeout=5)
        except Exception:
            self._proc.kill()


class SerialBackend(Backend):
    """One USB console per board. Unplugging a board is seen as its port
    disappearing; the script asks you to do it."""

    PORT_LOST = "<<port lost>>"
    PORT_BACK = "<<port back>>"

    # The console discards input for 1 s after boot and after a USB
    # re-plug (echo guard, BRINGUP_LOG problems 5 and 10), so nothing is
    # sent until this long after each (re)open. Each open first sends an
    # empty line: it runs any echoed boot fragment in the console's line
    # buffer as a harmless unknown command, so the fragment cannot join
    # the first real command.
    POST_OPEN_WAIT_S = 1.2

    def __init__(self, ports: Dict[str, str], baud: int = 115200) -> None:
        super().__init__()
        import serial  # pyserial

        self._serial = serial
        self._ports = ports
        self._baud = baud
        self._handles: Dict[str, object] = {}
        self._opened_at: Dict[str, float] = {}
        self._events: Dict[str, "queue.Queue[Tuple[str, float]]"] = {n: queue.Queue() for n in ports}
        self._stop = False
        for node in ports:
            threading.Thread(target=self._reader, args=(node,), daemon=True).start()
        deadline = time.monotonic() + 10
        while len(self._handles) < len(ports) and time.monotonic() < deadline:
            time.sleep(0.1)
        missing = [n for n in ports if n not in self._handles]
        if missing:
            raise SystemExit(f"measure: cannot open the console of node(s) {', '.join(missing)}")

    def _reader(self, node: str) -> None:
        lost = False
        while not self._stop:
            try:
                handle = self._serial.Serial(self._ports[node], self._baud, timeout=0.1)
            except (OSError, self._serial.SerialException):
                time.sleep(0.2)
                continue
            self._opened_at[node] = time.monotonic()
            try:
                handle.write(b"\n")
            except (OSError, self._serial.SerialException):
                pass
            self._handles[node] = handle
            if lost:
                t = time.monotonic()
                self.lines.put(Line(t, node, self.PORT_BACK))
                self._events[node].put(("back", t))
                lost = False
            buffer = b""
            try:
                while not self._stop:
                    data = handle.read(256)
                    if not data:
                        continue
                    buffer += data
                    *complete, buffer = buffer.split(b"\n")
                    now = time.monotonic()
                    for raw in complete:
                        self.lines.put(Line(now, node, raw.decode("utf-8", "replace").rstrip("\r")))
            except (OSError, self._serial.SerialException):
                t = time.monotonic()
                self._handles.pop(node, None)
                self.lines.put(Line(t, node, self.PORT_LOST))
                self._events[node].put(("lost", t))
                lost = True
                try:
                    handle.close()
                except Exception:
                    pass

    def send(self, node: str, command: str) -> None:
        handle = self._handles.get(node)
        if handle is None:
            raise RuntimeError(f"node {node} console not connected")
        wait = self._opened_at.get(node, 0.0) + self.POST_OPEN_WAIT_S - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        handle.write((command + "\n").encode())

    def _wait_event(self, node: str, kind: str, prompt: str, timeout: float = 180) -> float:
        print(f"\n>>> {prompt}", flush=True)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                event, t = self._events[node].get(timeout=0.5)
            except queue.Empty:
                continue
            if event == kind:
                print(f">>> node {node}: port {kind}", flush=True)
                return t
        raise SystemExit(f"measure: node {node} port never went '{kind}'")

    def power_off(self, node: str) -> float:
        return self._wait_event(node, "lost", f"Unplug node {node}'s USB cable now.")

    def power_on(self, node: str) -> float:
        return self._wait_event(node, "back", f"Plug node {node} back in now.")

    def close(self) -> None:
        self._stop = True
        for handle in list(self._handles.values()):
            try:
                handle.close()
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Session: commands and waiting on output
# ---------------------------------------------------------------------------


class Session:
    def __init__(self, backend: Backend, transcript: Optional[str] = None, verbose: bool = False) -> None:
        self.backend = backend
        self.history: List[Line] = []
        self._transcript = open(transcript, "a", encoding="utf-8") if transcript else None
        self._verbose = verbose
        self._t0 = time.monotonic()

    def close(self) -> None:
        self.backend.close()
        if self._transcript:
            self._transcript.close()

    def _pump(self, timeout: float) -> bool:
        try:
            line = self.backend.lines.get(timeout=timeout)
        except queue.Empty:
            return False
        self.history.append(line)
        if self._transcript:
            self._transcript.write(f"{line.t - self._t0:9.3f} [{line.node or '-'}] {line.text}\n")
        if self._verbose:
            print(f"  [{line.node or '-'}] {line.text}")
        return True

    def wait_for(self, predicate: Callable[[Line], bool], since: int, timeout: float) -> Optional[Line]:
        """First line at index >= since that matches; pumps new lines."""
        deadline = time.monotonic() + timeout
        index = since
        while True:
            while index < len(self.history):
                line = self.history[index]
                index += 1
                if predicate(line):
                    return line
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self._pump(min(remaining, 0.2))

    def settle(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds / self.backend.scale
        while time.monotonic() < deadline:
            self._pump(0.05)

    def command(self, node: str, command: str, pattern: "re.Pattern[str]", timeout: float = 45.0,
                retries_if_refused: int = 20) -> Tuple[Optional["re.Match[str]"], int]:
        """Send a console command to `node`, wait for its echo, then for a
        line from that node matching `pattern`. Returns (match, index)."""
        for _ in range(retries_if_refused + 1):
            mark = len(self.history)
            self.backend.send(node, command)
            echo = self.wait_for(lambda l: l.node == node and ECHO.match(l.text) is not None
                                 and ECHO.match(l.text).group(1).strip() == command.strip(),
                                 mark, timeout)
            if echo is None:
                return None, mark
            start = self.history.index(echo)
            refused: List[bool] = []

            def matches(line: Line) -> bool:
                if line.node != node:
                    return False
                if REFUSED.match(line.text):
                    refused.append(True)
                    return True
                return pattern.search(line.text) is not None

            found = self.wait_for(matches, start + 1, timeout)
            if found is None:
                return None, start
            if refused and REFUSED.match(found.text):
                self.settle(0.5)  # client slots busy (sampler): try again
                continue
            return pattern.search(found.text), self.history.index(found)
        return None, len(self.history)

    def lines_after(self, index: int, node: Optional[str] = None) -> List[Line]:
        return [l for l in self.history[index:] if node is None or l.node == node]

    def elapsed_ms(self, t_from: float, t_to: float) -> float:
        return (t_to - t_from) * 1000.0 * self.backend.scale


# ---------------------------------------------------------------------------
# Measurements
# ---------------------------------------------------------------------------


@dataclass
class Results:
    metrics: Dict[str, str] = field(default_factory=dict)
    raw: Dict[str, object] = field(default_factory=dict)

    def put(self, key: str, text: str, raw: object = None) -> None:
        self.metrics[key] = text
        if raw is not None:
            self.raw[key] = raw
        print(f"  {key:<22} {text}", flush=True)


def set_sampling(s: Session, on: bool) -> None:
    """Sensor samplers off while timing client ops: their writes would
    compete for client slots and add to the attempt counters."""
    for node in NODES:
        s.command(node, f"sensor {'on' if on else 'off'}", re.compile(r"^sampler (on|off)"))


def wait_cluster_ready(s: Session, timeout: float = 60) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready = True
        for node in NODES:
            m, _ = s.command(node, "status", STATUS_HEAD, timeout=10)
            if not m or m.group(2) != "ACTIVE" or m.group(3) != "ABCD":
                ready = False
                break
        if ready:
            return
        s.settle(1.0)
    raise SystemExit("measure: cluster not ACTIVE with view ABCD")


def op(s: Session, node: str, kind: str, key: str, value: str = "") -> Optional[Tuple[str, str, int, int]]:
    """One console SET/GET. Returns (status, served_by, attempts, latency_ms)."""
    command = f"{kind.lower()} {key}" + (f" {value}" if kind == "SET" else "")
    any_result = re.compile(OP_RESULT.pattern + "|" + GET_SERVED.pattern)
    m, _ = s.command(node, command, any_result)
    if not m:
        return None
    if m.group(1):  # OP_RESULT
        return m.group(3), m.group(4), int(m.group(5)), int(m.group(6))
    return "OK", m.group(7), int(m.group(8)), int(m.group(9))  # GET value line


def measure_latency(s: Session, r: Results, count: int) -> None:
    print("latency", flush=True)
    per: Dict[str, Dict[str, List[float]]] = {}
    retried = 0
    total = 0
    for node in NODES:
        per[node] = {"SET": [], "GET": []}
        for kind in ("SET", "GET"):
            for i in range(count):
                result = op(s, node, kind, f"lat/{node}/{i % 8}", f"v{i}")
                if result is None:
                    continue
                status, _, attempts, latency = result
                total += 1
                if attempts > 1:
                    retried += 1
                if status in ("OK", "UNDER_REPLICATED"):
                    per[node][kind].append(float(latency))
    for kind in ("SET", "GET"):
        bridge = summarize(per[BRIDGE][kind])
        others = summarize([v for n in NODES if n != BRIDGE for v in per[n][kind]])
        r.put(f"{kind.lower()}_bridge", fmt_latency(bridge), bridge)
        r.put(f"{kind.lower()}_other", fmt_latency(others), others)
    r.put("latency_retries", f"{retried} of {total} ops needed >1 attempt")


def find_key_with_primary(s: Session, observer: str, primary: str, prefix: str, start: int = 0) -> Tuple[str, int]:
    for i in range(start, start + 400):
        key = f"{prefix}{i}"
        m, _ = s.command(observer, f"ring {key}", RING_ALL, timeout=10)
        if m and m.group(1) == primary:
            return key, i
    raise SystemExit(f"measure: no key with primary {primary} found")


def measure_failover(s: Session, r: Results, victim: str, repair_keys: int) -> None:
    print("failover", flush=True)
    observer = "A" if victim != "A" else "B"
    writer = next(n for n in NODES if n not in (victim, observer))

    probe, last = find_key_with_primary(s, observer, victim, "fo/")
    keys = []
    for _ in range(repair_keys):
        key, last = find_key_with_primary(s, observer, victim, "fo/", last + 1)
        keys.append(key)

    if op(s, observer, "SET", probe, "before") is None:
        raise SystemExit("measure: probe SET failed")
    s.settle(1.0)

    mark = len(s.history)
    t_off_host = s.backend.power_off(victim)

    # Simulator: its own clock stamps the power-off; boards: port lost.
    off_line = None
    if s.backend.simulated:
        off_line = s.wait_for(lambda l: l.node == victim and "--- power off ---" in l.text, mark, 10)

    # Keep reading the probe from the observer until another node serves it.
    t_ok = None
    served_by = None
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        result = op(s, observer, "GET", probe)
        if result and result[0] == "OK" and result[1] != victim:
            t_ok = s.history[-1].t
            served_by = result[1]
            break

    view = s.wait_for(lambda l: l.node == observer and "view:" in l.text
                      and victim not in parse_log(l.text)[2].split("view:")[1], mark, 15)

    if s.backend.simulated and off_line is not None and view is not None:
        detect_ms = (parse_log(view.text)[0] - parse_log(off_line.text)[0]) * 1000
    elif view is not None:
        detect_ms = s.elapsed_ms(t_off_host, view.t)
    else:
        detect_ms = None
    r.put("failover_detect", f"{detect_ms:.0f} ms" if detect_ms is not None else "n/a", detect_ms)

    client_ms = s.elapsed_ms(off_line.t if off_line else t_off_host, t_ok) if t_ok else None
    r.put("failover_client", f"{client_ms:.0f} ms (served by {served_by})" if client_ms else "n/a", client_ms)

    statuses: Dict[str, int] = {}
    for i, key in enumerate(keys):
        result = op(s, writer, "SET", key, f"while-down-{i}")
        status = result[0] if result else "NO_ANSWER"
        statuses[status] = statuses.get(status, 0) + 1
    r.put("outage_writes", ", ".join(f"{v} {k}" for k, v in sorted(statuses.items())) + f" (of {len(keys)})",
          statuses)

    s.settle(1.0)
    mark = len(s.history)
    t_on_host = s.backend.power_on(victim)

    back = s.wait_for(lambda l: l.node == observer and "view:" in l.text
                      and parse_log(l.text)[2].split("view:")[1].strip() == "ABCD", mark, 60)
    active = s.wait_for(lambda l: l.node == victim and "state -> ACTIVE" in l.text, mark, 30)
    on_line = None
    if s.backend.simulated:
        on_line = s.wait_for(lambda l: l.node == victim and "--- power on ---" in l.text, mark, 5)

    if active is not None and on_line is not None:  # simulator: same clock
        catchup_ms = (parse_log(active.text)[0] - parse_log(on_line.text)[0]) * 1000
        how = "power-on -> ACTIVE"
    elif active is not None and parse_log(active.text)[0] is not None:  # board: uptime since boot
        catchup_ms = parse_log(active.text)[0] * 1000
        how = "boot -> ACTIVE (board uptime)"
    elif back is not None:
        catchup_ms = s.elapsed_ms(t_on_host, back.t)
        how = "port back -> view ABCD (underestimates: USB enumerates after boot)"
    else:
        catchup_ms, how = None, ""
    r.put("catchup", f"{catchup_ms:.0f} ms ({how})" if catchup_ms is not None else "n/a", catchup_ms)

    view_ms = s.elapsed_ms(on_line.t if on_line else t_on_host, back.t) if back else None
    r.put("catchup_view", f"{view_ms:.0f} ms" if view_ms is not None else "n/a", view_ms)

    s.settle(3.0)
    repaired_logged = sum(1 for l in s.lines_after(mark, victim) if "repaired from" in l.text)
    m, _ = s.command(victim, "stats", RESYNC)
    applied = int(m.group(2)) if m else None

    correct = 0
    for i, key in enumerate(keys):
        result = op(s, victim, "GET", key)
        if result and result[0] == "OK" and result[1] == victim:
            line = next((l for l in reversed(s.history) if l.node == victim and l.text.startswith(f"{key} = ")), None)
            if line and f'"while-down-{i}"' in line.text:
                correct += 1
    r.put("keys_repaired",
          f"{correct}/{len(keys)} written while down served by {victim} afterwards; "
          f"resync applied {applied if applied is not None else 'n/a'}, repair log lines {repaired_logged}",
          {"correct": correct, "expected": len(keys), "resync_applied": applied})


def snapshot(s: Session, node: str) -> Dict[str, int]:
    out: Dict[str, int] = {}
    m, index = s.command(node, "stats", STORAGE)
    for line in s.history[max(0, index - 12): index + 1]:
        if line.node != node:
            continue
        if (c := CLIENT.match(line.text)):
            out["attempts"] = int(c.group(3))
        if (c := SERVER.match(line.text)):
            out["dedup_replays"] = int(c.group(3))
    return out


def measure_loss(s: Session, r: Results, bench: int, levels=(10, 20)) -> None:
    print("loss", flush=True)
    for pct in levels:
        armed = True
        for node in NODES:
            m, index = s.command(node, f"fault loss {pct}",
                                 re.compile(FAULT_OK.pattern + "|" + FAULT_MISSING.pattern))
            if not m or not FAULT_OK.match(m.group(0)):
                armed = False
                break
        if not armed:
            for origin in (BRIDGE, "B"):
                r.put(f"loss{pct}_{origin}", "skipped: flash the -fi images (fault injection)")
            r.put(f"loss{pct}_retries", "skipped")
            continue

        before = {n: snapshot(s, n) for n in NODES}
        for origin in (BRIDGE, "B"):
            m, _ = s.command(origin, f"bench {bench}", BENCH, timeout=max(120.0, bench * 6.0))
            if m:
                ok, failed, total_ms, lo, avg, hi = (int(x) for x in m.groups())
                r.put(f"loss{pct}_{origin}", f"{ok} ok, {failed} failed; latency {lo} / {avg} / {hi} ms (min/avg/max)",
                      {"ok": ok, "failed": failed, "min": lo, "avg": avg, "max": hi, "total_ms": total_ms})
            else:
                r.put(f"loss{pct}_{origin}", "no bench result")
        after = {n: snapshot(s, n) for n in NODES}
        for node in NODES:
            s.command(node, "fault clear", re.compile(r"^fault armed"))

        extra = sum(after[n].get("attempts", 0) - before[n].get("attempts", 0) for n in (BRIDGE, "B")) - 2 * bench
        replays = sum(after[n].get("dedup_replays", 0) - before[n].get("dedup_replays", 0) for n in NODES)
        r.put(f"loss{pct}_retries", f"{extra} extra client attempts, {replays} duplicate requests answered from cache",
              {"extra_attempts": extra, "dedup_replays": replays})
        s.settle(3.0)


def measure_sensors(s: Session, r: Results) -> None:
    print("sensors", flush=True)
    for node in NODES:
        m, _ = s.command(node, "status", SENSOR)
        if not m:
            r.put(f"sensor_{node}", "n/a")
            continue
        kind, mode, _, samples, _, _, ok, under, failed, read_failures = m.groups()
        ok, under, failed, samples = int(ok), int(under), int(failed), int(samples)
        writes = ok + under + failed
        committed = f"{100.0 * (ok + under) / writes:.1f} % writes committed" if writes else "no writes yet"
        r.put(f"sensor_{node}",
              f"{kind} ({mode}): {samples} samples, {committed} "
              f"({under} under-replicated, {failed} failed), {read_failures} read failures",
              {"kind": kind, "mode": mode, "samples": samples, "ok": ok, "under": under,
               "failed": failed, "read_failures": int(read_failures)})


def measure_wear(s: Session, r: Results) -> None:
    print("wear", flush=True)
    for node in NODES:
        head, _ = s.command(node, "status", STATUS_HEAD)
        m, _ = s.command(node, "stats", WEAR)
        if not head or not m:
            r.put(f"wear_{node}", "n/a (no 'eeprom page writes' line in stats)")
            continue
        uptime = int(head.group(4))
        hottest = int(m.group(3))
        years = years_to_rating(hottest, uptime)
        r.put(f"wear_{node}",
              f"hottest page {hottest} writes in {uptime / 3600:.2f} h -> "
              + (f"{years:.1f} years to 1M" if years else "n/a (no writes yet)"),
              {"hottest": hottest, "uptime_s": uptime, "years": years})


# ---------------------------------------------------------------------------
# RESULTS.md filling
# ---------------------------------------------------------------------------


def fill_results(path: str, column: str, metrics: Dict[str, str]) -> int:
    """Put metrics into the rows whose first cell is `id`, in `column`."""
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    header: Optional[List[str]] = None
    changed = 0
    for i, line in enumerate(lines):
        if not line.startswith("|"):
            header = None
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if header is None:
            header = [c.lower() for c in cells]
            continue
        if set(line.replace("|", "").strip()) <= set("-: "):
            continue
        key = cells[0].strip("`")
        if key in metrics and column.lower() in header:
            cells[header.index(column.lower())] = metrics[key].replace("|", "/")
            lines[i] = "|" + "|".join(f" {c} " if c else " " for c in cells) + "|"
            changed += 1
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    return changed


# ---------------------------------------------------------------------------


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument("--sim", action="store_true", help="measure build/host/edge_sim")
    target.add_argument("--ports", help="A=/dev/...,B=...,C=...,D=... (board consoles)")
    p.add_argument("--sim-binary", default=os.path.join(os.path.dirname(__file__), "..", "build", "host", "edge_sim"))
    p.add_argument("--speed", type=float, default=1.0, help="simulator speed (timings are scaled back)")
    p.add_argument("--seed", type=int, help="simulator seed")
    p.add_argument("--sections", default=",".join(SECTIONS))
    p.add_argument("--count", type=int, default=50, help="SETs and GETs per node (latency)")
    p.add_argument("--bench", type=int, default=50, help="bench size per origin (loss)")
    p.add_argument("--repair-keys", type=int, default=10, help="keys written while the victim is down")
    p.add_argument("--victim", default="C", choices=NODES)
    p.add_argument("--log", help="append the console transcript to this file")
    p.add_argument("--json", help="write all results to this JSON file")
    p.add_argument("--fill", help="RESULTS.md to fill")
    p.add_argument("--column", default="hardware", help="column of --fill (simulator or hardware)")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    sections = [x.strip() for x in args.sections.split(",") if x.strip()]
    unknown = set(sections) - set(SECTIONS)
    if unknown:
        p.error(f"unknown section(s): {', '.join(sorted(unknown))}")

    if args.sim:
        backend: Backend = SimBackend(os.path.abspath(args.sim_binary), args.speed, args.seed)
    else:
        ports = dict(item.split("=", 1) for item in args.ports.split(","))
        if sorted(ports) != NODES:
            p.error("--ports needs A, B, C and D")
        backend = SerialBackend(ports)

    session = Session(backend, args.log, args.verbose)
    results = Results()
    try:
        session.settle(3.0)
        wait_cluster_ready(session)
        if "latency" in sections or "loss" in sections:
            set_sampling(session, False)
        if "latency" in sections:
            measure_latency(session, results, args.count)
        if "failover" in sections:
            measure_failover(session, results, args.victim, args.repair_keys)
            wait_cluster_ready(session)
            set_sampling(session, False)  # the victim rebooted with its sampler on
        if "loss" in sections:
            measure_loss(session, results, args.bench)
        set_sampling(session, True)
        if "sensors" in sections:
            measure_sensors(session, results)
        if "wear" in sections:
            measure_wear(session, results)
    finally:
        session.close()

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump({"metrics": results.metrics, "raw": results.raw,
                       "target": "simulator" if args.sim else "hardware"}, handle, indent=2, default=str)
    if args.fill:
        changed = fill_results(args.fill, args.column, results.metrics)
        print(f"filled {changed} cell(s) of column '{args.column}' in {args.fill}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
