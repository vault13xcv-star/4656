-- Exercise the actual shipped combat module, with only its engine API mocked.
local armorRating = 0
local names = {'Boots','Cuirass','Greaves','Helmet','LBracer','LGauntlet','LPauldron',
    'RBracer','RGauntlet','RPauldron','Shield','LeftGauntlet','LeftPauldron',
    'RightGauntlet','RightPauldron','CarriedLeft'}
local slots = {}
for i,name in ipairs(names) do slots[name] = i end
local noInstance = function() return false end
local I = {Combat = {addOnHitHandler = function() end}}
local api = {
    ['openmw.core'] = {getGMST = function() return .25 end},
    ['openmw.interfaces'] = I,
    ['openmw.self'] = {},
    ['openmw.storage'] = {globalSection = function() return {get = function() return false end} end},
    ['openmw.types'] = {Actor = {EQUIPMENT_SLOT = slots}, Player = {objectIsInstance = noInstance},
        Creature = {objectIsInstance = noInstance}, Armor = {TYPE = slots, objectIsInstance = noInstance}},
    ['openmw_aux.util'] = {shallowCopy = function(value)
        local copy = {}; for k,v in pairs(value) do copy[k] = v end; return copy
    end},
}
require = function(name) assert(api[name], name); return api[name] end
local module = assert(loadstring(combatSource, 'actual-combat-local.lua'))()
I.Combat = module.interface
I.Combat.getArmorRating = function() return armorRating end
I.Combat.pickRandomArmor = function() return nil end
for _,armor in ipairs({0,20,200}) do
    armorRating = armor
    for _,raw in ipairs({1,10,100}) do
        for _,scale in ipairs({1,.51,.255,.357}) do
            local attack = {damage = {health = raw * scale}, _combatDamageScale = scale}
            I.Combat.applyArmor(attack)
            local original = math.max(1, raw * math.max(raw / (raw + armor), .25))
            assert(math.abs(attack.damage.health - original * scale) < .00001,
                'Armor penalized low mastery/early blocking a second time')
        end
    end
end
local blocked = {damage = {health = 0}, _combatDamageScale = 0}
I.Combat.applyArmor(blocked)
assert(blocked.damage.health == 0, 'Armor minimum damage bypassed a perfect shield block')
print('PASS actual Lua armor pipeline: mastery, exhaustion, early guard, full guard, and armor penetration')
