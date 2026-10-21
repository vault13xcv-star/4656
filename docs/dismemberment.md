# Real Combat Ragdoll 0.6.0-preview.3

Physics sliders now span 50–300% of their baseline. Dismemberment instead uses
an absolute 0–100% probability with a 30% default, including exact RNG endpoints.
Baseline impulse limits are applied before the menu scale. Weapon momentum is
distributed over the corpse with a bounded local contact share, so light heads
and hands no longer clip most of a strong hit. Horizontal throw scaling keeps
the baseline flight time and scales initial horizontal momentum by exactly
0.5–3. Final settled travel also includes toppling, friction and collision
responses, so it is not guaranteed to be exactly half/triple on every surface.
Experiments with a ground-slide estimator were discarded because they compressed
the minimum/default response on the target Windows solver.
Zero-area direct elemental spells now have a one-metre reference throw.

| Control | Minimum | Default | Maximum |
| --- | ---: | ---: | ---: |
| Melee, crossbow, bow, thrown weapon and magic strength | 50% | 100% | 300% |
| Active corpses / active items (independent) | 50 | 100 | 300 |
| Corpse / item physics duration (independent) | 5 s | 10 s | 30 s |
| Crossbow pinning search distance | 1.25 m | 2.5 m | 7.5 m |
| Status duration | 50% | 100% | 300% |
| Dismemberment chance | 0% | 30% | 100% |

Changes affect new impacts. Timers and budgets update on resume without renewing
elapsed time. A higher active budget permits more newly active bodies/items; it
does not restart entities whose simulation has already ended. Pinning still
requires the corpse to physically reach the wall. Tests cover actual Bullet
momentum/travel at all three slider settings, saturated head/hand/item hits,
300-body/item budgets, live timers, pinning bounds and status deadlines.

Fixes the stretched clothing and visible-through-walls wound surfaces reported
in preview.1. Skin weights across a cut now use virtual bones relative to the
owning body part; intact elbows/knees remain articulated and robes stay whole.
Relative overrides follow the current frame, including elemental coating clones
and saved corpses. Wound caps and head debris use opaque depth writes and the
native projection uniform, supporting both normal and reversed Z buffers.

Based on the 0.5.2-preview.1 build at engine commit
66588b7c5a88a48f0beee689770dc00b916adc28.

Melee clutter contact now measures weapon reach from the actor's body, rather
than subtracting the eye's height from its useful range. A small multi-ray
contact area still stops at physical obstructions. Actor contacts use the visible
surface point when the aim ray intersects the selected actor, with the native
actor-cone fallback otherwise. Impulses do not change damage or hit chance.

New NPC death effects use one probability roll per lethal contact:

- Long blades detach the struck arm, leg or head. Short blades are excluded.
- Two-handed long blades and axes can also separate the upper torso at the waist
  when the lethal contact strikes the torso.
- Blunt weapons burst the head; crossbow bolts do so only on a head contact.
- Direct lethal fire/frost damage detaches all extremities. Direct lethal shock
  bursts the head. Damage over time, restored effects and splash damage alone do
  not create these direct-contact wounds. On-strike enchantments share their
  native contact with physical weapon damage.

At defaults the probability is 30%. Esc > Settings > Physics has a
**Dismemberment chance** control, from 0% to 100% probability.
The English Physics controls include melee, crossbow, bow, thrown
weapons and magic impulses, independent corpse/item budgets and lifetimes,
crossbow pinning distance and status lifetime. Reset includes the new option.
The status multiplier controls bleeding too: 10 seconds at 100%, 5–30 seconds
across the slider. Changing it preserves elapsed time.

Severed limbs retain their internal ragdoll joints. The renderer uses the NPC's
existing worn body-part meshes, so armour and clothing follow their body parts.
Robes remain whole on the torso; severed underlying skin parts hidden by the robe
are exposed separately. Head explosions use six small physical debris pieces.
Cut surfaces have simple dark-red caps and emit the game's standard blood VFX.
The original NPC reference retains all loot; severing creates no inventory copies
and removes no equipment. Reloading preserves wounds and their remaining timers;
cell unloading resets the transient physical presentation as in the base build.
Older saves without wound data continue to load. Saves produced by this version
contain an additional RWND record and should be used with this version or later.

Validation includes the actual Bullet solver, synthetic humanoid rig, debris,
blood deadline, pose restoration, wound-selection boundaries and native clutter
reach regression. These checks require no copyrighted game resources. A Windows
build and successful automated checks do not substitute for an in-game test with
your Morrowind data. Custom meshes that are not segmented like the standard
Morrowind NPC body parts, and non-NIF robe skinning, need separate compatibility
checks. The present feature applies to humanoid NPCs, not creature dismemberment.
