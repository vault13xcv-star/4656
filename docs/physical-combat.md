# Brutal Morrowind 0.8.0-preview.1 — hybrid physical combat

This preview adds driven weapon inertia, model-based melee contacts, live-body
impulses and an active shield to the existing blood/dismemberment runtime.
Extract the complete Windows ZIP into a new directory. Keep the working 0.7
installation and use a separate test save while checking your animation mods.

## Controls and shields

Hold the normal attack button to prepare a swing; release it to strike.
With a usable shield and a drawn one-handed weapon, **hold RMB to raise the
shield; release RMB to lower it**. In this situation RMB is consumed by combat
instead of its old inventory binding. Outside this situation the existing mouse
bindings remain available. Sheathe the weapon to use RMB for inventory, or assign
inventory to another key in Controls. Spells retain their existing ready/cast
controls. Two-handed weapons cannot use the shield.

The shield takes 0.28 seconds to raise at Block 0 and 0.12 seconds at Block 100.
The first 0.22 seconds after it becomes ready are the timed-block window. Holding
it longer gives the early-block result; holding it does not renew that window.
Protection is frontal (approximately 130 degrees), and an unfinished raise gives
no protection. Each incoming hit costs fatigue and shield durability.

| Shield state | Incoming physical damage passed through |
| --- | ---: |
| Timed, fatigue available | 0% |
| Raised too early, fatigue available | 50% |
| Timed, zero fatigue | 50% |
| Raised too early, zero fatigue | 90% |

Zero fatigue also doubles raising time. Block skill reduces raising/blocking
fatigue costs and shield wear. Raising costs once per raise, rather than every
frame. Repeated raises have a short release interval. Arrows/bolts/thrown weapons
also use the active guard. Full shield deflections prevent on-strike enchantments
from being applied to the body; partial blocks reduce their magnitudes by the
same transmission factor. Enchantment self effects and effects on other targets
in the surrounding area keep their normal rules. The percentages apply before
the normal armour, resistance and difficulty calculations.
NPCs raise shields in response to an opponent preparing/performing an attack,
using the same costs, raising times and damage rules.

## Handling and damage

| Weapon | Attribute contribution to handling |
| --- | --- |
| Two-handed long blades, axes and close-grip hammers | Strength |
| Short blades and one-handed long blades | 30% Strength, 70% Agility |
| One-handed axes/maces, spears and wide-grip staves | 50% Strength, 50% Agility |

The weapon's matching skill adds to its attribute handling bonus. It does not
multiply the two attribute contributions. Weight, reach and measured model length
set rotational inertia. Heavy weapons take longer to accelerate/recover; their
striking phase remains brisk. A bounded spring correction gives the arm/weapon
actual lag during changes of direction and settles after movement stops.

Wind-up, strike and recovery have separate rates. Stats above 150 do not keep
accelerating the motor, and a complete attack has a minimum cadence of
0.36–0.85 seconds depending on weapon/skill. These are additional bounds, not a
promise that every animation completes in that time. With zero fatigue every
phase runs at exactly half its normal rate and the minimum cycle doubles.

There is **no hit/miss skill roll** for native melee or projectile contacts.
Weapon skill instead scales damage from 55% at skill 0 to 130% at skill 100
(62.5% at skill 10). Skill still affects handling independently.

Contact speed is measured from the moving weapon relative to the victim. Running
into a thrust and falling into a descending strike add their velocity in the
appropriate direction. Moving together does not create extra closing speed.
The kinetic damage factor increases with squared relative speed and smoothly
approaches an upper bound of 3.0 (minimum 0.35), retaining the usual weapon
damage range, charge, condition, strength, armour, difficulty and resistances.
Existing dismemberment chances and directional/elemental eligibility remain.

## Contacts and living reactions

NPCs, the player and armed bipeds use the equipped model's blade/shaft segment.
Bounded longitudinal and transverse traces cover its movement throughout the
strike and follow-through, rather than selecting an aimed target during wind-up.
The first obstruction stops the offensive stroke. A surviving target causes a
short contact pause and a withdrawal along the preceding trajectory, with arm
recoil. If deferred damage kills the target, the swing may complete its arc.
During a resisted contact, movement into the obstruction is constrained too;
walking forward cannot simply carry the stopped blade through the body. Light
movable clutter receives momentum without arresting the whole weapon stroke.
One attack applies one physical contact; it does not repeatedly damage an enemy
while the weapon rests against it. Weapon clashes transfer recoil without body
damage. A lowered shield does not grant an automatic successful block.

The collision sampler uses the current keyframe time, NIF transition easing and
the same aiming/procedural corrections as rendering. Anatomical contacts on
clothes/armour retain their body-part metadata. Whole robes use the nearest
anatomy at the actual contact point. The invisible first-person player torso
uses its native collision shape.

Effective weapon mass and contact speed drive collision-aware live-body push,
spring-driven torso/pelvis lean and accumulated loss of balance. Very strong
blows cause native knockdown; smaller blows can stagger and then settle.
Physical item impacts use the measured momentum too. Existing Physics impulse
sliders also scale the new live-body impacts. Magic/DOT ticks never replay the
physical contact's living impulse. Transient guard/motor/contact state resets
on load and resurrection; the save format is unchanged from 0.7.

## Scope and validation

This is a **hybrid**, not a replacement of every animation with a fully simulated
active ragdoll. Authored poses supply the base attack/locomotion, with physical
handling, a driven arm motor, collision stopping and procedural body reactions.
Living actors still use OpenMW's character collision solver; there is no new
full-body muscle solver, foot-placement IK or independently simulated living
weapon rigid body. Natural non-bipedal bites/claws and missing weapon-animation
fallbacks retain the native contact selection, with the random miss roll removed.

The weapon collision proxy is its main segment, not a triangle-exact simulation
of every blade edge or axe head. The guard uses a frontal coverage rule, with
shield mesh contacts distinguished from weapon clashes. Unusual whole-body
meshes, custom skeletons, extreme weapon shapes and animation replacements need
in-game compatibility checks. No gameplay test with copyrighted Morrowind assets
is possible in this build environment.

Automated coverage includes handling/stat/skill/fatigue endpoints, heavy-weapon
strength response, driven inertia and settling, shield timing/transmission/costs,
bounded blade sweeps and deterministic knockdown. Native engine tests compare
current collision poses to actual aiming/blending callbacks, including partial
NIF tracks, and check native weapon classes and momentum expiry after DOT.
The previous crowded blood/intestinal physics, saved gore and normal/reversed-Z
rendering regressions also run, followed by the full Windows component/native
tests and installed executable/package checks.
