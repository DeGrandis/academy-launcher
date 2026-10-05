"""Summarizes Conquest balance runs (tools/balance/run_conquest.ps1) from the conquest_stats telemetry.

Usage: python tools/balance/summarize.py work/balance/<label> [--json out.json]

Side 1 = Republic (team 1, troops team 11), side 2 = CIS (team 2, troops team 12). Player 1 sits idle at the base of
the team in observer_team.txt, so that side's "player" numbers include an idle tank.
"""

import json
import re
import sys
from collections import defaultdict
from pathlib import Path

MAPS = {"multi8": "Geonosis", "multi12": "Kashyyyk", "multi10": "Asteroid", "multi6": "Rhen Var"}
CATEGORIES = ["player", "troop", "turret", "hq", "gen", "other"]
STATS = re.compile(r"stats: (tick|final) t=(\d+) side (\d) outposts (\d+) built (\d+) \|(.*)")
CATEGORY = re.compile(r" (\w+) alive (\d+) hp (\d+) dmg (\d+) deaths (\d+) shots (\d+) \|")
WINNER = re.compile(r"stats: winner side (\d) at t=(\d+)")
LIMIT = re.compile(r"stats: time limit at t=(\d+)")
DESTROYED = re.compile(r"stats: t=(\d+) side (\d) (\w+) destroyed(?: \(slot (\d)\))?")
FRAME = re.compile(r"^\[\s*\d+\]\s+([\d.]+) d3d: frame (\d+) ")
MATCH = re.compile(r"stats: conquest match on")


def parse_game(directory):
    log = (directory / "log.txt").read_text(errors="replace").splitlines()
    game = {"name": directory.name, "map": directory.name.rsplit("_", 1)[0], "observer": None, "winner": None, "end": None,
            "timeline": [], "final": {}, "player_deaths": {1: 0, 2: 0}, "ai_deaths": {1: 0, 2: 0}, "complete": False}
    observer = directory / "observer_team.txt"
    if observer.exists():
        game["observer"] = int(observer.read_text().strip())
    scale_file = directory / "scale.txt"
    scale = float(scale_file.read_text().strip()) if scale_file.exists() else 4.0
    clock = []  # (log seconds, game t) at each stats line: log time is real time x scale
    samples = {}
    frames = []  # (log seconds, frame) during the match
    in_match = False
    for line in log:
        if MATCH.search(line):
            in_match = True
        if in_match and (match := FRAME.search(line)):
            frames.append((float(match.group(1)), int(match.group(2))))
        if match := STATS.search(line):
            kind, t, side, held, built, rest = match.groups()
            entry = {"t": int(t), "outposts": int(held), "built": int(built)}
            for category in CATEGORY.finditer(rest):
                name, alive, hp, damage, deaths, shots = category.groups()
                entry[name] = {"alive": int(alive), "hp": int(hp), "damage": int(damage), "deaths": int(deaths), "shots": int(shots)}
            samples.setdefault(int(t), {})[int(side)] = entry
            if stamp := re.match(r"^\[\s*\d+\]\s+([\d.]+)", line):
                clock.append((float(stamp.group(1)), int(t)))
            game["final"][int(side)] = entry
        elif match := WINNER.search(line):
            game["winner"], game["end"], game["complete"] = int(match.group(1)), int(match.group(2)), True
        elif match := LIMIT.search(line):
            game["end"], game["complete"] = int(match.group(1)), True
        elif (match := DESTROYED.search(line)) and match.group(3) == "player":
            game["player_deaths"][int(match.group(2))] += 1
            if match.group(4) is not None and match.group(4) != "0":  # slot 0 is the idle observer
                game["ai_deaths"][int(match.group(2))] += 1
    # Frames per game second between frame-log lines (60 = the machine kept up with CW_TIME_SCALE).
    rates = sorted((f2 - f1) / (t2 - t1) for (t1, f1), (t2, f2) in zip(frames, frames[1:]) if t2 > t1)
    game["fps_median"] = rates[len(rates) // 2] if rates else 0
    game["fps_low"] = rates[len(rates) // 10] if rates else 0
    # Game seconds per real second over the match (the speed-up actually achieved).
    if len(clock) >= 2 and clock[-1][0] > clock[0][0]:
        game["speed"] = (clock[-1][1] - clock[0][1]) / ((clock[-1][0] - clock[0][0]) / scale)
    else:
        game["speed"] = 0.0
    game["timeline"] = [(t, s[1]["outposts"], s[2]["outposts"]) for t, s in sorted(samples.items()) if 1 in s and 2 in s]
    return game


def outpost_share(game):
    """Average outposts held per side over the match, and side 1's share of all outpost-time."""
    if not game["timeline"]:
        return 0.0, 0.0, 0.5
    one = sum(a for _, a, _ in game["timeline"]) / len(game["timeline"])
    two = sum(b for _, _, b in game["timeline"]) / len(game["timeline"])
    return one, two, one / (one + two) if one + two > 0 else 0.5


def first_capture(game, side):
    for t, a, b in game["timeline"]:
        if (a if side == 1 else b) > 0:
            return t
    return None


def side_value(game, side, category, field):
    return game["final"].get(side, {}).get(category, {}).get(field, 0)


def main(argv):
    root = Path(argv[1])
    games = [parse_game(d) for d in sorted(root.iterdir()) if (d / "log.txt").exists()]
    by_map = defaultdict(list)
    for game in games:
        by_map[game["map"]].append(game)

    print(f"# Conquest balance: {root.name}\n")
    print("Side 1 = Republic, side 2 = CIS. Observer = side player 1 idles on. Outposts = average held over the match.\n")
    print("| Game | Observer | Result | Length (min) | Outposts R/C | R share | Troop deaths R/C | Troop dmg taken R/C | "
          "Turret deaths R/C | AI-tank deaths R/C (observer excluded) | HQ hp R/C | Sim fps (median / 10th pct) | Speed |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for game in games:
        one, two, share = outpost_share(game)
        result = (f"{'Republic' if game['winner'] == 1 else 'CIS'} wins" if game["winner"] else
                  ("time limit" if game["complete"] else "incomplete"))
        length = (game["end"] or (game["timeline"][-1][0] if game["timeline"] else 0)) / 60
        print(f"| {game['name']} | {game['observer']} | {result} | {length:.1f} | {one:.1f} / {two:.1f} | {share:.0%} | "
              f"{side_value(game, 1, 'troop', 'deaths')} / {side_value(game, 2, 'troop', 'deaths')} | "
              f"{side_value(game, 1, 'troop', 'damage')} / {side_value(game, 2, 'troop', 'damage')} | "
              f"{side_value(game, 1, 'turret', 'deaths')} / {side_value(game, 2, 'turret', 'deaths')} | "
              f"{game['ai_deaths'][1]} / {game['ai_deaths'][2]} | "
              f"{side_value(game, 1, 'hq', 'hp')} / {side_value(game, 2, 'hq', 'hp')} | "
              f"{game['fps_median']:.0f} / {game['fps_low']:.0f} | {game['speed']:.1f}x |")

    print("\n## Per map\n")
    print("| Map | Games | Republic wins | CIS wins | Time limit | R outpost share | Troop deaths R/C | Troop shots R/C | "
          "Turret deaths R/C | First outpost (s) R/C |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    summary = {}
    for map_name, map_games in list(by_map.items()) + [("all", games)]:
        count = len(map_games)
        wins = [sum(1 for g in map_games if g["winner"] == side) for side in (1, 2)]
        limits = sum(1 for g in map_games if g["complete"] and not g["winner"])
        share = sum(outpost_share(g)[2] for g in map_games) / count if count else 0
        totals = {field: [sum(side_value(g, side, cat, key) for g in map_games) for side in (1, 2)]
                  for field, (cat, key) in {"troop_deaths": ("troop", "deaths"), "troop_shots": ("troop", "shots"),
                                            "turret_deaths": ("turret", "deaths")}.items()}
        captures = [[c for g in map_games if (c := first_capture(g, side)) is not None] for side in (1, 2)]
        first = [sum(c) / len(c) if c else float("nan") for c in captures]
        label = MAPS.get(map_name, map_name)
        print(f"| {label} | {count} | {wins[0]} | {wins[1]} | {limits} | {share:.0%} | "
              f"{totals['troop_deaths'][0]} / {totals['troop_deaths'][1]} | {totals['troop_shots'][0]} / {totals['troop_shots'][1]} | "
              f"{totals['turret_deaths'][0]} / {totals['turret_deaths'][1]} | {first[0]:.0f} / {first[1]:.0f} |")
        summary[label] = {"games": count, "wins": wins, "time_limit": limits, "republic_outpost_share": share, **totals,
                          "first_capture": first}
    if "--json" in argv:
        Path(argv[argv.index("--json") + 1]).write_text(json.dumps({"games": games, "summary": summary}, indent=2, default=str))


if __name__ == "__main__":
    main(sys.argv)
