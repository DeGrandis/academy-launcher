# Conquest balance: overnight simulation report (2026-10-04)

About 2,160 split-screen Conquest matches were played between two computer players (one per team, player 1 idle at
a base as an observer, alternating teams), on all four Conquest maps (Geonosis, Kashyyyk, Asteroid, Rhen Var). Matches
ran at 14x game speed, 12 at a time (one per CPU core), with a 20 or 30 minute cap. The `conquest_stats` plugin
recorded outposts held, losses and damage per unit type, shots, and the winner. Tools: `tools/balance/` (see README).

Side 1 = Republic, side 2 = CIS. "Timeout" = nobody won before the cap.

## 1. Side balance

| Setup (48 matches each, 30 min cap, AI players that only capture outposts) | Republic | CIS | Timeout |
|---|---|---|---|
| Original game (Republic troop mortar silent, no splash) | 10 | 19 | 19 |
| Mortar fix at full campaign splash (what `rep_vehicles` ships now) | 23 | 6 | 19 |
| Mortar splash 50% | 17 | 11 | 20 |
| **Mortar splash 30%** | **14** | **15** | **19** |

- The original game favours CIS about 2:1. The Republic outpost troop (`drone_blue`) fires its mortar about once per
  5 blaster shots, but it was silent, had no muzzle flash and did no splash damage.
- The full-strength fix overshoots to about 4:1 for the Republic. **30% of the campaign splash** is even overall with
  AI players that only capture outposts. With the final AI (which also pushes the HQ) the same 30% setup came out
  13-1 for the Republic over 40 matches (section 2), almost all from Geonosis and Asteroid: decided matches depend
  on the AI too, so treat these as relative, not absolute.
- The AI-player tanks have identical stats on both sides; the CIS AI tank still dies about 40% more often, because
  the Republic troops (1200 hull vs 900) win the troop fights around it.

## 2. The maps matter more than the units

Whatever the unit stats, the per-map results barely move (40-match confirmation, 20 min cap):

| Candidate | Overall R/C/T | Geonosis | Kashyyyk | Asteroid | Rhen Var |
|---|---|---|---|---|---|
| Current stats, mortar 30% | 13 / 1 / 26 | 8/0/2 | 1/0/9 | 4/1/5 | 0/0/10 |
| Best evolved (c038) | 10 / 8 / 22 | 9/0/1 | 1/0/9 | 0/8/2 | 0/0/10 |
| Most decisive evolved (c056) | 5 / 19 / 16 | 5/2/3 | 0/3/7 | 0/8/2 | 0/6/4 |

- **Geonosis favours the Republic** in every variant tried (up to 12-0): the CIS HQ falls in 5-7 minutes.
- **Asteroid** swings with unit stats; it is the map most sensitive to balance changes.
- **Kashyyyk and Rhen Var stalemate**: around 90% of matches hit the cap.
- So global unit stats can make the *overall* result even (c038: 10-8) but not each map; map-level changes are needed.

## 3. Why matches stall (fun)

- In timed-out matches the HQ took damage in only 8% of cases, even when one side held 6 outposts to 1 and had 10
  troops to 0. The HQ (6000 hull, 1800 shield regenerating 10/s) cannot be cracked by one tank, and the outpost troops
  rarely reach it. Outposts only build (turrets, then troops) while a player stands in them, so a player who leaves to
  attack starves their own troop production.
- The outpost lead changes hands about 0.5 times per match and comebacks (winner behind at half time) are about 0%:
  matches either snowball one way or freeze.
- AI tanks die about 17 times per match: turrets and troops shred a lone tank.

## 4. What the evolutionary search learned (~1,700 matches, 3 phases)

Genes: troop hulls and blaster damage per side, Republic mortar splash, CIS cannon damage, tank hulls per side, turret
hull, HQ hull / shield / shield regen, and a "loss bonus" (`conquest_comeback`: the trailing side's outposts build
faster). Fitness: even sides overall (15%) and per map (30%), decisive matches (25%), match length 6-15 min (10%),
lead changes (10%), comebacks (10%).

Patterns among the best candidates:

- **Weaker turrets (x0.5-0.9)** and **lower HQ shield** make matches more decisive.
- **Republic troop blaster down (15 -> 9-12) and CIS blaster up (15 -> 16-18)**, or alternatively a much tougher CIS
  troop (900 -> 1214 hull), offset the Republic troop advantage.
- **The loss bonus was never selected**: the trailing side's outposts are usually already fully built, so faster
  building rarely helps it. A comeback mechanic has to act on something the losing side is actually short of.
- Noise is large: 12 matches per candidate is not enough to rank reliably; only candidates with 40+ matches should be
  trusted.

Best evolved stat set (c038): CIS troop hull 900 -> 1214, CIS cannon 150 -> 197, Republic troop blaster 15 -> 26, CIS
troop blaster 15 -> 9, mortar splash 0, Republic tank hull 700 -> 569, CIS tank hull 700 -> 525, turrets x0.89, HQ hull
6000 -> 7244, HQ shield 1800 -> 1288. Even overall, but it polarises the maps (Geonosis 9-0 R, Asteroid 0-8 C).

## 5. Recommendations

1. **Lower the mortar splash from 100% to about 30%** of the campaign values (`drone_blue_mortar_xpl` damageCenter
   150 105, damageEdge 105 60). Full splash is clearly too strong (23-6); 30% was even (14-15) with the outpost-only
   AI but still favours the Republic with the HQ-pushing AI (13-1, mostly Geonosis). For an even overall result the
   evolved set c038 (10-8 over 40 matches) is the best candidate found, but it needs per-map work (point 2) and
   human playtesting before shipping.
2. **Per-map tuning before more unit tuning.** Geonosis needs a CIS advantage (for example a tougher CIS HQ or CIS
   turrets on that map only); Kashyyyk and Rhen Var need a way to end. A small plugin can apply per-map multipliers at
   mission start, so the evolution can then search per-map genes.
3. **Make matches end (CS-style round pressure)**, candidates to test next:
   - *Sudden death*: after N minutes the HQ shields stop regenerating (or drop), the way CS's round timer forces a
     decision. The AI already escalates (it pushes the HQ when 2 outposts ahead, 1 after 8 minutes, tied after 14).
   - *Siege troops*: once a side holds every outpost it can, its outposts send their troops at the enemy HQ.
4. **Comebacks that matter**: give the trailing side something it lacks, for example faster tank respawns, a stronger
   HQ shield while it holds fewer outposts, or turrets that rebuild at the HQ. (A bonus on outpost build speed does
   nothing.)
5. **Tank survivability**: AI tanks die about every minute; human players will too against these troop counts. A
   modest shield or hull increase for both player tanks is worth testing for fun, not balance.

## Changes made overnight (uncommitted)

- `ai_players`: AI players now fight (nearest enemy, primary always, secondary every 2.5 s), never target their own
  troops (troops are teams 11/12), stop all input off the match screen (the end screen's Restart), and push the enemy
  HQ (shooting it first) when ahead with nothing to rebuild, with the escalation above.
- `rep_vehicles`: the Republic outpost troop's mortar has its fire sound, muzzle flash and splash damage (currently
  100%; see recommendation 1).
- New mods: `conquest_stats` (test-only telemetry), `conquest_comeback` (loss bonus; not in a preset, not recommended
  as is).
- Runtime: `CW_TIME_SCALE` (sped-up simulation, menus at a safe speed), `CW_RENDER_EVERY` (skip drawing), read-only game
  files shareable between several running copies, all test cases run at 4x by default (14/14 pass).
- Tools: `tools/balance/run_conquest.ps1`, `summarize.py`, `make_variant.py`, `evolve.py`, `confirm.py`.

Raw data: `work/balance/` (per-match logs, `ga_phase1/`, `ga/state.json`, generation reports).

## 6. Comeback and pacing mechanics (follow-up, `mods/conquest_mechanics`)

Four candidate mechanics, each with a strength:

- **Respawn scaling**: a dead player's respawn countdown runs 1 + k x (outposts behind, max 3) times as fast.
- **Last-stand turrets**: the trailing side's turrets shrug off min(75%, k x outposts behind) of their damage.
- **Bounty**: each turret, troop or tank the leading side loses gives the trailing side k seconds of build progress.
- **Sudden death**: after m minutes both HQ shields go down for good.

Stage 1 sampled 48 strength combinations (Latin hypercube over all four ranges, 576 matches) and fitted a quadratic
model with pairwise interactions (`tools/balance/mechanics_model.py`). Respawn scaling was the only mechanic that
raised both lead changes and comebacks without costing decisive matches; last stand made matches longer and less
decisive, bounty made them less decisive, and an early sudden death cancelled the other mechanics' comebacks
(respawn x sudden interaction).

Stage 2 played the candidates head to head (40 matches each, 20 min cap):

| Configuration | Republic / CIS / timeout | Lead changes per match | Comebacks | Fitness |
|---|---|---|---|---|
| No mechanics (current game) | 10 / 5 / 25 | 0.25 | 0% | 0.477 |
| **Respawn scaling 2.0** | 9 / 4 / 27 | **0.62** | **8%** | **0.528** |
| Respawn 3.0 + last stand 0.08 | 7 / 2 / 31 | 0.68 | 11% | 0.457 |
| Respawn 2.0 + last stand 0.08 (model pick) | 9 / 2 / 29 | 0.50 | 0% | 0.438 |
| Respawn 1.6 + last stand 0.08 + sudden death 12 min | 4 / 3 / 33 | 0.57 | 0% | 0.462 |

**Selected: respawn scaling at 2.0**, on by default in `conquest_mechanics` (the others stay off). The lead now changes
hands 2.5 times as often and comebacks appear, at about the same number of decided matches. Players see it: when their
side falls behind on outposts the HUD shows "BEHIND: REINFORCEMENTS FASTER", and while they wait to respawn
"BEHIND - FAST RESPAWN x3" (the pickup message line, `0x43ADD8`).

None of the four mechanics ends the stalemates on Kashyyyk and Rhen Var (about 90% timeouts in every configuration):
those need map- or AI-level changes (per-map tuning, troops that march on the HQ).
