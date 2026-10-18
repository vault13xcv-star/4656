Experimental OpenMW 0.51.0 runtime for Windows x64 with humanoid ragdolls 0.4.0.

Download the **Windows-x64 ZIP**, extract it fully, and read RAGDOLL-README.md.
This build reuses your OpenMW user configuration; copyrighted game data is not included.
The launcher/editor are not bundled. Keep your stable installation and separate test saves.

Source commit: 447e31f821bf7ebbfab5304985d0cf5d1087d6bc
Compiler: MSVC 2022 x64; dependency archive verified against the checked-in SHA512 manifest.
Version 0.4 fixes corpse fire/smoke with reversed depth, frozen-body slope motion, explosion wave geometry and visible whole-body electrical spasms. Pickable world items now have mass, rigid-body collisions and weapon/spell impulses.
Automated checks: compiled runtime, component tests, ragdoll and item physics, synthetic skeleton and lethal-hit attribution tests, installed --version smoke checks, ZIP integrity.
These checks do not replace testing animations and gameplay inside Morrowind.

Corresponding engine sources are attached. Dependency notices are in the runtime's licenses directory.
