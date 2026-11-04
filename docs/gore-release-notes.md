# G.O.R.E: Morrowind 1.0

**Guts Overhaul & Ragdoll Extension** — the main release of the project formerly called Brutal Morrowind, based on OpenMW 0.51.

## Download and start

Download **GORE-Morrowind-1.0-Windows-x64.zip**, extract the entire archive into a separate folder and run **GORE-Morrowind.cmd**. Your configured OpenMW data paths and content list are reused. Morrowind game data is required and is not included; new users should first complete official OpenMW setup. For Russian game data use OpenMW-Russian.cmd. Back up saves before upgrading.

[Installation, controls and features](https://github.com/vault13xcv-star/4656#readme) · [English guide](https://github.com/vault13xcv-star/4656/blob/gore-v1.0/docs/GORE-Morrowind.en.md)

## Features

Ragdoll corpses and severed parts; physical loose items; blood sprays, stains and pools; directional dismemberment and physical intestines; elemental corpse appearances; physical arrows, bolts and thrown weapons; contact-based combat, skill-based damage/tempo/wear/stamina; active shields and dodge; sprint and stamina breaks; protection against accidental NPC physical friendly fire.

The combat and gore rules from the tested 1.3-combat.4 build are retained. This release adds a persistent **Settings → Physics → Last enemy slow motion** on/off control. It is enabled by default. Turning it off cancels pending and active slow motion and restores ordinary audio scaling without restarting. Reset restores the enabled default.

Source code is provided in **GORE-Morrowind-1.0-Source.zip** and the gore-v1.0 tag. **SHA256SUMS** covers both archives; **package.json**, BUILD-INFO.txt and VALIDATION.txt identify the exact engine build. Launcher/editor and game assets are not bundled.

## Validation exception — retained executable

The Windows engine compiled and its installed EXE and launchers passed startup/version checks. All 5 physics/render/combat groups passed. 1404 of 1405 component tests passed; the old Physics menu test incorrectly treated the new boolean checkbox as a numeric slider. The source fixes that test; all 17 settings tests passed separately on Linux. The Windows native integration test executable compiled but was not run because the component test stopped that step.

The completed engine is retained without another full compilation. Its original failed-step report and logs remain in the archive; **KNOWN-ISSUES.md** and **RELEASE-INFO.json** explain the exception and identify the compiled and publication commits. No all-tests-pass or Morrowind-data gameplay claim is made. See [the detailed report](https://github.com/vault13xcv-star/4656/blob/gore-v1.0/docs/gore-validation.md).
