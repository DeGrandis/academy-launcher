"""Response-surface model of the conquest_mechanics strengths, from an evolve.py --mode mechanics run.

Usage: python tools/balance/mechanics_model.py work/balance/mech [--top 8]

Per candidate (a point in strength space, 12+ matches) it computes outcome metrics, then fits each metric with a
quadratic model in the normalized strengths (linear, squared and pairwise terms, weighted by matches played), and
reports: each mechanic's effect (the metric's change from off to full strength, the others at mid), the strongest
pairwise interactions, and the best strength combinations predicted on a grid. Predictions far from sampled points
are less certain; confirm the picks with tools/balance/confirm.py.
"""

import itertools
import json
import statistics
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evolve  # noqa: E402

GENES = evolve.MECH_GENES
NAMES = list(GENES)


def metrics(results):
    played = [g for g in results if g["complete"]]
    if not played:
        return None
    r = sum(g["winner"] == 1 for g in played)
    c = sum(g["winner"] == 2 for g in played)
    decided = [g for g in played if g["winner"]]
    comebacks = [g["comeback"] for g in decided if g["comeback"] is not None]
    score, parts = evolve.fitness(results)
    return {
        "fitness": score,
        "decisive": (r + c) / len(played),
        "republic_share": (r + 0.5) / (r + c + 1),
        "lead_changes": statistics.mean(g["lead_changes"] for g in played),
        "comebacks": sum(comebacks) / len(comebacks) if comebacks else 0.0,
        "length": statistics.median(g["length"] for g in decided) if decided else 20.0,
        "map_balance": parts["map_balance"],
        "games": len(played),
    }


def normalize(genes):
    return [(genes[n] - GENES[n][1]) / (GENES[n][2] - GENES[n][1]) for n in NAMES]


def features(x):
    terms = [1.0] + list(x) + [v * v for v in x] + [a * b for a, b in itertools.combinations(x, 2)]
    return np.array(terms)


def term_names():
    return (["const"] + NAMES + [f"{n}^2" for n in NAMES] + [f"{a}*{b}" for a, b in itertools.combinations(NAMES, 2)])


def main(argv):
    folder = Path(argv[1])
    top = int(argv[argv.index("--top") + 1]) if "--top" in argv else 8
    state = json.loads((folder / "state.json").read_text())
    rows = []
    for cand in state["candidates"].values():
        results = [evolve.game_metrics(d) for d in sorted((folder / "games" / cand["id"]).iterdir()) if (d / "log.txt").exists()]
        m = metrics(results)
        if m:
            rows.append((cand, m))
    X = np.array([features(normalize({**{n: s[0] for n, s in GENES.items()}, **c["genes"]})) for c, _ in rows])
    W = np.sqrt(np.array([m["games"] for _, m in rows], dtype=float))
    print(f"# Mechanics model ({len(rows)} strength combinations, {sum(m['games'] for _, m in rows)} matches)\n")

    models = {}
    for metric in ["fitness", "decisive", "republic_share", "lead_changes", "comebacks", "length", "map_balance"]:
        y = np.array([m[metric] for _, m in rows])
        coef, *_ = np.linalg.lstsq(X * W[:, None], y * W, rcond=None)
        predicted = X @ coef
        r2 = 1 - np.sum(W * (y - predicted) ** 2) / np.sum(W * (y - np.average(y, weights=W)) ** 2)
        models[metric] = (coef, r2)

    mid = [0.5] * len(NAMES)
    print("## Effect of each mechanic (off -> full strength, the others at mid)\n")
    print("| Mechanic (range) | " + " | ".join(models) + " |")
    print("|---|" + "---|" * len(models))
    for i, name in enumerate(NAMES):
        low, high = list(mid), list(mid)
        low[i], high[i] = 0.0, 1.0
        cells = []
        for metric, (coef, _) in models.items():
            cells.append(f"{features(high) @ coef - features(low) @ coef:+.2f}")
        print(f"| {name} ({GENES[name][1]}-{GENES[name][2]}) | " + " | ".join(cells) + " |")
    print("| model fit (R^2) | " + " | ".join(f"{r2:.2f}" for _, r2 in models.values()) + " |")

    print("\n## Strongest interactions (pairwise terms of the fitness and decisive models)\n")
    names = term_names()
    for metric in ["fitness", "decisive", "comebacks"]:
        coef = models[metric][0]
        pairs = sorted(((abs(coef[i]), names[i], coef[i]) for i in range(len(names)) if "*" in names[i]), reverse=True)[:3]
        print(f"- {metric}: " + ", ".join(f"{n} {c:+.2f}" for _, n, c in pairs))

    grid_axis = np.linspace(0, 1, 6)
    predictions = []
    for x in itertools.product(grid_axis, repeat=len(NAMES)):
        f = features(x)
        predictions.append((float(f @ models["fitness"][0]), x, {m: float(f @ c) for m, (c, _) in models.items()}))
    predictions.sort(key=lambda p: -p[0])
    print(f"\n## Best predicted combinations\n")
    print("| " + " | ".join(NAMES) + " | fitness | decisive | Republic share | lead changes | comebacks | length |")
    print("|" + "---|" * (len(NAMES) + 6))
    for score, x, values in predictions[:top]:
        strengths = [GENES[n][1] + v * (GENES[n][2] - GENES[n][1]) for n, v in zip(NAMES, x)]
        print("| " + " | ".join(f"{v:.2f}" for v in strengths) + f" | {score:.3f} | {values['decisive']:.2f} | "
              f"{values['republic_share']:.2f} | {values['lead_changes']:.2f} | {values['comebacks']:.2f} | {values['length']:.1f} |")

    print("\n## Best measured combinations\n")
    print("| Id | " + " | ".join(NAMES) + " | matches | fitness | decisive | Republic share | lead changes | comebacks | length |")
    print("|---|" + "---|" * (len(NAMES) + 7))
    for cand, m in sorted(rows, key=lambda r: -r[1]["fitness"])[:top]:
        genes = {**{n: s[0] for n, s in GENES.items()}, **cand["genes"]}
        print(f"| {cand['id']} | " + " | ".join(f"{genes[n]:.2f}" for n in NAMES) + f" | {m['games']} | {m['fitness']:.3f} | "
              f"{m['decisive']:.2f} | {m['republic_share']:.2f} | {m['lead_changes']:.2f} | {m['comebacks']:.2f} | {m['length']:.1f} |")


if __name__ == "__main__":
    main(sys.argv)
