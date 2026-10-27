Brutal Morrowind 1.2-procedural.1 for OpenMW 0.51.0 / Windows x64.

Download the **Windows-x64 ZIP**, extract it fully, and read BRUTAL-MORROWIND-README.md.
This build reuses your OpenMW user configuration; copyrighted game data is not included.
The launcher/editor are not bundled. Keep your stable installation and separate test saves.

Source commit: b2c05852dd1d4c2f9975e71857406b11dcd0c66e
Compiler: MSVC 2022 x64; dependency archive verified against the checked-in SHA512 manifest.
This experimental build uses procedural blood sprites and stains, with gravity-driven
surface flow and growing pools. Visible receiver meshes supplement collision-backed
outdoor terrain. Original corpse/cut materials remain included. Surface behavior,
performance and compatibility must still be checked in the actual game.
Automated checks: compiled runtime, component tests, ragdoll and item physics, synthetic skeleton and lethal-hit attribution tests, installed --version smoke checks, ZIP integrity.
These checks do not replace testing animations and gameplay inside Morrowind.

Corresponding engine sources are attached. Dependency notices are in the runtime's licenses directory.
