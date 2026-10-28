Brutal Morrowind **1.3-combat.4 — experimental Windows x64**.

Fast regeneration previously refilled stamina during sustained attacks and shield holds. This build requires a real combat break to recover and limits backwards ranged kiting for both the player and NPCs.

- Ordinary backwards running on land is half speed, including backwards diagonals. Forward/sideways running and ordinary walking retain their speed. The shared movement controller applies this to the player, NPCs and creatures.
- Backwards sprint is disabled. Both requested and smoothed movement are checked, so changing direction cannot give a backwards sprint frame. Dodge distance stays 160 units.
- Dodge stamina and all shield costs (raising, holding and impact) are doubled. Skill and Agility progression still apply.
- In combat, natural stamina regeneration waits two seconds after effort ends. Attacks, sustained wind-up/draw, release, follow-through/reload, casting, shield work, dodge, sprint, jump and stamina-consuming movement refresh the wait. Ordinary walking/running allow recovery after the quiet interval.
- At zero stamina, the existing Agility-dependent 3–10 second wait remains. The waits overlap; they are not added. Outside-combat recovery stays 5–10 seconds for a full pool. Explicit potion/magic restoration is retained.

Weapon damage, the fourfold capped combat recovery rate, skill tempo, gore/blood, physical projectiles, allied physical-contact protection and last-hostile slow motion are retained. Damage is not reduced again to lengthen fights. The prior allied-contact test now supplies the World PRNG required when constructing native AI packages.

Automated model/physics/render tests exercise sustained combat, shield exhaustion, useful breaks, exact cooldowns, doubled costs and retreat directions; native/component results and installed executable checks are preserved in the ZIP validation directory. These checks do not replace a Morrowind-data playtest or establish actual duel length/FPS.

Fully extract the Windows x64 ZIP separately and use copied saves. Game data and launcher/editor are not bundled. Corresponding sources and dependency notices accompany the prerelease. Stable Latest remains 1.1.
