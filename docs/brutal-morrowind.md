# Brutal Morrowind 1.3-combat.2 — experimental active combat

This branch extends the physical-projectile build descended from stable 1.1. Stable blood DDS, corpse coatings, dismemberment rules and blood rendering are retained. Extract the full Windows ZIP separately and use copied saves. It reuses your OpenMW configuration and requires Morrowind data. Launcher/editor are not bundled.

## Controls

- Hold **right mouse button** to raise/hold a carried shield with a compatible one-handed weapon or unarmed stance.
- Press **Q** while moving sideways/backwards to dodge. It is also available with a readied spell or sheathed hands. Standing still dodges backwards; forward-only input, jumping and swimming cannot start a dodge.
- **I** opens inventory; **G** toggles auto-move. Old default RMB/Q conflicts are migrated on first run; unrelated custom bindings are retained. Actions can be rebound in keyboard settings. Dedicated controller defaults are not added.

## Contact and balance

Melee contact and physical projectile collisions have no second random hit/miss roll. This applies symmetrically to player, NPCs and creatures. Agility, luck, skill and Sanctuary no longer create random physical evasion. Spell success/resistance, armor, difficulty, strength, weapon condition, critical damage, knockdown resistance and noncombat agility effects retain their ordinary rules.

Melee samples the attached weapon's dominant mesh axis and its movement between frames during the strike. Contacts are checked again at the animation hit key after advancing the animation. First-person coordinate mismatches and unsupported/unarmed models use a narrow view/head ray; a hidden first-person player can still be hit via native actor collision. Work is bounded to six rays per active blade sample; idle actors do not traverse weapon bounds. This geometry remains approximate and needs testing on ordinary/modded models, low creatures and third-person attacks.

Damage and wear retain their previous curves, with `u = clamp(skill / 100, 0, 1)`. The rebalance uses `s = clamp((skill - 1) / 99, 0, 1)` and `a = clamp((agility - 1) / 99, 0, 1)` so the requested skill 1/100 endpoints are exact.

| Factor | Formula | Skill 1 | Skill 50 | Skill 100 |
| --- | --- | --- | --- | --- |
| Physical damage | `0.30 + 0.70u` | 30.7% | 65% | 100% |
| Attack/block stamina, relative to native base | `0.875 + 2.8525(1-s)` | 372.75% | 231.57% | 87.5% |
| Weapon wear | `0.50 + 0.80(1-u)` | 129.2% | 90% | 50% |
| Preparation/recovery duration, agility 50 | `T = 3 - 2[s + 0.3(2a-1)s(1-s)]` | 3x | about 2.01x | 1x |

The stamina endpoints mean three times the old skill 1 multiplier (1.2425), tapering to the old skill 50 multiplier (0.875). Intermediate costs decrease smoothly. Weapon attacks use their weapon skill and shield actions use Block. Dodge has no independent native skill and uses agility as its progression stat.

Preparation includes bow draw and thrown preparation; recovery includes crossbow reload and follow-through. The actual release/strike retains original speed. Zero stamina halves physical damage and multiplies preparation/recovery durations by another 1.5. Fractional wear, armor processing at the original damage scale, partial blocks and OnStrike enchantments retain the previous implementation.

## Exhaustion and interruption

Reaching zero or negative fatigue starts a natural-regeneration lock of `10 - 7a` simulation seconds: exactly 10 at agility 1 and 3 at agility 100. Starting/releasing another attack, maintaining an exhausted wind-up or attempting a shield raise restarts that deadline. Holding an already collapsed shield does not automatically raise it after regeneration; release and press the button again. Natural regeneration resumes only after the quiet interval. Waiting/sleeping consumes the corresponding delay before restoring fatigue; explicit magical/potion restoration is retained. Regaining positive fatigue clears the depletion state. The delay is transient and reinitializes if a save is loaded at zero fatigue.

Every damaging hit previously requested hit recovery, allowing weak dagger attacks to interrupt a much stronger opponent continuously. A health hit now needs at least `max(2, 0.03 * maximum health + 0.025 * agility)` damage to request a flinch. A fatigue hit uses `max(3, 0.05 * maximum fatigue + 0.025 * agility)`. An accepted flinch grants 1 simulation second of protection against another flinch; small follow-up hits do not extend that timer. This applies symmetrically to NPCs, the player and creatures. Native strong-hit knockdown checks remain separate.

Ranged spread rotates velocity about world Z only, preserving vertical aim and speed. Maximum angle: `(1-u)(1.5-0.5 * clamp(agility / 100, 0, 1))` degrees. The difference of two uniform samples concentrates shots near the aim point, with either horizontal direction possible. Skill 100 has zero deviation. NPC aim includes a low ballistic arc and limits shooting distance to a usable gravitational range.

## Shield and dodge

Raising takes `(0.18 - 0.09 * clamp(agility / 100, 0, 1)) * T(Block, agility)` seconds. Re-raising has a `0.2 * T` recovery interval. The actual block animation advances to its raised key (or midpoint when no hit key exists), then stays frozen there until the guard ends. Guard release preserves unrelated shield equip/unequip animations. Once ready, a shield raised between the incoming attack's wind-up and impact blocks 100% physical damage. A shield raised before that attack, including one held through successive attacks, blocks 30%. It covers the frontal arc; side/rear attacks bypass it. Projectiles keep their own attack-start timestamp, so later shots cannot alter an earlier arrow's block window. A perfect projectile block rebounds into loose-item physics without opening a body puncture.

Using Block skill in the resource/wear formulas: raise cost is `3 × resource`, hold cost is `2 × resource` per simulation second, impact cost is `(3 + 0.15 × blocked damage) × resource`. Shield wear scales with blocked damage and Block skill. Zero stamina prevents raising and lowers a held shield; release RMB before raising again. NPCs respond to active enemy wind-ups with a skill/agility-dependent reaction delay and the same costs.

Dodge costs `11.5 + 30.35(1-a)` stamina: 41.85 at agility 1, 11.5 at agility 100. It travels 160 engine units over 0.24 simulation seconds and has a `1.5 - a` second cooldown from activation. It uses ordinary collision-aware movement, adds no vertical motion and grants no invulnerability. Walls can shorten it and badly timed dodges can still be hit. Preparation may be cancelled to defend; beginning a shield raise/dodge cannot skip strike/recovery.

## Retained features and saves

Arrows, bolts and thrown weapons use full gravity. Actor contacts attach uncollectible models to living bones, then corpse rigid bodies including severed parts. Organic punctures bleed at half the ordinary limb-wound rate for 5 seconds times the status-duration setting. Nonflesh exclusions remain. Hard impacts produce dissipative rebounds and use the shared item solver, count/duration settings. Remnants expire after 600 real seconds or changing the player's cell, with a 1024-remnant cap; they are session effects cleared on load.

The last nearby hostile death triggers two unpaused real seconds at one-third simulation speed using ordinary rest enemy rules. Physics, animations, particles, music and voices share the change. New saves preserve in-flight projectile damage and attack age; old projectile records load with fallback values. Active guard/dodge state resets on load/resurrection. Older executables may not understand new projectile save fields.

## Validation

Automated checks cover exact rebalance boundaries, exhausted-spam lockout and deadline resets, natural-rest recovery, weak-hit interruption resistance, the native raised shield track, timed/held guard, dodge distance across framerates, bounded spread, gravitational aim, actual OSG transforms/sweeps, the shipped Lua armor module, save compatibility, and retained blood/ragdoll/item regressions. Windows component/native tests and installed executable/ZIP checks run before release. Logs are in the package's validation directory.

These checks are not a Morrowind-data playtest. Shield pose quality, first/third-person contacts, ordinary creatures, controller bindings, actual difficulty, FPS and modded skeletons require in-game testing. Use copied saves and retain the stable installation.
