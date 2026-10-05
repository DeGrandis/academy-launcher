"""Evolutionary search for Conquest balance and pacing, by simulating AI-vs-AI matches.

Usage: python tools/balance/evolve.py [--until HH:MM] [--parallel 12] [--scale 14] [--minutes 20] [--resume]

Each candidate is a set of unit stats (GENES) applied as a throwaway mod (mods/bal_ga_<id>) on top of the "full"
preset. A generation plays every new candidate GAMES_PER_EVAL matches (an equal number on each Conquest map, the
idle observer alternating teams) and every surviving elite GAMES_PER_EVAL more, so the survivors' scores sharpen
over time. Fitness rewards even sides (overall and on every map), decisive matches of a good length, lead changes and
the occasional comeback (see fitness()). State and per-generation reports go to work/balance/ga/.

Games run like tools/balance/run_conquest.ps1: split-screen Conquest, two computer players (one per team), the
conquest_stats telemetry plugin, CW_TIME_SCALE, one physical core per game.
"""

import argparse
import ctypes
import datetime
import json
import math
import os
import random
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import summarize  # noqa: E402

EXE = ROOT / "native_port" / "build-x86" / "bin" / "clone_wars.exe"
OUT = ROOT / "work" / "balance" / "ga"
FULL = ["unlock_all", "academy_vehicles", "academy_powerups", "academy_waves", "mp_solo", "ai_players", "cis_spiders",
        "cis_bosses", "rep_vehicles", "conquest_comeback", "conquest_mechanics"]
MAPS = {"multi8": 8, "multi12": 9, "multi10": 10, "multi6": 11}  # position in the split-screen map list
GAMES_PER_EVAL = 12      # 3 per map
POPULATION = 12
ELITES = 4

# name: (base value in the current game, low, high, integer?)
STAT_GENES = {
    "blue_hull": (1200, 600, 1800, True),          # drone_blue (Republic troop) hull
    "red_hull": (900, 500, 1500, True),            # drone_red (CIS troop) hull
    "mortar_splash": (0.3, 0.0, 1.0, False),       # drone_blue mortar splash, fraction of the campaign mortar's
    "red_cannon": (150, 60, 260, True),            # drone_red charge cannon damage
    "blue_blaster": (15, 7, 26, True),             # drone_blue blaster damage per bolt
    "red_blaster": (15, 7, 26, True),              # drone_red blaster damage per bolt
    "rep_tank_hull": (700, 400, 1200, True),       # Republic fighter tank (the AI players' and default player tank)
    "cis_tank_hull": (700, 400, 1200, True),       # CIS fighter tank
    "turret_hull": (1.0, 0.5, 1.6, False),         # outpost turrets (750) and guard towers (800), multiplier
    "hq_hull": (6000, 2500, 9000, True),           # HQ hull
    "hq_shield": (1800, 400, 3000, True),          # HQ shield
    "hq_shield_regen": (10, 0, 20, False),         # HQ shield regeneration per second
    "comeback": (0.0, 0.0, 2.0, False),            # conquest_comeback: trailing side's outposts build faster (0 = off)
}
# --mode mechanics: the conquest_mechanics strengths, on top of the current game's stats (STAT_GENES base values).
MECH_GENES = {
    "respawn": (0.0, 0.0, 2.0, False),             # trailing side's respawn countdown runs 1 + k x outposts behind as fast
    "last_stand": (0.0, 0.0, 0.4, False),          # trailing side's turrets shrug off k x outposts behind of damage
    "bounty": (0.0, 0.0, 4.0, False),              # build seconds the trailing side gets per unit the leader loses
    "sudden": (20.0, 6.0, 20.0, False),            # minutes until HQ shields go down for good (20 = never, the cap)
}
STAT_BASE = {name: spec[0] for name, spec in STAT_GENES.items()}
GENES = STAT_GENES  # the genes being searched (main() switches to MECH_GENES with --mode mechanics)
ADDITIVE = {"mortar_splash", "comeback", "respawn", "last_stand", "bounty", "sudden"}  # mutate by +-, not x


def edits_for(genes):
    """edits.json entries setting the candidate's values."""
    genes = {**STAT_BASE, **genes}
    def setting(odf, key, value):
        return {"file": f"{odf}.odf", "find": rf"(?mi)^(\s*{key}\s*=\s*)[^\r\n/]*?([ \t]*(?://[^\r\n]*)?\r?)$",
                "replace": rf"\g<1>{value}\g<2>", "count": 1}
    splash = genes["mortar_splash"]
    turret = genes["turret_hull"]
    return [
        setting("drone_blue", "maxHull", genes["blue_hull"]),
        setting("drone_red", "maxHull", genes["red_hull"]),
        setting("drone_blue_mortar_xpl", "damageCenter", f"{500 * splash:.1f} {350 * splash:.1f}"),
        setting("drone_blue_mortar_xpl", "damageEdge", f"{350 * splash:.1f} {200 * splash:.1f}"),
        setting("drone_red_cannon_ord", "damageValue", genes["red_cannon"]),
        setting("drone_blue_blaster_ord", "damageValue", genes["blue_blaster"]),
        setting("drone_red_blaster_ord", "damageValue", genes["red_blaster"]),
        setting("rep_tank_fighter1_multi", "maxHull", genes["rep_tank_hull"]),
        setting("cis_tank_fighter_multi", "maxHull", genes["cis_tank_hull"]),
        setting("neu_bldg_outpost_turret", "maxHull", round(750 * turret)),
        setting("blaster_gtower_multi", "maxHull", round(800 * turret)),
        setting("hq_multi", "maxHull", genes["hq_hull"]),
        setting("hq_multi", "maxShield", genes["hq_shield"]),
        setting("hq_multi", "addShield", genes["hq_shield_regen"]),
    ]


def stats_env(genes):
    genes = {**STAT_BASE, **{name: spec[0] for name, spec in MECH_GENES.items()}, **genes}
    turret = genes["turret_hull"]
    return {"CW_STATS_TURRET_HULLS": f"{round(750 * turret)},{round(800 * turret)}", "CW_STATS_HQ_HULL": str(genes["hq_hull"]),
            "CW_COMEBACK": str(genes["comeback"]), "CW_MECH_RESPAWN": str(genes["respawn"]),
            "CW_MECH_LASTSTAND": str(genes["last_stand"]), "CW_MECH_BOUNTY": str(genes["bounty"]),
            "CW_MECH_SUDDEN": str(genes["sudden"] if genes["sudden"] < 19.9 else 0)}


def clip(name, value):
    base, low, high, integer = GENES[name]
    value = min(high, max(low, value))
    return int(round(value)) if integer else round(value, 3)


def mutate(genes, rate=0.4, sigma=0.2):
    child = dict(genes)
    for name in GENES:
        if random.random() < rate:
            base, low, high, _ = GENES[name]
            if name in ADDITIVE:
                child[name] = clip(name, child[name] + random.gauss(0, (high - low) * 0.15))
            else:
                child[name] = clip(name, child[name] * math.exp(random.gauss(0, sigma)))
    return child


def latin_hypercube(count):
    """count gene sets spread evenly over every gene's range (one per stratum per gene, in random order)."""
    columns = {}
    for name, (_, low, high, _) in GENES.items():
        strata = [(i + random.random()) / count for i in range(count)]
        random.shuffle(strata)
        columns[name] = [clip(name, low + u * (high - low)) for u in strata]
    return [{name: columns[name][i] for name in GENES} for i in range(count)]


def crossover(a, b):
    return {name: (a if random.random() < 0.5 else b)[name] for name in GENES}


# ---------------------------------------------------------------- games

def route(index, observer_team):
    rights = ",".join(f"{11000 + i * 350}:right" for i in range(index))
    t = max(11000 + index * 350 + 500, 14500) - 14500
    steps = ["2700:start", "4250:a", "7000:right", "7500:a", rights, f"{14500 + t}:down", f"{15000 + t}:a", f"{16500 + t}:a",
             f"{18000 + t}:black", f"{19000 + t}:start", f"{20000 + t}:a", f"{21000 + t}:a", f"{22000 + t}:a",
             f"{23000 + t}:black", f"{24000 + t}:start", f"{25000 + t}:a", f"{26000 + t}:a", f"{27000 + t}:a",
             f"{28000 + t}:black", f"{28700 + t}:black", f"{29500 + t}:a"]
    if observer_team == 2:
        steps.append(f"{30300 + t}:right")
    steps += [f"{31000 + t}:a", f"{32500 + t}:a", f"{34000 + t}:start"]
    return ",".join(steps)


def build(candidate):
    name = f"bal_ga_{candidate['id']}"
    folder = ROOT / "mods" / name
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "edits.json").write_text(json.dumps(edits_for(candidate["genes"]), indent=2) + "\n")
    mods = FULL + [name, "conquest_stats"]
    result = subprocess.run([sys.executable, str(ROOT / "tools" / "build_mod.py"), *mods], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"build of {name} failed: {result.stdout[-500:]} {result.stderr[-500:]}")
    # The joined mod names make a path too long for the plugins' DLL paths (Windows MAX_PATH): use a short folder.
    short = ROOT / "work" / "mod_root" / f"ga_{candidate['id']}"
    shutil.rmtree(short, ignore_errors=True)
    shutil.move(str(ROOT / "work" / "mod_root" / "+".join(mods)), str(short))
    return short


def run_games(jobs, args):
    """Plays jobs [(candidate, map, observer_team, directory)], at most args.parallel at once, one per core."""
    kernel32 = ctypes.windll.kernel32
    cores = max(1, (os.cpu_count() or 2) // 2)
    slots = {}  # core -> (process, job, started)
    pending = list(jobs)
    roots = {}
    while pending or slots:
        for core in range(min(args.parallel, cores)):
            if core in slots:
                process, job, started = slots[core]
                timed_out = time.time() - started > args.wall_timeout
                if process.poll() is None and not timed_out:
                    continue
                if timed_out and process.poll() is None:
                    process.kill()
                shutil.rmtree(job[3] / "hdd", ignore_errors=True)
                del slots[core]
            if not pending:
                continue
            candidate, map_name, team, directory = job = pending.pop(0)
            if candidate["id"] not in roots:
                roots[candidate["id"]] = build(candidate)
            shutil.rmtree(directory, ignore_errors=True)
            directory.mkdir(parents=True)
            shutil.copytree(ROOT / "tests" / "fixtures" / "hdd", directory / "hdd")
            shutil.copy(ROOT / "extracted_iso" / "gameicon.xpr", directory / "hdd/E/UDATA/4c410004/TitleImage.xbx")
            shutil.copy(ROOT / "extracted_iso" / "saveicon.xpr", directory / "hdd/E/UDATA/4c410004/SaveImage.xbx")
            (directory / "observer_team.txt").write_text(str(team))
            (directory / "scale.txt").write_text(str(args.scale))
            env = {k: v for k, v in os.environ.items() if not k.startswith("CW_")}
            env.update({
                "CW_MOD_ROOT": str(roots[candidate["id"]]), "CW_AUDIO": "0", "CW_FPS": "30", "CW_TIME_SCALE": str(args.scale),
                "CW_RESOLUTION": "640x480", "CW_RENDER_SCALE": "1", "CW_RENDER_EVERY": "10",
                "CW_STATS_MINUTES": str(args.minutes), "CW_STATS_EXIT": "1", "CW_EXIT_MS": str((args.minutes * 60 + 240) * 1000),
                "CW_HDD_ROOT": str(directory / "hdd"), "CW_LOG_PATH": str(directory / "log.txt"),
                "CW_INPUT_SCRIPT": route(MAPS[map_name], team), **stats_env(candidate["genes"]),
            })
            process = subprocess.Popen([str(EXE)], cwd=directory, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            kernel32.SetProcessAffinityMask(ctypes.c_void_p(int(process._handle)), ctypes.c_size_t(3 << (2 * core)))
            slots[core] = (process, job, time.time())
        time.sleep(0.5)


# ---------------------------------------------------------------- scoring

def game_metrics(directory):
    game = summarize.parse_game(directory)
    timeline = game["timeline"]
    diffs = [a - b for _, a, b in timeline if a != b]
    lead_changes = sum(1 for x, y in zip(diffs, diffs[1:]) if (x > 0) != (y > 0))
    comeback = None
    if game["winner"] and timeline:
        half = timeline[len(timeline) // 2]
        lead = half[1] - half[2]
        comeback = (lead < 0) if game["winner"] == 1 else (lead > 0)
    length = (game["end"] or (timeline[-1][0] if timeline else 0)) / 60
    return {"map": game["map"], "winner": game["winner"], "complete": game["complete"], "length": length,
            "lead_changes": lead_changes, "comeback": comeback, "fps": game["fps_median"]}


def smoothed_rate(r, c):
    return (r + 1) / (r + c + 2)


def fitness(results):
    """Score in 0..1 and its parts, from a candidate's games."""
    played = [g for g in results if g["complete"]]
    if not played:
        return 0.0, {}
    parts = {}
    map_balance = []
    for map_name in MAPS:
        games = [g for g in played if g["map"] == map_name]
        r = sum(1 for g in games if g["winner"] == 1)
        c = sum(1 for g in games if g["winner"] == 2)
        map_balance.append(1 - abs(smoothed_rate(r, c) - 0.5) * 2)
    parts["map_balance"] = statistics.mean(map_balance)
    r = sum(1 for g in played if g["winner"] == 1)
    c = sum(1 for g in played if g["winner"] == 2)
    parts["overall_balance"] = 1 - abs(smoothed_rate(r, c) - 0.5) * 2
    parts["decisive"] = (r + c) / len(played)
    decided = [g for g in played if g["winner"]]
    parts["length"] = statistics.mean(1.0 if 6 <= g["length"] <= 15 else 0.5 if g["length"] <= 20 else 0.0 for g in decided) if decided else 0.0
    parts["lead_changes"] = min(1.0, statistics.mean(g["lead_changes"] for g in played) / 3)
    comebacks = [g["comeback"] for g in decided if g["comeback"] is not None]
    rate = sum(comebacks) / len(comebacks) if comebacks else 0.0
    parts["comeback"] = max(0.0, 1 - abs(rate - 0.3) / 0.3)
    weights = {"map_balance": 0.30, "overall_balance": 0.15, "decisive": 0.25, "length": 0.10, "lead_changes": 0.10, "comeback": 0.10}
    score = sum(weights[k] * parts[k] for k in weights)
    parts.update({"republic": r, "cis": c, "timeouts": len(played) - r - c, "games": len(played), "comeback_rate": rate,
                  "median_length": statistics.median(g["length"] for g in decided) if decided else 0})
    return score, parts


# ---------------------------------------------------------------- evolution

def save(state):
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "state.json").write_text(json.dumps(state, indent=1))


def report(state):
    lines = [f"# Generation {state['generation']} ({datetime.datetime.now():%H:%M})", "",
             "| Rank | Id | Fitness | Games | R / C / timeout | Map bal | Decisive | Len | Leads | Comeback | Genes |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    ranked = sorted(state["candidates"].values(), key=lambda c: -c["score"])
    for rank, cand in enumerate(ranked[:15], 1):
        p = cand["parts"]
        if not p:
            continue
        changed = {k: v for k, v in cand["genes"].items() if v != GENES[k][0]}
        lines.append(f"| {rank} | {cand['id']} | {cand['score']:.3f} | {p['games']} | {p['republic']} / {p['cis']} / {p['timeouts']} | "
                     f"{p['map_balance']:.2f} | {p['decisive']:.2f} | {p['median_length']:.1f} | {p['lead_changes']:.2f} | "
                     f"{p['comeback_rate']:.2f} | {json.dumps(changed)} |")
    (OUT / f"gen_{state['generation']:02d}.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines[:8]), flush=True)


def evaluate(state, candidates, args):
    jobs = []
    for cand in candidates:
        start = len(cand["results"])
        for i in range(GAMES_PER_EVAL):
            map_name = list(MAPS)[i % len(MAPS)]
            n = start + i
            team = 1 + (n // len(MAPS) + int(cand["id"][1:])) % 2
            jobs.append((cand, map_name, team, OUT / "games" / cand["id"] / f"{map_name}_{n}"))
    random.shuffle(jobs)  # mix candidates and maps so slow maps spread over the batch
    run_games(jobs, args)
    for cand in candidates:
        cand["results"] = [game_metrics(d) for d in sorted((OUT / "games" / cand["id"]).iterdir()) if (d / "log.txt").exists()]
        cand["score"], cand["parts"] = fitness(cand["results"])


def new_candidate(state, genes, origin):
    state["next_id"] += 1
    cid = f"c{state['next_id']:03d}"
    state["candidates"][cid] = {"id": cid, "genes": genes, "origin": origin, "results": [], "score": 0.0, "parts": {}, "generation": state["generation"]}
    return state["candidates"][cid]


def main():
    global POPULATION, GAMES_PER_EVAL, ELITES
    parser = argparse.ArgumentParser()
    parser.add_argument("--until", default=None, help="stop starting generations after this local time (HH:MM)")
    parser.add_argument("--parallel", type=int, default=12)
    parser.add_argument("--scale", type=float, default=14)
    parser.add_argument("--minutes", type=int, default=20)
    parser.add_argument("--wall-timeout", type=float, default=420)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--population", type=int, default=POPULATION)
    parser.add_argument("--games", type=int, default=GAMES_PER_EVAL, help="matches per evaluation (a multiple of 4)")
    parser.add_argument("--generations", type=int, default=1000)
    parser.add_argument("--seed-from", default=None, help="state.json of an earlier run: its best candidates seed this one")
    parser.add_argument("--mode", choices=["stats", "mechanics"], default="stats", help="search unit stats or mechanic strengths")
    parser.add_argument("--sample", type=int, default=0, help="generation 0: this many Latin-hypercube points (plus all-off and all-mid)")
    parser.add_argument("--out", default=None, help="output folder (default work/balance/ga)")
    args = parser.parse_args()
    global GENES, OUT
    if args.mode == "mechanics":
        GENES = MECH_GENES
    if args.out:
        OUT = Path(args.out).resolve()
    POPULATION, GAMES_PER_EVAL = args.population, args.games
    ELITES = min(ELITES, max(1, POPULATION // 3))
    deadline = None
    if args.until:
        hour, minute = map(int, args.until.split(":"))
        deadline = datetime.datetime.now().replace(hour=hour, minute=minute, second=0)
        if deadline < datetime.datetime.now():
            deadline += datetime.timedelta(days=1)

    random.seed(1)
    if args.resume and (OUT / "state.json").exists():
        state = json.loads((OUT / "state.json").read_text())
        for cand in state["candidates"].values():  # genes added since the run started take their base value
            for name, spec in GENES.items():
                cand["genes"].setdefault(name, spec[0])
    else:
        shutil.rmtree(OUT, ignore_errors=True)
        state = {"generation": 0, "next_id": 0, "candidates": {}, "elites": []}
        base = {name: spec[0] for name, spec in GENES.items()}
        fresh = [new_candidate(state, dict(base), "current game" if args.mode == "mechanics" else "current (mortar 30%)")]
        if args.mode == "stats":
            fresh.append(new_candidate(state, {**base, "mortar_splash": 0.0}, "original game"))
        if args.sample:
            mid = {name: clip(name, (low + high) / 2) for name, (_, low, high, _) in GENES.items()}
            fresh.append(new_candidate(state, mid, "all mid"))
            fresh += [new_candidate(state, genes, "sample") for genes in latin_hypercube(args.sample)]
        if args.seed_from:
            earlier = json.loads(Path(args.seed_from).read_text())["candidates"].values()
            for cand in sorted(earlier, key=lambda c: -c["score"])[:3]:
                fresh.append(new_candidate(state, {**base, **cand["genes"]}, f"seed {cand['id']}"))
        while len(fresh) < POPULATION:
            fresh.append(new_candidate(state, mutate(base, rate=0.6, sigma=0.3), "random"))
        evaluate(state, fresh, args)
        report(state)
        save(state)

    while (deadline is None or datetime.datetime.now() < deadline) and state["generation"] < args.generations:
        state["generation"] += 1
        ranked = sorted(state["candidates"].values(), key=lambda c: -c["score"])
        elites = ranked[:ELITES]
        state["elites"] = [c["id"] for c in elites]
        children = []
        while len(children) < POPULATION - ELITES:
            a, b = random.sample(elites, 2) if len(elites) > 1 and random.random() < 0.7 else (random.choice(elites),) * 2
            children.append(new_candidate(state, mutate(crossover(a["genes"], b["genes"])), f"{a['id']} x {b['id']}"))
        evaluate(state, elites + children, args)  # elites get more games: their scores sharpen
        report(state)
        save(state)
    print("deadline reached", flush=True)


if __name__ == "__main__":
    main()
