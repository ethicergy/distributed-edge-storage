#!/usr/bin/env python3
"""chip_temp_soak.png for the final report: the same chart as
scripts/make_figures.py (title, series, room line), with the x-axis in elapsed
hours from the first reading of the soak instead of clock time.

    python3 -m venv /tmp/plotenv && /tmp/plotenv/bin/pip install matplotlib
    /tmp/plotenv/bin/python docs/report/make_chip_temp_elapsed.py

Reuses parse_soak() from scripts/make_figures.py; writes docs/report/ only.
Also prints min / median / max die temperature per node.
"""

import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "scripts"))

from make_figures import NODE_COLORS, parse_soak, plt  # noqa: E402


def main() -> None:
    chip, room = parse_soak()
    t0 = min(t for t, _, _ in chip + [(t, None, v) for t, v in room])

    def hours(t):
        return (t - t0).total_seconds() / 3600

    fig, ax = plt.subplots(figsize=(10, 4.8))
    for node in "ABCD":
        pts = [(hours(t), v) for t, n, v in chip if n == node]
        ax.plot([p[0] for p in pts], [p[1] for p in pts], label=f"{node} die", color=NODE_COLORS[node], linewidth=1.4)
        vals = [v for _, v in pts]
        print(f"{node}: n={len(vals)} min {min(vals):.1f} median {statistics.median(vals):.1f} max {max(vals):.1f} degC")
    if room:
        ax.plot([hours(t) for t, _ in room], [v for _, v in room], label="room (D's DHT11)", color="black",
                linestyle="--", linewidth=1.2)
    ax.set_ylabel("degC")
    ax.set_xlabel("elapsed time since the start of the soak (h)")
    ax.set_title("Chip (die) temperature per node during the 8.5 h soak, with room temperature")
    ax.set_xlim(0, None)
    ax.set_ylim(25, 70)
    ax.grid(alpha=0.25)
    ax.legend(ncol=5, loc="center right", fontsize=8)
    fig.tight_layout()
    fig.savefig(HERE / "chip_temp_soak.png", dpi=150)
    plt.close(fig)


if __name__ == "__main__":
    main()
