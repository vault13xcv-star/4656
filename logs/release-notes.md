Brutal Morrowind 1.2-projectiles.1 for OpenMW 0.51.0 / Windows x64.

Download the **Windows-x64 ZIP**, extract it fully, and read BRUTAL-MORROWIND-README.md.
This build reuses your OpenMW user configuration; copyrighted game data is not included.
The launcher/editor are not bundled. Keep your stable installation and separate test saves.

Source commit: e738572084976cc3f9b43639dfea27e6d7fe1088
Compiler: MSVC 2022 x64; dependency archive verified against the checked-in SHA512 manifest.
Experimental branch based directly on stable 1.1; the procedural-blood experiments are not included.
Arrows, bolts and thrown weapons use gravitational flight, embed in actor bodies, or bounce into the existing loose-item solver. Impact remnants cannot be collected or recovered from corpses. Puncture wounds bleed at half the ordinary limb-wound rate for half its duration. Remnants are cleared on leaving the player cell or after ten real minutes (with a 1024-remnant safety cap); they are session effects and are not saved.
Killing the last nearby hostile triggers two real seconds at one-third simulation speed. Eligibility uses the same nearby-enemy rules as rest, including followers and water-only creatures. Animations, physics, particles and all currently playing sounds/music share the time change.
Native collision uses swept projectile shapes and bounded rigid-body item physics, not a full aerodynamic model. In-game visual quality, modded skeletons, FPS and audio must still be checked with Morrowind data.
Automated checks: compiled runtime, component tests, ragdoll and item physics, synthetic skeleton and lethal-hit attribution tests, installed --version smoke checks, ZIP integrity.
These checks do not replace testing animations and gameplay inside Morrowind.

Corresponding engine sources are attached. Dependency notices are in the runtime's licenses directory.
