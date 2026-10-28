Brutal Morrowind **1.3-combat.1 — experimental Windows x64**.

Active combat on top of the physical-projectile branch descended from stable 1.1. Stable Latest remains 1.1.

- Weapon/projectile contacts count without a random hit roll, symmetrically for all actors.
- Weapon skill controls physical damage, preparation/recovery, wear and stamina; actual strikes retain original speed. Agility improves preparation, ranged accuracy and active defenses. Exhaustion halves physical damage and increases preparation/recovery durations by 50%.
- Hold RMB for a shield: a timely raise blocks 100%, early/held guard blocks 30%. Raising, holding and impacts cost stamina; Block skill improves costs/wear. Zero stamina lowers the shield.
- Q dodges backwards/sideways through ordinary movement/collision, with stamina and agility-dependent cooldown. Inventory moves to I; auto-move to G. Actions are rebindable.
- Mild horizontal ranged spread vanishes at skill100. NPC aim compensates for gravity. Perfect shield blocks bounce projectiles without a blood wound.
- Stable blood, dismemberment, corpse textures, physical projectiles and last-hostile slow motion remain.

Contact geometry uses bounded blade-axis sweeps and native ray/capsule fallbacks and remains approximate. Controller defaults for the new actions are not supplied. Projectile remnants are session effects and disappear on load, cell changes or after ten real minutes. New in-flight projectile save fields may not load in older builds.

Automated checks exercise actual collision geometry, the shipped Lua armor module, save compatibility, balance and active-defense rules, alongside existing physics/render/component/native tests. They do not replace a playtest. Gameplay FPS and Morrowind fight balance have not been measured.

Fully extract the Windows-x64 ZIP separately, read BRUTAL-MORROWIND-README.md and use copied saves. Game data and launcher/editor are not bundled. Corresponding sources and dependency notices accompany the release.
