# Procedural blood — experimental 1.2

This branch builds on Brutal Morrowind 1.1. Blood droplets and stains use
procedural shaders; stains remain projected decals rather than fluid surfaces.
The runtime package does not include the six blood DDS
images. Corpse/cut artwork is unchanged. Existing impact particles travel under
gravity, sweep against receivers and project splashes at actual contacts.
The stock BloodSplat NIF/DDS effect is suppressed; native weapon-hit blood uses
the procedural trail rather than adding a second textured effect.

Small bounded surface particles are rendered while moving downhill along contacted surfaces,
and leave narrower directional marks. Ceiling blood
and flows that reach an edge become airborne drops again. Floor contacts collect
into limited local pools; new projected footprints replace smaller footprints
instead of stacking indefinitely. Stains and pools keep their saved ages and
expire with the blood-lifetime setting. Per-trail particle, flow, pool and
projection queues are capped. The procedural density filter limits nearby
impacts without cutting rectangular holes in the stain geometry.

Interiors use a snapshot of visible meshes, excluding actors and particles.
Outdoors, visible world geometry is combined with the existing collision
receiver: nearby visible meshes (within a 512-unit capture radius) are preferred, while collision remains a
fallback for terrain and nearer obstacles. Geometry capture, receiver building
and projection remain bounded. Snapshot invalidation and asynchronous building
can delay a contact; the ballistic cursor continues flying and replays missed
sweeps when receivers are ready. Native ambient, sun, local lights, fog and
shadows affect the procedural blood material.

These are bounded approximations rather than a full fluid simulation. Pools
are projected footprints, not simulated liquid volumes or a persistent surface
flow field. Stains are in world coordinates and do not follow objects after they
move. Receiver capture has finite limits and may not include every unusual
third-party object. Actual visual quality, exterior coverage and frame rate
need verification with Morrowind game data; synthetic and automated tests do
not establish those results. Use a separate installation and copies of saves:
older versions can discard the new pool records.

The usual dismemberment, corpse textures, inventory and combat behavior are
inherited from 1.1. Run `openmw.exe` from the fully extracted Windows package
using your own Morrowind data and OpenMW configuration. The launcher, editor
and game assets are not included.
