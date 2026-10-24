# Brutal Morrowind 0.7.0-preview.1

A modified OpenMW 0.51.0 Windows x64 runtime. This continues the working
Real Combat Ragdoll 0.6.0-preview.5 engine, retaining its physics controls,
enchantment handling, directional hit rules and one-second immediate-death window.

Extract the complete Windows ZIP into a new directory and run `openmw.exe`.
Morrowind data and the usual OpenMW configuration are required. The launcher and
editor are not included. Keep a separate copy of saves: this version adds saved
Brutal Morrowind wounds and blood records, which older engine builds cannot read.

## Blood

Successful physical health hits on NPCs emit ballistic droplets. Severed wounds
continue to spray and drip for the configured status duration (ten seconds at
defaults). The exposed neck has a stronger, pulsing arterial fountain. Droplets
follow gravity, sweep against collision triangles and stop at walls/floors.
Each stain's perimeter is projected onto the contacted surface, so a splat does
not simply float across a corner. Grounded wounds gradually form larger pools.
Wet blood darkens as it dries; stains fade during their last ten seconds.

Esc > Settings > Physics adds two English controls:

| Control | Minimum | Default | Maximum |
| --- | ---: | ---: | ---: |
| Blood amount | 0% (off) | 100% | 300% |
| Blood stain lifetime | 300 s | 600 s (10 minutes) | 1800 s |

The amount changes actual droplet emission and splat/pool size. Native blood
sprite size follows the same amount control. Stain lifetime is independent of
bleeding duration and corpse physics: freezing or looting the body does not erase
its floor/wall stains. Lifetime changes retain elapsed time. Ages stop while
paused, continue in other locations and are saved with the stain geometry and
cell ID. Returning to an unexpired cell restores its stains; interiors at the
same coordinates do not share them. Loading a save preserves remaining time.

Blood is batched per cell. A global limit of 8192 stains evicts the oldest in
extreme battles; nearby pools merge. Short hit sprays are limited to 32, and
each bleeding corpse has at most 192 live droplets. These limits prevent
unbounded scene/physics growth. Stains are attached to the location, not to moving
doors or clutter. Blood particles themselves are transient across save/load.

## Intestines and new physical wounds

Horizontal waist separation now exposes two loops of intestines. They are smooth,
rounded pink/red tubes with folds, veins and wet highlights, driven by 36 small
Bullet capsules and linked joints. They collide with world geometry, fall out of
the abdomen and remain attached to the torso. Their physical pose and joints are
saved with the corpse. They use the corpse's existing active-body/time budget.

The existing dismemberment chance applies to the new outcomes: 30% by default,
adjustable from 0% to 100%. No extra independent roll changes that probability.

| Weapon and lethal physical contact | New result after a successful wound roll |
| --- | --- |
| Two-handed long blade or two-handed axe, descending chop into head/torso | Longitudinal separation |
| Two-handed hammer, chop/slash into torso | Torso and extremities tear apart |
| Existing heavy horizontal slash into torso/eligible arm | Existing waist cut, now with intestines |

Longitudinal cuts clip actual posed triangles, interpolating UVs/normals and
retaining clothing/armour textures. Each side has its own pelvis, torso and head
collision bodies; the right arm/leg remain articulated with the right half.
Depth-tested flesh closes the exposed surfaces. Intestines appear on this cut
and on hammer torso tearing too. Removing equipment while looting updates the
visible halves. Whole robes remain uncut; the underlying severed body is exposed.
No equipment or inventory entries are removed, duplicated or replaced.

`BluntTwoClose` is the native two-handed hammer class. Staves (`BluntTwoWide`),
one-handed maces, spears and short blades do not inherit torso tearing. A thrust
does not produce a physical longitudinal cut. A vertical arm/leg hit does not
invent a centre-line cut. Existing one-handed cuts, crossbow head bursts and
direct elemental enchanted wounds retain their rules. Strong direct elemental
presentation still takes precedence over the physical wound where applicable.
Magic deaths before one second retain the impact; later DOT does not replay it.

Previous saved corpses retain their wounds. New features apply to new hits and
kills; loading an older save does not randomly recut its existing bodies.

## Validation and limits

Automated checks exercise the real Bullet solver, skinned mesh snapshots,
clipping/seam UVs, independent half poses, intestinal joints, saved blood ages,
cell visibility, blood amount endpoints and lifetime expiry. Actual EGL/OpenGL
tests cover blood/flesh under native normal/reversed-Z projection, inherited NPC
shaders/alpha testing, opaque wall occlusion and expiration. Windows CI also
builds the full runtime, runs component/native integration tests, starts the
installed executable with `--version`, and checks ZIP integrity.

These checks do not replace an in-game playtest with your Morrowind data.
Standard humanoid NPC body-part meshes are supported; custom whole-body meshes
and unusual skeletons need separate compatibility checks. Creatures retain their
existing ragdoll support, but do not receive these humanoid cuts or intestines.
