#!/usr/bin/env python3
"""Charts for the presentation (docs/SLIDES_DATA.md) into docs/figures/.

    python3 -m venv /tmp/plotenv && /tmp/plotenv/bin/pip install matplotlib
    /tmp/plotenv/bin/python scripts/make_figures.py

  latency_sim_vs_hw.png   RESULTS.md section 1, simulator vs hardware
  chip_temp_soak.png      chip temp per node over the soak, parsed from
                          docs/test-logs/soak-sat24.log (+ room temp, D's DHT11)
  failover_timeline.png   RESULTS.md section 2 (failure) and the H6 boot of
                          node B on Sun 25 (docs/test-logs/sun25-htests.log)
"""

import re
from datetime import datetime, timedelta
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "figures"
SOAK_LOG = ROOT / "docs" / "test-logs" / "soak-sat24.log"

NODE_COLORS = {"A": "#1f77b4", "B": "#ff7f0e", "C": "#2ca02c", "D": "#d62728"}
IST = timedelta(hours=5, minutes=30)


def latency_chart() -> None:
    # RESULTS.md section 1: min / avg / p95 / max in ms.
    metrics = ["SET from A\n(bridge)", "SET from\nB, C, D", "GET from A", "GET from\nB, C, D"]
    sim = [(6, 11, 14, 17), (3, 42, 15, 2428), (3, 6, 8, 9), (1, 5, 9, 9)]
    hw = [(24, 30, 34, 36), (22, 30, 35, 56), (5, 5, 6, 8), (0, 5, 7, 10)]

    fig, ax = plt.subplots(figsize=(9, 4.8))
    width = 0.36
    for i, (s, h) in enumerate(zip(sim, hw)):
        for offset, data, color, label in ((-width / 2, s, "#9aa5b1", "simulator"), (width / 2, h, "#1f77b4", "hardware")):
            x = i + offset
            ax.bar(x, data[1], width, color=color, label=label if i == 0 else None)
            ax.plot([x, x], [max(data[0], 0.5), data[3]], color="black", linewidth=1)
            ax.plot(x, data[2], marker="_", color="black", markersize=14)
            ax.text(x, data[1] * 1.08, f"{data[1]}", ha="center", va="bottom", fontsize=8)
    ax.set_yscale("log")
    ax.set_ylim(0.5, 5000)
    ax.set_xticks(range(len(metrics)))
    ax.set_xticklabels(metrics)
    ax.set_ylabel("latency, ms (log scale)")
    ax.set_title("Latency, no loss: bar = average, line = min..max, tick = p95 (n = 50 / 150 per bar)")
    ax.annotate("sim max 2428 ms: 2 ops hit\n'dedup table full' (RESULTS §6)", xy=(1 - width / 2, 2428),
                xytext=(1.6, 900), fontsize=8, arrowprops={"arrowstyle": "->", "linewidth": 0.8})
    ax.legend(loc="upper right")
    ax.grid(axis="y", which="both", alpha=0.25)
    fig.tight_layout()
    fig.savefig(OUT / "latency_sim_vs_hw.png", dpi=150)
    plt.close(fig)


def parse_soak():
    """(datetime UTC, node, value) for chip temp and D's room temp; the log has
    times only, starting Sat 24 Oct 2026 17:05 UTC and crossing midnight."""
    line_re = re.compile(r"^(\d\d):(\d\d):(\d\d)\.(\d+) \[([A-D])\] (.*)$")
    chip_re = re.compile(r"chip temp (-?[\d.]+) degC")
    room_re = re.compile(r"sensor temp \(hardware\).* last ([\d.]+) degC")
    day = datetime(2026, 10, 24)
    last = None
    chip, room = [], []
    for raw in SOAK_LOG.read_text(errors="replace").splitlines():
        m = line_re.match(raw)
        if not m:
            continue
        h, mi, s, ms, node, text = m.groups()
        t = day + timedelta(hours=int(h), minutes=int(mi), seconds=int(s), milliseconds=int(ms[:3]))
        if last is not None and t < last - timedelta(hours=1):
            day += timedelta(days=1)
            t += timedelta(days=1)
        last = t
        if (c := chip_re.search(text)):
            chip.append((t, node, float(c.group(1))))
        elif node == "D" and (r := room_re.search(text)):
            room.append((t, float(r.group(1))))
    return chip, room


def chip_temp_chart() -> None:
    chip, room = parse_soak()
    fig, ax = plt.subplots(figsize=(10, 4.8))
    for node in "ABCD":
        pts = [(t + IST, v) for t, n, v in chip if n == node]
        ax.plot([p[0] for p in pts], [p[1] for p in pts], label=f"{node} die", color=NODE_COLORS[node], linewidth=1.4)
    if room:
        ax.plot([t + IST for t, _ in room], [v for _, v in room], label="room (D's DHT11)", color="black",
                linestyle="--", linewidth=1.2)
    ax.set_ylabel("degC")
    ax.set_xlabel("laptop time, IST (Sat 24 → Sun 25 Oct)")
    ax.set_title("Chip (die) temperature per node during the 8.5 h soak, with room temperature")
    ax.xaxis.set_major_formatter(matplotlib.dates.DateFormatter("%H:%M"))
    ax.set_ylim(25, 70)
    ax.grid(alpha=0.25)
    ax.legend(ncol=5, loc="center right", fontsize=8)
    fig.tight_layout()
    fig.savefig(OUT / "chip_temp_soak.png", dpi=150)
    plt.close(fig)


def failover_chart() -> None:
    fig, (top, bottom) = plt.subplots(2, 1, figsize=(10, 5.2))

    # Failure: RESULTS.md section 2 (victim C, Tue 20), ms after power off.
    events = [(0, "C powered off"), (1909, "A's view drops C\n(heartbeat timeout)"),
              (3423, "first GET of a C-owned key\nserved by D (client retry)")]
    top.axvspan(0, 1909, color="#f4c7c3", alpha=0.6)
    top.axvspan(1909, 3423, color="#fde9b8", alpha=0.6)
    for t, label in events:
        top.axvline(t, color="black", linewidth=1)
        top.text(t + 40, 0.55, f"{t} ms\n{label}", fontsize=8, va="center")
    top.set_xlim(-100, 4200)
    top.set_yticks([])
    top.set_title("Failure (C unplugged): detection < 2 s, a client sees the failover after ~3.4 s")
    top.set_xlabel("ms after power off")

    # Recovery: node B's boot during H6, Sun 25 (board uptime, s).
    phases = [(0, 1.92, "boot + storage\nrecovered", "#c9d7f0"), (1.92, 4.21, "BOOTING\n(silent, listening)", "#e2e2e2"),
              (4.21, 7.21, "CATCHUP\n(peers push)", "#d4ecd0"), (7.21, 8.2, "ACTIVE", "#b6e3b0")]
    for start, end, label, color in phases:
        bottom.axvspan(start, end, color=color)
        bottom.text((start + end) / 2, 0.5, label, ha="center", va="center", fontsize=8)
    bottom.axvline(4.30, color="black", linewidth=1)
    bottom.text(4.33, 0.12, "4.30 s: k20 repaired from D", fontsize=8)
    bottom.set_xlim(0, 8.2)
    bottom.set_yticks([])
    bottom.set_title("Recovery (node replugged): ACTIVE after 7.2 s, missed keys repaired in CATCHUP")
    bottom.set_xlabel("s after power on (board uptime)")

    fig.tight_layout()
    fig.savefig(OUT / "failover_timeline.png", dpi=150)
    plt.close(fig)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    latency_chart()
    chip_temp_chart()
    failover_chart()
    for p in sorted(OUT.glob("*.png")):
        print(p.relative_to(ROOT))


if __name__ == "__main__":
    main()
