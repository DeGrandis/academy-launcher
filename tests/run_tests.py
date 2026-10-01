"""Regression tests: boot the native port with scripted input and check game state.

Usage: python tests/run_tests.py [case-name ...]

Each case runs clone_wars.exe from native_port/build-x86/bin with a fresh copy of
tests/fixtures/hdd, writes its log and screenshots to tests/results/<case>/, and
checks the last CW_WATCH sample against the case's expectations. A case also fails
if the log reports a hardware exception or a fatal error.
"""

import json
import operator
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "native_port" / "build-x86" / "bin" / "clone_wars.exe"
FIXTURE_HDD = ROOT / "tests" / "fixtures" / "hdd"
RESULTS = ROOT / "tests" / "results"

OPERATORS = {"==": operator.eq, "!=": operator.ne, "<": operator.lt, "<=": operator.le, ">": operator.gt, ">=": operator.ge}
# Hardware exceptions (0xC...) are real faults; C++ exceptions (0xE06D7363) are part of normal game flow.
FAULT = re.compile(r"exception 0xC[0-9A-F]{7}|UNHANDLED exception|fatal error")


def parse_watch(line, names_by_address):
    values = {}
    for address, value in re.findall(r" ([0-9A-F]+)=('[^']*'|\S+)", line):
        name = names_by_address.get(address)
        if name is None:
            continue
        values[name] = value.strip("'") if value.startswith("'") else float(value) if value != "?" else None
    return values


def run_case(case, routes):
    name = case["name"]
    out = RESULTS / name
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    shutil.copytree(FIXTURE_HDD, out / "hdd")

    watch_spec = ",".join(case.get("watch", {}).values())
    names_by_address = {spec.split(":")[0].upper().lstrip("0"): name for name, spec in case.get("watch", {}).items()}
    script = routes[case["route"]]
    if case.get("extra_input"):
        script += "," + case["extra_input"]

    env = dict(os.environ)
    env.update({
        "CW_FPS": "60",
        "CW_INPUT_SCRIPT": script,
        "CW_HDD_ROOT": str(out / "hdd"),
        "CW_LOG_PATH": str(out / "cw_runtime.log"),
        "CW_EXIT_MS": str(case["seconds"] * 1000),
        "CW_WATCH": watch_spec,
        "CW_WATCH_MS": "1000",
        "CW_SCREENSHOT_FRAMES": ",".join(str(frame) for frame in case.get("screenshots", [])),
    })
    started = time.time()
    try:
        subprocess.run([str(EXE)], cwd=out, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=case["seconds"] + 30)
    except subprocess.TimeoutExpired:
        return False, "timed out (game did not exit)"

    log = (out / "cw_runtime.log").read_text(errors="replace").splitlines()
    problems = [line for line in log if FAULT.search(line)]
    watch_lines = [line for line in log if "watch:" in line]
    last = parse_watch(watch_lines[-1], names_by_address) if watch_lines else {}
    failures = []
    for key, (op, expected) in case.get("expect", {}).items():
        actual = last.get(key)
        if actual is None or not OPERATORS[op](actual, expected):
            failures.append(f"{key} = {actual} (expected {op} {expected})")
    if problems:
        failures.append(f"{len(problems)} fault line(s), first: {problems[0].strip()}")
    detail = f"{time.time() - started:.0f}s, last watch: {last}"
    return not failures, "; ".join(failures) if failures else detail


def main():
    if not EXE.exists():
        sys.exit(f"build the port first: {EXE} not found")
    config = json.loads((ROOT / "tests" / "cases.json").read_text())
    selected = sys.argv[1:]
    cases = [case for case in config["cases"] if not selected or case["name"] in selected]
    failed = 0
    for case in cases:
        passed, detail = run_case(case, config["routes"])
        failed += not passed
        print(f"{'PASS' if passed else 'FAIL'} {case['name']}: {detail}", flush=True)
    print(f"{len(cases) - failed}/{len(cases)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
