# Brutal Morrowind 1.1

**The current stable Windows x64 release, based on OpenMW 0.51.**

[Download Windows x64](https://github.com/vault13xcv-star/4656/releases/download/v1.1/Brutal-Morrowind-1.1-Windows-x64.zip)
· [Release page](https://github.com/vault13xcv-star/4656/releases/latest)
· [Source tree](https://github.com/vault13xcv-star/4656/tree/v1.1)
· [Full source ZIP](https://github.com/vault13xcv-star/4656/releases/download/v1.1/Brutal-Morrowind-1.1-Source.zip)
· [Русский](https://github.com/vault13xcv-star/4656)

## Features

- Ragdoll corpses, detached body parts and physics-enabled loose items.
  Melee, projectiles and magic impart impulses; crossbow bolts can pin bodies.
- Dismemberment considers hit location, attack direction, weapon class and
  elemental effects. Supported effects include limb/head removal, waist and
  longitudinal cuts, head bursts and body tearing. Equipment remains lootable.
- Blood fountains and airborne sprite droplets, with three floor and three wall
  textures. Blood responds to world lighting. Organic creatures bleed;
  mechanisms, ghosts and skeletons do not.
- Shared projection budgets, background receiver preparation and redundant-layer
  filtering limit blood-related work. Droplets keep flying while receivers build.
- `Esc → Settings → Physics` controls impulses, active-body/item budgets and
  lifetimes, wound probability, blood amount and stain lifetime. Defaults include
  a 30% wound chance and ten-minute stain persistence.

Version 1.1 adds burnt/poisoned corpse coatings with enlarged seamless patterns,
art-fitted limb/waist caps and left/right longitudinal anatomy. Fire/frost
severing is bloodless. Successful two-handed hammer torso dismemberment selects
3–5 random extremities and keeps the waist intact. The established combat system
is retained; all loot stays with the original body.

## Installation

Requires **Windows x64** and your own installed copy of **Morrowind**. Game data,
the launcher and the editor are not bundled.

1. If needed, install [official OpenMW](https://openmw.org/downloads/), use its
   wizard to locate Morrowind and verify a normal game launch.
2. Download the Windows ZIP and extract **the entire archive into a new folder**.
3. Start that folder's `openmw.exe`. It reuses your existing OpenMW configuration.
   `OpenMW-Russian.cmd` is also included for Russian game data.
4. Adjust `Esc → Settings → Physics`. Use a copy of your save for the first run.

When updating, keep the EXE, libraries and resources from the same package.

## Verification and compatibility

The underlying Windows build passed 1402 component tests, four physics/render
regression groups and 38/39 native integration tests. One old integration
expectation still required the intentionally removed hammer waist cut. Its
source is corrected; the compiled engine is retained without recompilation and
the original test result is disclosed in [KNOWN-ISSUES](releases/v1.1-known-issues.md).
Executable startup, ZIP integrity and resource hashes were checked. These
checks do not replace an in-game test with your Morrowind data.

Interior blood follows visible geometry, including carpets, furniture, steps and
columns. Exterior blood retains collision-backed landscape/object receivers.
Deposited stains do not follow subsequently moved doors/items. Unusual skeletons
and whole-body NPC meshes retain the established dismemberment limitations.

## Source and support

- [V1.1 source tree](https://github.com/vault13xcv-star/4656/tree/v1.1),
  [source ZIP](https://github.com/vault13xcv-star/4656/releases/download/v1.1/Brutal-Morrowind-1.1-Source.zip),
  [checksums](https://github.com/vault13xcv-star/4656/releases/download/v1.1/SHA256SUMS).
- [Build instructions](../CI/ragdoll-build.md), [blood implementation](brutal-morrowind.md),
  [dismemberment rules](dismemberment.md).
- [Report an issue](https://github.com/vault13xcv-star/4656/issues) with the release,
  location, Physics settings, mods and reproduction steps.

Based on [OpenMW](https://openmw.org/), whose engine code is licensed under
[GNU GPLv3](../LICENSE). Dependency notices are included in the runtime. Blood
artwork was supplied by the mod author. Morrowind belongs to Bethesda Softworks.
This community project is not an official OpenMW or Bethesda release.
