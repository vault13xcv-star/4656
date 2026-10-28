Brutal Morrowind **1.3-combat.3 — experimental Windows x64**.

**Validation notice: retained engine, one test-fixture failure**

The executable is from commit `af9a8c9b15aa8a7cf4f8f71445c43717e35d118d`, Windows run [40698202596](https://github.com/vault13xcv-star/4656/actions/runs/40698202596). All 5 physics/render/combat groups, 1405 component tests and 45 of 46 native tests passed. The held-shield animation test passed. The new allied-contact test raised an access violation while constructing AI packages: its synthetic environment omitted World, whose PRNG initializes AI reaction timers. The source branch adds a PRNG-only World fixture; the corrected full test was compiled locally but not rerun on Windows. The full allied-contact integration check therefore remains unverified.

The compiled EXE and all original runtime files are retained unchanged; KNOWN-ISSUES.txt discloses the failure, and VALIDATION.txt retains the original failed result. This publication does not claim all native tests passed or that an in-game playtest was performed.


Walking and ordinary running no longer spend stamina. Short combat breaks restore stamina faster, Shift sprints, and RMB chooses shield block or dodge based on the carried equipment.

- Combat natural stamina recovery uses four times the previous native rate, including modified Endurance and GMST factors, capped at the outside-combat rate.
- Outside combat a full stamina pool recovers in 10 seconds at Endurance 1, decreasing to 5 seconds at Endurance 100. The existing 3–10 second exhaustion delay still applies first. Potions and Restore Fatigue retain their effect.
- Shift sprints on land at twice normal running speed, spends stamina and suppresses natural regeneration during the sprint. Sprint stops at zero; release Shift before starting again. Ordinary walking/running remain free; swimming, sneaking and jumping retain their costs.
- RMB with a shield raises/holds the shield; RMB without a shield starts one dodge per press. Standing still dodges backwards. Dodge remains lateral/backwards, requires sufficient positive stamina, and cannot start with a shield. The old Q dodge is retired. Caps Lock toggles ordinary run/walk; I opens inventory and G toggles auto-move.
- Accidental melee/projectile hits between non-player NPCs/creatures cause no physical damage, on-strike enchantments or puncture bleeding unless either actor is actively fighting the other. They still intercept the attack: melee is consumed and a physical projectile rebounds. Player damage and spell rules remain unchanged.

The prior skill/resource/tempo rebalance, exhaustion delay, anti-stunlock thresholds, held shield pose, physical projectiles, gore and blood are retained. The previous held-shield test fixture now supplies its missing LuaManager. Automated physics/render/combat, component and native integration results are preserved in the ZIP validation directory. These checks do not replace a Morrowind-data playtest. No measured FPS, in-game shield quality or guard duel is claimed.

Fully extract the Windows x64 ZIP separately and use copied saves. Game data and launcher/editor are not bundled. Contacts remain approximate; no dedicated controller defaults are added. Corresponding source archive and dependency notices accompany the prerelease. Stable Latest remains 1.1.
