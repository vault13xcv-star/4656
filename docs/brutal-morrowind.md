# Brutal Morrowind 0.7.0-preview.13

This Windows x64 runtime continues preview.12, with combat, damage, hit attribution,
physical dismemberment, inventories and the one-second magic-death window retained
from stable preview.2 (`a76508441a8dd4a791a38cab140f04bf7c2a2323`). The changes are
to blood presentation, receiver preparation and frame-time control.

Extract the entire ZIP into a new directory and run `openmw.exe`. Morrowind data
and your normal OpenMW configuration are required. The launcher/editor and game
data are not included. Keep separate save copies. This build reads preview.2
and preview.11/12 blood records; new blood records remain version 2 and cannot
be read by preview.2. Saved wounds and inventories are not rerolled.

## Floor textures and removal of procedural pools

The three original DXT5 files from `blood_pools_1024_dds.zip` replace all three
floor textures, byte for byte. They use the existing ASCII runtime filenames:

| Runtime asset | Original supplied file |
| --- | --- |
| `floor (1).dds` | `Матовая алая лужа на прозрачном фоне.dds` |
| `floor (2).dds` | `Матовая багровая лужа с каплями.dds` |
| `floor (3).dds` | `Тёмно-красная лужа крови на прозрачном фоне-2.dds` |

The three wall textures from preview.12 are unchanged: `wall (1).dds`,
`wall (2).dds` and `walll.dds`. Floor variants may rotate 180 degrees. Wall
variants remain upright. Both may mirror horizontally and vary slightly in size.
Transparent silhouettes retain the supplied alpha channel. All six textures are
packed into one shared atlas at startup; there is no texture object per stain.

The old untextured irregular-fan pools are removed. Wounds no longer probe the
floor periodically or generate those meshes. Old procedural pools are also
skipped while loading a save and are absent from subsequent saves. Existing
textured splatters load using the new artwork and retain their remaining ages.
Floor blood now consists entirely of textured decals, independently of the
physical corpse. No replacement growing procedural pool is generated.

## Blood emission frame-time fix

Inside interiors, each flying droplet sweeps against visible scene triangles.
Projecting an impact onto detailed furniture/carpet meshes then clips triangles
and performs visibility rays. Preview.12 allowed up to eight such projections
per trail per update. Several simultaneous fountains could therefore execute
dozens of expensive projections in one frame, particularly on layered interior
geometry. This work stopped after bleeding ended and disappeared at 0% blood.

Preview.13 keeps the sprite count, emission rates and ballistic trajectories.
Impact positions enter a bounded queue owned by their trail. Nearby pending
contacts are combined before projection. Each trail retains at most 32 pending
impacts for at most half a second. All corpse and hit/death trails share a
round-robin scheduler with at most four projection jobs per rendered frame and
a soft 1.5 ms projection-work budget. A running job finishes; this is not a hard
1.5 ms frame-time guarantee. A trail performs at most two jobs in its update.
Excess redundant or old requests are discarded instead of forming an unbounded
backlog. Fresh accepted impacts continue to leave persistent decals.

At 0% blood, active droplets and pending impact work are cleared immediately.
Completed short-lived bursts are removed without continuing receiver queries.
The setting does not erase previously deposited textured blood.

Interior triangles are captured on the update thread; the immutable Bullet BVH
is built on OpenMW's background work queue. No worker accesses live scene nodes
or game objects, and the renderer polls completion without waiting. Invalidated
jobs are cancelled and cannot publish stale geometry. One new receiver capture
is allowed per frame, with a bounded shared cache. The separate physics-mesh
snapshot/BVH previously required for pool probes is no longer created indoors.
Freezing an interior corpse likewise does not construct that unused pool mesh.

In a synthetic five-trail test with 28,800 receiver triangles, preview.12's
interior live-blood update peaked around 28–40 ms. The bounded path measured
around 4–6.5 ms in that same fixture, with no legacy pools. These are CPU-only
test measurements, not in-game Balmora FPS or a guarantee for a Windows PC.
Capturing scene triangles and creating/slicing a ragdoll still have an initial
CPU cost; the expensive receiver-tree construction no longer blocks the frame.

## Surfaces, lighting and retained controls

Interiors receive blood on visible world meshes, including noncolliding carpets,
furniture, steps and curved columns. Actors and transient effects are excluded.
Exteriors retain collision-backed receivers, including landscape and collidable
walls, doors, barrels and steps. Existing stains stay fixed to their location;
they do not follow a door or item after it moves. Receiver caches invalidate on
object and cell changes. The interior visual-mesh method is not substituted for
exterior landscape collision.

Preview.12's alpha-aware density control remains: a whole new splatter is omitted
when at least 90% of its sampled ink is already covered by two visible layers.
It does not carve square holes in artwork. Different surface heights and cells
remain separate. Dense older saves are filtered on load without renewing ages.
The global limit remains 32768 stains and 8192 x 192 vertices, with persistent
chunks of at most 128 stains. The oldest marks are evicted at the global limits.

Blood receives ambient, sunlight and nearby point lights, fog and sun shadows.
The darker direct-sun response and reduced white highlight from preview.12 are
retained. Flying drops are camera-facing sprite cards, not 3D droplet volumes or
individual rigid bodies. Neck and major cuts retain their stronger jets.
Organic creatures bleed and emit fountains on fresh deaths; mechanisms, skeletons,
ghosts and other nonflesh families remain excluded. Loaded corpses do not restart
fountains. Custom creature models may require additional classification.

Esc > Settings > Physics:

| Control | Minimum | Default | Maximum |
| --- | ---: | ---: | ---: |
| Blood amount | 0% (off) | 100% (twice preview.2's original spray baseline) | 300% |
| Blood stain lifetime | 300 s | 600 s (10 minutes) | 1800 s |

Amount increases emission, launch speed and spread, subject to the live-sprite,
projection and local-density budgets. Stain lifetime is independent of bleeding
and corpse physics. Ages pause with the game, persist across cells/saves and are
preserved when changing lifetime. Stains fade during their last ten seconds.

## Validation and limits

Tests cover the Bullet solver, existing wounds/poses, blood age/cell isolation,
visible stairs/columns/carpets, organic exclusions, sprite buffers, alpha-aware
density, loaded legacy pool removal, projection fairness/time/count budgets,
bounded impact queues, zero-amount shutdown, asynchronous BVH completion and
cancellation. Real ImageManager/EGL/OpenGL tests load the six DXT5 files and check
transparency, sunlight/darkness/torches, shadows, normal/reversed depth, floor
impact decals, density and absence of procedural pools.

Windows CI builds the runtime, runs physics/render/component/item integration
tests, preserves the compiled executable before packaging, checks installed
DDS/shader hashes, starts `openmw.exe --version` and verifies ZIP integrity.
These automated scenes do not replace an in-game Balmora playtest. Noncolliding
exterior decorations retain the stable collision receiver's limitations. Custom
whole-body NPC meshes and unusual creature skeletons retain preview.2's
compatibility limits; no new humanoid cuts are applied to creature skeletons.
