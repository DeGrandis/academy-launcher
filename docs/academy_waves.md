# Thule Moon Academy waves

How the Academy wave script works. The full tables (every wave for 1-4 players) are the game's own data, so they are
not published here; generate them from your own copy of the game:

```
python tools/re/academy_waves.py --markdown work/academy_waves.md --json work/academy_waves.json
```

The engine code is `ThuleAcademyScript` (see `symbols/manual.csv` and `native_port/runtime/game/GameObjects.h`).

## How waves work

1. **Pick a wave set.** When the match starts, the script reads the number of players (1-4) and loads one of four
   wave sets. Each set has 26 waves.
2. **Run a wave, one group at a time.** A wave is a list of *groups*. Running a group means:
   - spawn its units (each one an ODF at a named spawn point, sent along that spawn point's path towards the base),
   - play its voice-over line, if it has one (`MWM26_xxx` files in `Data/Sounds/VO`, the Academy instructor),
   - start its bonus-path effect, if it has one (`BonusPath*` / `EndPath*` paths in the map; bonus waves use these).
3. **Wait before the next group.** Unless the group is marked *keeps going*, the script first waits until **every
   enemy is dead**. Then it waits the group's **delay** (seconds) and runs the next group. So a group's delay is the
   pause *after* it, and the last group's delay is the pause before the next wave.
4. **Finish the wave.** After its last group, the wave number goes up and the **Academy level** goes up by the wave's
   level step. Level only matters for enemy toughness: every unit spawns with its health and ammo multiplied by
   `1 + 0.5 * floor((level - 1) / 20)` (x1 for levels 1-20, x1.5 for 21-40, ...).
5. **Loop.** After wave 26 the script starts again at wave 1 with the level it reached, so later loops are tougher.

Special waves:
- **Bonus waves** (level step 0, every 4th wave): no enemies, a sequence of bonus-path effects instead.
- **Gladiator** (2+ players, the bonus waves): a `GLADIATOR` entry ends co-op; players are put on separate teams and
  fight each other (the "elimination" round).
- **Wave 26** is the finale: a voice line, then an `EndPath` effect sequence, then a 20 s pause before the loop.

### Reading a wave table

| Column | Meaning |
|---|---|
| Group | order within the wave |
| Spawns | units created when the group runs (ODF names, so they can be looked up in `data.zwp`) |
| Then | what happens before the next group: *clear* = wait until all enemies are dead, then the delay |
| Also | voice-over line or bonus-path effect started with the group |

For example, wave 2 for one player:

| Group | Spawns | Then | Also |
|---|---|---|---|
| 1 | - | clear, 2 s | voice MWM26_075 (wave intro) |
| 2 | 2x cis_bike_speeder | **0.5 s, keeps going** | |
| 3 | 2x cis_bike_speeder | clear, 1 s | |
| 4 | 2x CIS_tank_fighter | clear, 1 s | bonus path BonusPath2_4 |
| 5 | 2x cis_bike_speeder, 2x CIS_tank_fighter | clear, 2 s | |
| 6 | - | clear, 3 s | voice MWM26_096 (wave outro) |

The intro line plays, two bikes arrive and two more follow half a second later without waiting. Once all four
are destroyed two fighter tanks come in, then a mixed group; after the last kill the outro plays and wave 3 starts
3 s later.

Change waves with a plugin (the tables are in the script object; `ThuleAcademyScript::wave()` in GameObjects.h).

The generated tables list each player count (1-4) as 26 such waves.
