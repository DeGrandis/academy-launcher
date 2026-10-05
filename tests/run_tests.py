"""Regression tests: boot the native port with scripted input and check game state.

Usage: python tests/run_tests.py [case-name ...]

Each case runs clone_wars.exe from native_port/build-x86/bin with a fresh copy of
tests/fixtures/hdd, writes its log and screenshots to tests/results/<case>/, and
checks the last CW_WATCH sample against the case's expectations. A case also fails
if the log reports a hardware exception or a fatal error.

Cases run at TIME_SCALE x game speed (CW_TIME_SCALE); "seconds", input-script times and
screenshot frames are all in game time. A case that needs real time (audio) sets
"env": {"CW_TIME_SCALE": "1"}.
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
TIME_SCALE = 4
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
    # The save's dashboard images are the game's own icons, so the fixture leaves them out; copy them from the game.
    game = Path(os.environ.get("CW_GAME_ROOT", ROOT / "extracted_iso"))
    save = out / "hdd" / "E" / "UDATA" / "4c410004"
    for image, source in (("TitleImage.xbx", "gameicon.xpr"), ("SaveImage.xbx", "saveicon.xpr")):
        if (game / source).exists():
            shutil.copyfile(game / source, save / image)

    watch_spec = ",".join(case.get("watch", {}).values())
    names_by_address = {spec.split(":")[0].upper().lstrip("0"): name for name, spec in case.get("watch", {}).items()}
    script = routes[case["route"]]
    if case.get("extra_input"):
        script += "," + case["extra_input"]

    env = dict(os.environ)
    if case.get("mods"):
        # Build the mods fresh so the run records its own level caches.
        subprocess.run([sys.executable, str(ROOT / "tools" / "build_mod.py"), *case["mods"]], check=True, stdout=subprocess.DEVNULL)
        env["CW_MOD_ROOT"] = str(ROOT / "work" / "mod_root" / "+".join(case["mods"]))
    env.update({
        "CW_FPS": "60",
        "CW_TIME_SCALE": str(TIME_SCALE),
        "CW_INPUT_SCRIPT": script,
        "CW_HDD_ROOT": str(out / "hdd"),
        "CW_LOG_PATH": str(out / "cw_runtime.log"),
        "CW_EXIT_MS": str(case["seconds"] * 1000),
        "CW_WATCH": watch_spec,
        "CW_WATCH_MS": "1000",
        **case.get("env", {}),
        "CW_SCREENSHOT_FRAMES": ",".join(str(frame) for frame in case.get("screenshots", [])),
    })
    started = time.time()
    try:
        subprocess.run([str(EXE)], cwd=out, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=case["seconds"] / float(env["CW_TIME_SCALE"]) + 60)
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
    for pattern in case.get("log_regex", []):
        if not any(re.search(pattern, line) for line in log):
            failures.append(f"log has no line matching /{pattern}/")
    for pattern in case.get("log_regex_absent", []):
        found = next((line for line in log if re.search(pattern, line)), None)
        if found is not None:
            failures.append(f"log has a line matching /{pattern}/: {found.strip()}")
    for text in case.get("log_contains", []):
        if not any(text in line for line in log):
            failures.append(f"log has no line containing '{text}'")
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
