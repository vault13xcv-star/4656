Brutal Morrowind **1.3-combat.2 — experimental Windows x 64**.

Weak dagger hits could continuously interrupt a stronger opponent, while cheap stamina costs and immediate regeneration made sustained attacks and low-skill blocking too effective. This build increases the resource commitment and removes automatic flinches from tiny hits.

- Weapon and Block costs now decrease from three times the previous skill 1 cost to the previous skill 50 cost at mastery. Raise, hold and block-impact costs use Block skill.
- Preparation and recovery durations decrease from 3x at skill 1 to 1x at skill 100. Agility improves intermediate speed and shield raising. Actual strike/release speed remains original. Shield re-raises have a short recovery interval.
- Zero fatigue pauses natural regeneration for 10 seconds at agility 1, decreasing to 3 seconds at agility 100. Attacks, exhausted wind-ups and shield-raise attempts restart the wait. Potions/magic can restore fatigue. Continuing to attack empty retains half damage and 1.5x additional preparation/recovery durations.
- Weak hits no longer automatically trigger flinches. Meaningful hits use health/fatigue and agility thresholds, with a 1-second repeat-flinch grace period. Strong-hit knockdown retains native rules.
- Held shields now freeze the actual block track at its raised pose instead of continuing through the lowering animation. Release leaves unrelated equipment tracks intact.
- Dodge costs and cooldowns follow agility progression; distance and ordinary collisions are retained.

RMB holds the shield, Q dodges backwards/sideways, I opens inventory and G toggles auto-move. Rebinding remains available. No random hit roll is restored. Blood, dismemberment, physical projectiles and last-hostile slow motion remain from the previous build; stable Latest remains 1.1.

Native animation and combat regressions are compiled early on Windows before the full engine. The compiled engine is retained before test-program compilation, and packaging preserves the candidate if a later test fails. Validation status is recorded inside the ZIP.

Contacts use bounded blade-axis sweeps and native ray/capsule fallbacks and remain approximate. No dedicated controller defaults are supplied. Projectile remnants are session effects and disappear on load, cell changes or after ten real minutes. New in-flight projectile fields may not load in older versions.

Fully extract the Windows-x 64 ZIP separately and use copied saves. Game data and launcher/editor are not bundled. Automated checks do not replace a Morrowind-data playtest; no measured gameplay FPS or verified in-game guard duel is claimed. Corresponding sources and dependency notices accompany the release.
