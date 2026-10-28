# Brutal Morrowind 1.3-combat.1 — experimental active combat

This branch extends the physical-projectile build descended from stable 1.1. Stable blood DDS, corpse coatings, dismemberment rules and blood rendering are retained. Extract the full Windows ZIP separately and use copied saves. It reuses your OpenMW configuration and requires Morrowind data. Launcher/editor are not bundled.

## Controls

- Hold **right mouse button** to raise/hold a carried shield with a compatible one-handed weapon or unarmed stance.
- Press **Q** while moving sideways/backwards to dodge. Standing still dodges backwards; forward-only input, jumping and swimming cannot start a dodge.
- **I** opens inventory; **G** toggles auto-move. Old default RMB/Q conflicts are migrated on first run; unrelated custom bindings are retained. Actions can be rebound in keyboard settings. Dedicated controller defaults are not added.

## Contact and balance

Melee contact and physical projectile collisions have no second random hit/miss roll. This applies symmetrically to player, NPCs and creatures. Agility, luck, skill and Sanctuary no longer create random physical evasion. Spell success/resistance, armor, difficulty, strength, weapon condition, critical damage, knockdown resistance and noncombat agility effects retain their ordinary rules.

Melee samples the attached weapon's dominant mesh axis and its movement between frames during the strike. Contacts are checked again at the animation hit key after advancing the animation. First-person coordinate mismatches and unsupported/unarmed models use a narrow view/head ray; a hidden first-person player can still be hit via native actor collision. Work is bounded to six rays per active blade sample; idle actors do not traverse weapon bounds. This geometry remains approximate and needs testing on ordinary/modded models, low creatures and third-person attacks.

Let `s = clamp(skill / 100, 0, 1)` and `a = clamp(agility / 100, 0, 1)`. Weapons use their native skill; creatures use the engine's combat-skill mapping.

| Factor | Formula | Skill 0 | Skill 30 | Skill 100 |
| --- | --- | --- | --- | --- |
| Physical damage | `0.30 + 0.70s` | 30% | 51% | 100% |
| Attack stamina | `1.25 - 0.75s` | 125% | 102.5% | 50% |
| Weapon wear | `0.50 + 0.80(1-s)` | 130% | 106% | 50% |
| Wind-up duration, agility 40 | `1 + (1-s)(0.20 + 0.15(1-a))` | 129% | 120.3% | 100% |
| Recovery duration, agility 40 | `1 + (1-s)(0.35 + 0.15(1-a))` | 144% | 130.8% | 100% |

Wind-up includes bow draw and thrown preparation; recovery includes crossbow reload and follow-through. The actual release/strike retains original weapon animation speed. At zero stamina, damage receives a further ×0.5 and wind-up/recovery durations a further ×1.5. Fractional wear accumulates so integer item condition preserves mastery savings. Armor is evaluated at the original damage scale before mastery/exhaustion/partial-guard factors, avoiding a second low-skill penalty. A perfect block stays at zero despite armor's minimum damage. Fully blocked contacts do not apply OnStrike enchantments; partial blocks retain ordinary enchantments.

Calibration uses a skill30/agility40/luck40 full-fatigue attacker against an agility20/luck20 opponent: original expected hit chance approximately 45%. For a representative cycle of 50% preparation, 20% strike and 30% recovery, new expected damage rate differs by about 5%, stamina per fight by about 10%, and wear by about 6%. This is a formula calibration, not a measured mudcrab fight. Exact effort cannot be preserved for every defender after removing defender-dependent evasion. Low/high-level balance requires gameplay feedback.

Ranged spread rotates velocity about world Z only, preserving vertical aim and speed. Maximum angle: `(1-s)(1.5-0.5a)` degrees. The difference of two uniform samples concentrates shots near the aim point, with either horizontal direction possible. Skill100 has zero deviation. NPC aim includes a low ballistic arc and limits shooting distance to a usable gravitational range.

## Shield and dodge

Raising takes `0.18 - 0.09a` seconds. Once ready, a shield raised between the incoming attack's wind-up and impact blocks 100% physical damage. A shield raised before that attack, including one held through successive attacks, blocks 30%. It covers the frontal arc; side/rear attacks bypass it. Projectiles keep their own attack-start timestamp, so later shots cannot alter an earlier arrow's block window. A perfect projectile block rebounds into loose-item physics without opening a body puncture.

Using Block skill in the resource/wear formulas: raise cost is `3 × resource`, hold cost is `2 × resource` per simulation second, impact cost is `(3 + 0.15 × blocked damage) × resource`. Shield wear scales with blocked damage and Block skill. Zero stamina prevents raising and lowers a held shield; release RMB before raising again. NPCs respond to active enemy wind-ups with a skill/agility-dependent reaction delay and the same costs.

Dodge costs `14 - 5a` stamina, travels 160 engine units over 0.24 simulation seconds and has a `0.90 - 0.40a` second cooldown from activation. It uses ordinary collision-aware movement, adds no vertical motion and grants no invulnerability. Walls can shorten it and badly timed dodges can still be hit. Preparation may be cancelled to defend; beginning a shield raise/dodge cannot skip strike/recovery.

## Retained features and saves

Arrows, bolts and thrown weapons use full gravity. Actor contacts attach uncollectible models to living bones, then corpse rigid bodies including severed parts. Organic punctures bleed at half the ordinary limb-wound rate for 5 seconds times the status-duration setting. Nonflesh exclusions remain. Hard impacts produce dissipative rebounds and use the shared item solver, count/duration settings. Remnants expire after 600 real seconds or changing the player's cell, with a 1024-remnant cap; they are session effects cleared on load.

The last nearby hostile death triggers two unpaused real seconds at one-third simulation speed using ordinary rest enemy rules. Physics, animations, particles, music and voices share the change. New saves preserve in-flight projectile damage and attack age; old projectile records load with fallback values. Active guard/dodge state resets on load/resurrection. Older executables may not understand new projectile save fields.

## Validation

Automated checks cover balance boundaries, expected-effort calibration, exhaustion, timed/held guard, dodge distance across framerates, bounded spread, gravitational aim, actual OSG transforms/sweeps, the shipped Lua armor module, save compatibility, and retained blood/ragdoll/item regressions. Windows component/native tests and installed executable/ZIP checks run before release. Logs are in the package's validation directory.

These checks are not a Morrowind-data playtest. Shield pose quality, first/third-person contacts, ordinary creatures, controller bindings, actual difficulty, FPS and modded skeletons require in-game testing. Use copied saves and retain the stable installation.
