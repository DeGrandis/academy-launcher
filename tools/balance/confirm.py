"""Confirmation runs for evolve.py results: plays the chosen candidates side by side (same batch, same conditions)
and prints per-map results.

Usage: python tools/balance/confirm.py <state.json> <id> [<id> ...] [--games 48] [--minutes 20] [--scale 14]
"""

import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evolve  # noqa: E402

OUT = evolve.ROOT / "work" / "balance" / "confirm"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("state")
    parser.add_argument("ids", nargs="+")
    parser.add_argument("--games", type=int, default=48)
    parser.add_argument("--minutes", type=int, default=20)
    parser.add_argument("--scale", type=float, default=14)
    parser.add_argument("--parallel", type=int, default=12)
    parser.add_argument("--wall-timeout", type=float, default=420)
    args = parser.parse_args()
    candidates = json.loads(Path(args.state).read_text())["candidates"]
    chosen = []
    for cid in args.ids:
        cand = dict(candidates[cid])
        cand["genes"] = {**{k: v[0] for k, v in evolve.GENES.items()}, **cand["genes"]}
        cand["id"] = f"confirm_{cid}"
        chosen.append(cand)
    jobs = []
    for cand in chosen:
        for n in range(args.games):
            map_name = list(evolve.MAPS)[n % len(evolve.MAPS)]
            jobs.append((cand, map_name, 1 + (n // len(evolve.MAPS)) % 2, OUT / cand["id"] / f"{map_name}_{n}"))
    jobs.sort(key=lambda job: job[3].name)  # interleave candidates
    evolve.run_games(jobs, args)

    print("| Candidate | Fitness | Games | R / C / timeout | " + " | ".join(evolve.summarize.MAPS[m] + " R/C/T" for m in evolve.MAPS) +
          " | Median length | Lead changes | Comebacks |")
    print("|---|---|---|---|" + "---|" * len(evolve.MAPS) + "---|---|---|")
    for cand in chosen:
        results = [evolve.game_metrics(d) for d in sorted((OUT / cand["id"]).iterdir()) if (d / "log.txt").exists()]
        score, parts = evolve.fitness(results)
        per_map = []
        for m in evolve.MAPS:
            games = [g for g in results if g["map"] == m and g["complete"]]
            per_map.append(f"{sum(g['winner'] == 1 for g in games)}/{sum(g['winner'] == 2 for g in games)}/{sum(not g['winner'] for g in games)}")
        print(f"| {cand['id']} | {score:.3f} | {parts.get('games', 0)} | {parts.get('republic', 0)} / {parts.get('cis', 0)} / "
              f"{parts.get('timeouts', 0)} | " + " | ".join(per_map) +
              f" | {parts.get('median_length', 0):.1f} | {statistics.mean(g['lead_changes'] for g in results):.2f} | "
              f"{parts.get('comeback_rate', 0):.2f} |")


if __name__ == "__main__":
    main()
