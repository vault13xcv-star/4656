Brutal Morrowind **1.2-projectiles.1 — experimental Windows x64**, based directly on stable **1.1**.

Download the Windows-x64 ZIP and extract it fully into a separate directory. Morrowind game data and your OpenMW configuration are required. The launcher/editor and game assets are not included. The stable Latest release remains 1.1.

- Gravitational flight for arrows, bolts and thrown weapons; hard-surface ricochets followed by rigid-body item physics.
- Embedded projectiles follow living pose bones and individual corpse parts. Impact remnants cannot be picked up and ammunition is no longer added to the victim's inventory.
- Organic punctures bleed at half the ordinary limb-wound emission rate for half the duration. Stable 1.1 blood/corpse textures are retained.
- A fresh last-nearby-hostile death queues two real unpaused seconds at one-third simulation speed, including physics, animations, particles, music and voices. Nearby enemies are checked with the same rest rules.

**Known issues and limits**

- NPC ranged targeting still uses the old target-leading calculation without compensating for the increased gravitational drop. NPC shots may undershoot at long range. This is not fixed in this executable.
- Impact remnants are session effects and are not serialized. Loading a save clears them. They otherwise disappear after 600 real seconds or changing the player's cell, including crossing an exterior cell boundary. There is a 1024-remnant safety cap.
- Contact shapes and aerodynamics are approximations. Unusual modded skeletons and displaced corpse parts still need in-game validation.
- No playtest with actual Morrowind data or measured gameplay FPS/audio validation has been performed. Automated checks are not a substitute for that test.

**Validation and sources**

Compiled engine/source commit: `e738572084976cc3f9b43639dfea27e6d7fe1088`.
Windows build: https://github.com/vault13xcv-star/4656/actions/runs/40564818890
Windows checks: 4/4 physics/render groups, 1402/1402 component tests and 42/42 native integration tests passed. Installed executable version, ZIP CRC/SHA256, original blood/gore assets and public downloads were checked.
The source ZIP corresponds to the compiled commit; subsequent branch changes to these notes do not rebuild or modify the executable.

Use copied saves for testing. Dependency notices are included in the runtime's licenses directory.
