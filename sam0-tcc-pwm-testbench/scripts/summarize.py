#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Collect testbench results into one table.

Reads the TBSUM lines that each run prints at the end. The input can be
a saved console log with many runs, a twister output directory, or stdin:

    summarize.py console.log
    summarize.py ../twister-out
    cdc_console.py | tee console.log      # then summarize.py console.log

If a board, prescaler and driver ran more than once, the last run wins.
Prints a Markdown table, so it can go straight into a PR comment.
"""

import re
import sys
from pathlib import Path

LINE = re.compile(r"TBSUM (.*)$")
FIELD = re.compile(r"(\w+)=(\S+)")

COLUMNS = [
    ("board", "Board"),
    ("prescaler", "Prescaler"),
    ("driver", "Driver"),
    ("boot", "Works after boot"),
    ("period_lost", "Period changes lost"),
    ("shrink_us", "Shorter period starts after (us)"),
    ("glitch_cpu", "CPU cycles added per update"),
    ("duty_lost", "Duty changes lost"),
    ("skipped", "Skipped matches"),
    ("verdict", "Verdict"),
]

DRIVER_ORDER = ["main", "lupd", "stop", "direct", "hybrid", "in_tree"]


def read_texts(args):
    if not args:
        yield sys.stdin.read()
        return
    for arg in args:
        path = Path(arg)
        if path.is_dir():
            # twister keeps the device output in handler.log for each scenario
            for log in sorted(path.rglob("handler.log")):
                yield log.read_text(errors="replace")
        else:
            yield path.read_text(errors="replace")


def main() -> None:
    runs = {}
    for text in read_texts(sys.argv[1:]):
        for line in text.splitlines():
            match = LINE.search(line)
            if match:
                fields = dict(FIELD.findall(match.group(1)))
                key = (fields.get("board"), fields.get("prescaler"), fields.get("driver"))
                runs[key] = fields

    if not runs:
        sys.exit("No TBSUM lines found.")

    def order(key):
        board, prescaler, driver = key
        rank = DRIVER_ORDER.index(driver) if driver in DRIVER_ORDER else len(DRIVER_ORDER)
        return (board or "", int(prescaler or 0), rank)

    print("| " + " | ".join(title for _, title in COLUMNS) + " |")
    print("|" + "---|" * len(COLUMNS))
    for key in sorted(runs, key=order):
        row = runs[key]
        print("| " + " | ".join(row.get(name, "-") for name, _ in COLUMNS) + " |")


if __name__ == "__main__":
    main()
