# Brutal Morrowind 0.7.0-preview.11

This Windows x64 runtime is built directly from the stable **0.7.0-preview.2**
source, commit `a76508441a8dd4a791a38cab140f04bf7c2a2323`. The later experimental
combat changes are excluded. Melee, projectile and enchantment damage, wound
selection, physical bodies, intestines, inventory and the one-second immediate
magic-death window retain the stable implementation.

Extract the entire Windows ZIP into a new directory and run `openmw.exe`.
Your Morrowind data and normal OpenMW configuration are required. No launcher,
editor or game data are included. Use separate save copies: this build reads
preview.2 blood records, but its new blood records store texture variation and
are not readable by preview.2. Existing wounds are not rerolled on load.

## Wall and floor splatters

The six original DXT5 files from the supplied `splatterss_dds.zip` are bundled,
with their original filenames and contents. `wall (1).dds` through `wall (4).dds`
are wall variants; `floor (6).dds` and `floor (7).dds` are floor variants. Each
impact chooses a variant and a slightly varied size. Floor artwork can rotate
180 degrees; wall artwork stays upright. Both can mirror horizontally.
Transparent areas retain the DDS alpha channel. The small shared atlas is built
once, with transparent gutters; there is no texture or scene object per stain.

**Pools are not DDS decals.** Their untextured irregular geometry, merge rule,
formation timer, collision probe and radius formula come from preview.2. Changing
splat artwork does not change the pool material. The blood amount control keeps
the stable pool scaling independently of the increased spray emission.

Inside interiors, flying droplets hit the actual rendered triangles of nearby
world meshes. Splatter geometry is clipped to those surfaces rather than laid
on a simplified physics box: stair treads, rounded columns, furniture and
collision-free carpets can receive blood. Actors and transient effects are
excluded. Exterior blood keeps collision-backed receivers from the stable
implementation, including the landscape and collidable walls, doors, barrels
and steps. The interior visual-mesh method is never substituted for exterior
terrain. Pools keep their stable physics receivers in both environments.

Stains remain fixed to their location after placement; they do not follow a door
or item when it moves. Mesh snapshots are bounded, reused and invalidated when
world objects change. Existing stain geometry is not reprojected during load.

## Spray, creatures and lighting

Flying drops and fountains use camera-facing sprite cards, four vertices per
drop. They follow gravity and swept collision rays, depositing a persistent
splatter at impact. They are not individual rigid bodies or 3D droplet volumes.
Neck fountains and major horizontal/longitudinal cuts emit stronger, more visible
jets. Duplicate legacy wound particle systems are suppressed when the new sprite
trail is available.

Default spray emission is twice preview.2. Raising Blood amount increases both
concentration and launch speed/cone spread, so more blood reaches farther from
the wound. Organic creatures bleed on physical health hits and emit a fountain
on a new death, even without a supported ragdoll skeleton. Canonical record/model
identifiers exclude mechanisms, skeletons, ghosts and other nonflesh families;
flesh-bearing undead remain eligible. Loading an already dead creature does not
restart its fountain. Custom creature models may need additional classification.

Blood uses the scene's ambient, sunlight and nearby point lights, with fog and
native sun shadows. It darkens in a dark room instead of emitting its own light.
The same lighting applies to flying sprites. Wet/dry aging remains supported.

Esc > Settings > Physics:

| Control | Minimum | Default | Maximum |
| --- | ---: | ---: | ---: |
| Blood amount | 0% (off) | 100% (twice the old spray baseline) | 300% |
| Blood stain lifetime | 300 s | 600 s (10 minutes) | 1800 s |

Stain lifetime is independent of bleed duration and corpse physics. Ages pause
with the game, persist across cells and saves, and are preserved when changing
lifetime. Interiors at the same coordinates do not share stains. Expired stains
fade out during their last ten seconds.

The global stain count rises from 8192 to 32768 to retain older marks during
battles. Total geometry remains capped at the old worst-case allocation,
8192 x 192 vertices. The oldest stains are evicted when either limit is reached.
Persistent cell batches hold at most 128 stains each and only changed batches
upload geometry. Each trail has at most 192 live sprites, 32 wound sources and
eight projected impact splatters per update. Creature/hit bursts and receiver
snapshots have separate bounded caches. These bounds limit accumulation; they
do not guarantee a particular frame rate.

## Validation and practical limits

Automated tests cover the Bullet solver, existing physical wounds, saved poses,
cell visibility, stain lifetime/capacity, visible stair/column/carpet receivers,
actor exclusion, separate stable pool probes, invalidated receivers, sprite
geometry, doubled emission and organic creature death lifecycle. The real
ImageManager loads all six uploaded DXT5 textures in EGL/OpenGL tests. Native
rendering tests exercise transparency, darkness, sunlight, torches, shadows,
shader reload, normal/reversed depth, unchanged pool output and exterior
collision-backed spray impacts at 100% and 300%.

Windows CI builds the complete runtime, runs physics/render, component and item
integration tests, checks the installed executable with `--version`, verifies
the SHA256 of every installed DDS and checks the ZIP. These automated scenes do
not replace an in-game Balmora playtest with Morrowind assets. Noncolliding
exterior decorations follow the limitations of the stable collision receiver.
Custom whole-body NPC meshes and unusual creature skeletons retain preview.2's
compatibility limits. No new humanoid cuts are applied to creature skeletons.
