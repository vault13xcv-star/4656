# G.O.R.E: Morrowind 1.0

**Guts Overhaul & Ragdoll Extension** — a modified OpenMW 0.51 runtime for Windows x64, previously called Brutal Morrowind.

[Download Windows x64](https://github.com/vault13xcv-star/4656/releases/download/gore-v1.0/GORE-Morrowind-1.0-Windows-x64.zip) · [Release and checksums](https://github.com/vault13xcv-star/4656/releases/tag/gore-v1.0) · [Source ZIP](https://github.com/vault13xcv-star/4656/releases/download/gore-v1.0/GORE-Morrowind-1.0-Source.zip)

## Included features

- Ragdoll corpses and severed parts, physical loose items, gravity-driven arrows/bolts/thrown weapons, hard-surface rebounds and living/dead body attachments. Spent projectiles cannot be picked up. Organic punctures bleed.
- Weapon/direction/body-part-aware NPC dismemberment, waist and longitudinal cuts, head destruction, physical intestines, blood sprays, stains and pools, elemental corpse appearances. Equipment remains in the corpse inventory. Mechanical creatures, ghosts and skeletons do not bleed.
- Contact-based physical hits without a second hit roll for players, NPCs and creatures. Weapon skills affect damage, preparation/recovery, wear and stamina. Agility affects tempo and ranged spread.
- Active RMB shields: timely 100% physical block, early/held 30% block; fatigue costs for raising, holding and impacts. RMB without a shield dodges backwards/sideways, with ordinary collisions and no invulnerability.
- Free ordinary land walking/running, stamina-driven Shift sprint, half-speed backwards running and no backwards sprint, including diagonals. Backwards restrictions apply to NPCs too.
- Two quiet combat seconds before positive stamina naturally regenerates; the stricter 3–10 second Agility-dependent exhaustion delay remains. Outside combat a full pool recovers in 5–10 seconds by Endurance after any exhaustion delay. Weak hits no longer cause perpetual flinches. Nonhostile NPCs intercept friendly physical attacks without damage; spell rules are unchanged.
- Optional last-nearby-hostile slow motion: two real seconds at one-third time and audio speed.

## Install and launch

1. Install Morrowind. If OpenMW is not configured, install [official OpenMW](https://openmw.org/downloads/), complete its setup wizard pointing at Morrowind's `Data Files`, verify an ordinary game launch and close it.
2. Download the **Windows x64** ZIP. The Source ZIP is for development.
3. Extract the **whole archive** into a separate directory. Keep `openmw.exe`, DLLs, `openmw.cfg`, `defaults.bin` and `resources` together.
4. Run **GORE-Morrowind.cmd**. G.O.R.E uses your existing OpenMW configuration, data paths and content list. No separate ESP is required. Morrowind assets, launcher and editor are not bundled.
5. Back up saves before continuing a playthrough. Configure Physics options in game.

For Russian game data use the included `OpenMW-Russian.cmd`; Cyrillic font notes are in `CYRILLIC-README.md`. Missing content: correct the `Data Files` path and enable `Morrowind.esm` in the official launcher before relaunching G.O.R.E. Tribunal/Bloodmoon are optional if installed.

## Controls and settings

LMB attacks/shoots/casts. RMB holds a shield, or dodges without a shield; standing still dodges backwards. Shift sprints, Caps Lock toggles run/walk, F readies weapons, R readies magic, E jumps, Space activates, I opens inventory and G toggles auto-move. Keys can be rebound. Sprint/dodge require stamina; a shield prevents dodging.

**Esc → Settings → Physics → Last enemy slow motion** toggles the effect. Enabled by default. Disabling cancels active and pending slow motion, restoring time and audio without restarting. Reset enables it again.

The same tab configures impulses, active-body/item budgets and lifetimes, bolt pinning, effect duration, dismemberment chance, blood quantity and stain lifetime. Default dismemberment chance is 30%; stain lifetime is ten minutes.

## Limits and source

Contacts remain approximate. Animation/skeleton/combat/input mods require compatibility checks; unsupported skeletons can fall back to native death animations. Projectile remnants are session effects cleared on load/cell changes or after ten real minutes. Keep save backups; older executables may not understand newer projectile fields. Automated tests and packaged logs do not replace a Morrowind-data playtest or establish gameplay FPS.

[Technical reference](gore-morrowind.md) · [Dismemberment rules](dismemberment.md) · [Build recipe](../CI/ragdoll-build.md) · [OpenMW GPLv3 license](../LICENSE)

Validation exception: the completed Windows executable is retained unchanged after one obsolete menu-test failure (1404/1405 component tests passed). The corrected 17 settings tests passed separately on Linux; Windows native tests were not run after that failed step. [Detailed report](gore-validation.md).
