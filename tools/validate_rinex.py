#!/usr/bin/env python3
"""Validate a RINEX 3 observation file the way OPUS-style parsers expect.

Catches exactly the failures that got our files rejected:
  * satellite IDs outside the legal per-constellation PRN range (e.g. G34),
  * epoch headers whose satellite count != the number of observation rows that
    follow (which desyncs the parser and makes the file look truncated),
  * declared non-GPS systems with no actual records.

Usage:
    tools/validate_rinex.py FILE.rnx [FILE2.rnx ...]
    tools/validate_rinex.py --expect-fail BAD.rnx   # regression: must be invalid

Exit code 0 = all files valid (or expected-fail files were indeed invalid).
"""
import argparse
import re
import sys

# Inclusive max PRN per RINEX constellation prefix.
MAX_PRN = {"G": 32, "R": 24, "E": 36, "C": 63, "J": 10, "S": 99}

SAT_ID_RE = re.compile(r"^([GRECJS])(\d{2})")


def validate(path):
    """Return a list of human-readable problems (empty == valid)."""
    problems = []
    with open(path, "r", errors="replace") as fh:
        lines = fh.readlines()

    # --- split header / body ------------------------------------------------
    end = None
    for i, ln in enumerate(lines):
        if "END OF HEADER" in ln:
            end = i
            break
    if end is None:
        return ["no END OF HEADER record"]

    header, body = lines[: end + 1], lines[end + 1 :]

    declared = set()
    for ln in header:
        if "SYS / # / OBS TYPES" in ln and ln[0] in MAX_PRN:
            declared.add(ln[0])
    for required in ("TIME OF FIRST OBS", "INTERVAL"):
        if not any(required in ln for ln in header):
            problems.append(f"missing header record: {required}")

    # --- walk epochs --------------------------------------------------------
    seen_systems = set()
    epoch_count = 0
    i = 0
    while i < len(body):
        ln = body[i]
        if not ln.startswith(">"):
            i += 1
            continue
        epoch_count += 1
        toks = ln[1:].split()
        # Epoch line: year mon day hr min sec flag nsat
        try:
            nsat = int(toks[-1])
        except (ValueError, IndexError):
            problems.append(f"epoch {epoch_count}: cannot parse satellite count")
            i += 1
            continue

        rows = 0
        j = i + 1
        while j < len(body) and not body[j].startswith(">"):
            rec = body[j].rstrip("\n")
            if rec.strip():
                rows += 1
                m = SAT_ID_RE.match(rec)
                if not m:
                    problems.append(
                        f"epoch {epoch_count}: malformed sat id {rec[:3]!r}")
                else:
                    sysc, prn = m.group(1), int(m.group(2))
                    seen_systems.add(sysc)
                    if prn < 1 or prn > MAX_PRN[sysc]:
                        problems.append(
                            f"epoch {epoch_count}: illegal sat id "
                            f"{sysc}{prn:02d} (max {sysc}{MAX_PRN[sysc]})")
            j += 1

        if rows != nsat:
            problems.append(
                f"epoch {epoch_count}: header says {nsat} sats but "
                f"{rows} observation rows follow")
        i = j

    if epoch_count == 0:
        problems.append("no epochs found")

    # Declared systems should actually appear (and vice-versa).
    for sysc in declared - seen_systems:
        problems.append(f"system {sysc} declared in header but no records present")
    for sysc in seen_systems - declared:
        problems.append(f"system {sysc} has records but is not declared in header")

    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--expect-fail", action="store_true",
                    help="treat files as known-bad: pass only if they ARE invalid")
    args = ap.parse_args()

    rc = 0
    for path in args.files:
        problems = validate(path)
        if args.expect_fail:
            if problems:
                print(f"[OK ] {path}: correctly flagged as invalid "
                      f"({len(problems)} problem(s))")
            else:
                print(f"[BAD] {path}: expected invalid but it validated clean")
                rc = 1
        else:
            if problems:
                print(f"[BAD] {path}: {len(problems)} problem(s)")
                for p in problems[:20]:
                    print(f"        - {p}")
                if len(problems) > 20:
                    print(f"        ... and {len(problems) - 20} more")
                rc = 1
            else:
                print(f"[OK ] {path}: valid RINEX 3 observation file")
    return rc


if __name__ == "__main__":
    sys.exit(main())
