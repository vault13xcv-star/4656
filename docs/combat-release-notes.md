Brutal Morrowind **1.3-combat.3 — experimental Windows x64**.

Walking and ordinary running no longer spend stamina. Short combat breaks restore stamina faster, Shift sprints, and RMB chooses shield block or dodge based on the carried equipment.

- Combat natural stamina recovery uses four times the previous native rate, including modified Endurance and GMST factors, capped at the outside-combat rate.
- Outside combat a full stamina pool recovers in 10 seconds at Endurance 1, decreasing to 5 seconds at Endurance 100. The existing 3–10 second exhaustion delay still applies first. Potions and Restore Fatigue retain their effect.
- Shift sprints on land at twice normal running speed, spends stamina and suppresses natural regeneration during the sprint. Sprint stops at zero; release Shift before starting again. Ordinary walking/running remain free; swimming, sneaking and jumping retain their costs.
- RMB with a shield raises/holds the shield; RMB without a shield starts one dodge per press. Standing still dodges backwards. Dodge remains lateral/backwards, requires sufficient positive stamina, and cannot start with a shield. The old Q dodge is retired. Caps Lock toggles ordinary run/walk; I opens inventory and G toggles auto-move.
- Accidental melee/projectile hits between non-player NPCs/creatures cause no physical damage, on-strike enchantments or puncture bleeding unless either actor is actively fighting the other. They still intercept the attack: melee is consumed and a physical projectile rebounds. Player damage and spell rules remain unchanged.

The prior skill/resource/tempo rebalance, exhaustion delay, anti-stunlock thresholds, held shield pose, physical projectiles, gore and blood are retained. The previous held-shield test fixture now supplies its missing LuaManager. Automated physics/render/combat, component and native integration results are preserved in the ZIP validation directory. These checks do not replace a Morrowind-data playtest. No measured FPS, in-game shield quality or guard duel is claimed.

Fully extract the Windows x64 ZIP separately and use copied saves. Game data and launcher/editor are not bundled. Contacts remain approximate; no dedicated controller defaults are added. Corresponding source archive and dependency notices accompany the prerelease. Stable Latest remains 1.1.
