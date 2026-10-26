# Brutal Morrowind 1.1 Experimental

**Experimental corpse and cut textures on top of V1.0, based on OpenMW 0.51.**

This separate branch adds uploaded burnt/poisoned body coatings, circular limb
cross-sections, waist tissue and left/right longitudinal anatomy. The stable
V1.0 release remains unchanged. See [1.1 materials and limitations](brutal-morrowind.md).

[Download 1.1 Experimental](https://github.com/vault13xcv-star/4656/releases/tag/brutal-1.1-experimental.1)

The links below document the stable V1.0 baseline.

[Download Windows x64](https://github.com/vault13xcv-star/4656/releases/download/v1.0/Brutal-Morrowind-1.0-Windows-x64.zip)
· [Release page](https://github.com/vault13xcv-star/4656/releases/tag/v1.0)
· [Full source ZIP](https://github.com/vault13xcv-star/4656/releases/download/v1.0/Brutal-Morrowind-1.0-Source.zip)
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

V1.0 promotes the tested `0.7.0-preview.14` executable and gameplay resources
unchanged. The established combat system is retained.

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

The underlying Windows build passed 1402 component tests, 39 native item
integration tests and the physics/render regression groups. Executable startup,
ZIP integrity and resource hashes were checked. These are automated checks,
not a guarantee of in-game performance or compatibility with every mod.

Interior blood follows visible geometry, including carpets, furniture, steps and
columns. Exterior blood retains collision-backed landscape/object receivers.
Deposited stains do not follow subsequently moved doors/items. Unusual skeletons
and whole-body NPC meshes retain the established dismemberment limitations.

## Source and support

- [V1.0 source tree](https://github.com/vault13xcv-star/4656/tree/v1.0),
  [source ZIP](https://github.com/vault13xcv-star/4656/releases/download/v1.0/Brutal-Morrowind-1.0-Source.zip),
  [checksums](https://github.com/vault13xcv-star/4656/releases/download/v1.0/SHA256SUMS).
- [Build instructions](../CI/ragdoll-build.md), [blood implementation](brutal-morrowind.md),
  [dismemberment rules](dismemberment.md).
- [Report an issue](https://github.com/vault13xcv-star/4656/issues) with the release,
  location, Physics settings, mods and reproduction steps.

Based on [OpenMW](https://openmw.org/), whose engine code is licensed under
[GNU GPLv3](../LICENSE). Dependency notices are included in the runtime. Blood
artwork was supplied by the mod author. Morrowind belongs to Bethesda Softworks.
This community project is not an official OpenMW or Bethesda release.
