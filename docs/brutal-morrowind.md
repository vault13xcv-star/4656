# Brutal Morrowind 0.8.0-preview.2

This release adds hybrid physical combat: driven weapon inertia, swept model
contacts, skill/speed damage, live stagger/knockdown and a held RMB shield.
See [physical-combat.md](physical-combat.md) for controls, exact shield percentages,
handling rules, validation and the scope of the hybrid simulation.

A modified OpenMW 0.51.0 Windows x64 runtime. This continues the working
Real Combat Ragdoll 0.6.0-preview.5 engine, retaining its physics controls,
enchantment handling, directional hit rules and one-second immediate-death window.

Extract the complete Windows ZIP into a new directory and run `openmw.exe`.
Morrowind data and the usual OpenMW configuration are required. The launcher and
editor are not included. Keep a separate copy of saves: this version adds saved
Brutal Morrowind wounds and blood records, which older engine builds cannot read.

## Fix for stalls after several dismemberments

Preview.2 fixes the rapidly increasing cost of accumulated gore. A full blood
cache now evicts its oldest entry without shifting the other 8191 records. Blood
uses persistent chunks of at most 128 stains; a new splat only updates affected
chunks, and drying/fading uses a shader clock without rebuilding the entire cache
ten times a second. Droplets and intestinal tubes retain their geometry/VBOs
across frames. Intestinal triangles share ring vertices instead of duplicating
them. Gore materials share their shader programs.

Legacy blood sprites rotate between wounds with at most two NIF particle systems
per pulse, rather than spawning another system at every end of every severed
joint. Ballistic blood still comes from every wound. Pool growth is sampled at
most four times per second per wound; at most eight droplet impacts per trail per
update project a splat perimeter. These bounds prevent a slow frame from
amplifying the next frame's blood work. Blood quantity, stain lifetime, existing
physical wounds, inventories and saved blood remain supported.

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

Blood is batched in small chunks per cell. A global limit of 8192 stains evicts the oldest in
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

The stall regression exercises ten concurrent waist/longitudinal/hammer deaths
at maximum blood, stable intestinal buffers, bounded legacy sprite spawning and
cleanup. It also fills and overflows all 8192 stain slots, verifies that idle
aging does not reupload geometry, restores a full saved cache and expires it.
Both GL depth modes draw a full cache with eight simultaneous blood trails.
In the same debug CPU-only ten-hammer-death benchmark, the tenth second fell from
129 ms/frame to approximately 25 ms/frame. This measures physics/effect updates,
not game FPS, and does not predict a particular Windows machine's performance.

These checks do not replace an in-game playtest with your Morrowind data.
Standard humanoid NPC body-part meshes are supported; custom whole-body meshes
and unusual skeletons need separate compatibility checks. Creatures retain their
existing ragdoll support, but do not receive these humanoid cuts or intestines.
