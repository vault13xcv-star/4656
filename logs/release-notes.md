Brutal Morrowind 1.1-experimental.1 for OpenMW 0.51.0 / Windows x64.

Download the **Windows-x64 ZIP**, extract it fully, and read BRUTAL-MORROWIND-README.md.
This build reuses your OpenMW user configuration; copyrighted game data is not included.
The launcher/editor are not bundled. Keep your stable installation and separate test saves.

Source commit: 7ff827e3f025708f791ea8d535939b083dd633d3
Compiler: MSVC 2022 x64; dependency archive verified against the checked-in SHA512 manifest.
Experimental 1.1 adds the five supplied corpse and cut DDS textures: burnt, poisoned, cut_limb, cut_body_torso and sliced_body. Periodic stochastic coatings avoid regular tiled patterns. Elliptical limb/waist meshes and anatomy-aligned left/right longitudinal UVs avoid opaque image backdrops. Wound materials receive actual world ambient/sun/point lighting and shadows. V1.0 remains the stable release.
Automated checks: compiled runtime, component tests, ragdoll and item physics, synthetic skeleton and lethal-hit attribution tests, installed --version smoke checks, ZIP integrity.
These checks do not replace testing animations and gameplay inside Morrowind.

Corresponding engine sources are attached. Dependency notices are in the runtime's licenses directory.
