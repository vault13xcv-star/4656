# Brutal Morrowind 0.8.0-preview.2

New blood, physical intestines, longitudinal cuts and hammer torso tearing are
documented in [brutal-morrowind.md](brutal-morrowind.md). This continues preview.5;
the previous directional/elemental rules below remain except that a descending
two-handed sword/axe chop into head/torso now selects a longitudinal cut, and a
two-handed hammer torso swing can tear the body apart.

Wounds now retain the native chop/slash/thrust hit key and the anatomical part
captured on the living NPC before weapon damage, on-strike magic and deferred
Lua damage. Rendering rays identify the actual attached body/clothing/armour
part, so a foreground arm is not confused with the leg behind it. Whole robes
and untagged models use the current skeletal segments with signed capsule
distance; no later death pose or random blood-effect position chooses the cut.
A native confirmed damaging hit no longer requires a second visible mesh
intersection. If rendering cannot identify its surface, a ray through the living
skeletal capsules resolves the part; broad melee-cone hits use the nearest
segment to the aim. This restores one-handed cuts without random limb selection.
Unsupported skeletons still fall back to the native death animation.

| Physical weapon / attack | Eligible result after a successful wound roll |
| --- | --- |
| Long blade, chop or slash into head/arm/leg | Sever the struck extremity |
| Two-handed long blade or axe, horizontal slash into torso | Waist separation |
| Two-handed long blade or axe, horizontal slash into arm | Sever that arm or continue through the torso (equal shares of the wound chance) |
| Two-handed long blade / two-handed axe, descending chop into head/torso | Longitudinal separation |
| Other cutting chops into head/limb | Sever that extremity; no waist separation |
| Two-handed hammer, chop/slash into torso | Tear torso and extremities apart |
| Thrust, spear, short blade or unarmed | No physical severing or head burst |
| Blunt chop/slash into head | Head burst; other hit locations do not burst the head |
| Crossbow bolt into head | Head burst; other hit locations do not burst the head |

Direct elemental on-strike enchantments retain their separate magical wound
rules, including on spears and thrusts. A plain sword never bursts the head.

Death before **1.0 second** from the contact counts as immediate. Such deaths
retain the original magical blast/attraction and direct elemental wounds. At
1.0 second or later, DOT keeps the elemental death status but cannot replay the
original wave, weapon impulse or direct-hit dismemberment.

Native zero-time effect initialization already sets Flag_Applied without doing
any duration damage. That flag is no longer used to reject the first real damage
tick. Each shared contact advances once per positive simulation update, even
when several enchantment spell entries share it. Weapon and magical provenance
remain together inside the one-second window, including instant effects that
expire before Lua physical damage arrives.

Corpse creation briefly waits for either the physical Lua hit or the same
weapon's first elemental contribution, whichever is queued later. Thus physical
kills can retain their fire/frost/shock/poison status and elemental wounds.
Bow/crossbow WhenStrikes enchantments and ammunition enchantments use the same
contact; thrown weapons are not applied twice. Charge/resistance checks remain.
Existing saved corpse wounds are not recomputed.

Regression checks include real Bullet capsule targeting with no drawable mesh,
one-handed chop/slash cuts on head/both arms/both legs, the 0.999/1.0/1.001-second
boundary, zero-time priming before real duration damage, both weapon/magic
orders, and native CreatureStats queued-hit/death/pending presentation handling.

Previous slider and rendering fixes are retained:

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

- Chopping/slashing long blades detach the struck arm, leg or head. Short blades
  and thrusts are excluded.
- Two-handed long blades and axes can separate the upper torso only with a
  horizontal slash into torso/arm, as specified above.
- Blunt chopping/slashing head hits and crossbow head hits can burst the head.
- Direct lethal fire/frost damage detaches all extremities. Direct lethal shock
  bursts the head. DOT deaths at/after one second, restored effects and splash
  damage alone do not create these direct-contact wounds. On-strike enchantments share their
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
