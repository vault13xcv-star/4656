# Real Combat Ragdoll 0.6.0-preview.2

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

At defaults the probability is 30%. Esc > Settings > Physics has a new
**Dismemberment chance** multiplier, from 50% to 200% of that default (15–60%
probability). The English Physics controls include melee, crossbow, bow, thrown
weapons and magic impulses, independent corpse/item budgets and lifetimes,
crossbow pinning distance and status lifetime. Reset includes the new option.
The status multiplier controls bleeding too: 10 seconds at 100%, 5–20 seconds
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
