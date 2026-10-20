#include <apps/openmw/mwmechanics/ragdollimpact.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/mathutil.hpp>
#include <components/ragdoll/profiles.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
    using Element = Ragdoll::DeathElement;

    void check(bool condition, const std::string& message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    bool near(float actual, float expected, float tolerance = 0.0001f)
    {
        return std::abs(actual - expected) <= tolerance;
    }

    bool near(const osg::Vec3f& actual, const osg::Vec3f& expected)
    {
        return (actual - expected).length() <= 0.0001f;
    }

    Ragdoll::Impact magic(const osg::Vec3f& direction, float distance,
        Element element = Element::None, float effectiveDamage = 1.f)
    {
        Ragdoll::Impact result;
        result.mMagicDirection = direction;
        result.mMagicDistance = element == Element::None ? 0.f : distance;
        result.mPosition = osg::Vec3f(10, 20, 30);
        result.mDeathElement = element;
        result.mDominantMagicDamage = effectiveDamage;
        result.mShock = element == Element::Shock;
        result.mValid = true;
        return result;
    }

    void testDominantDamageSelectsMotionAndElement()
    {
        MWMechanics::RagdollHit hit;
        hit.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 8, Element::Fire, 4));
        hit.applyMagic(2, magic(osg::Vec3f(0, 1, 0), 2, Element::Shock, 12));
        check(hit.mApplied.mDeathElement == Element::Shock && hit.mApplied.mShock,
            "A weaker wide-area fire effect suppressed stronger shock");
        check(near(hit.mApplied.mDominantMagicDamage, 12), "Dominant damage was added instead of selected");
        check(near(hit.mApplied.mMagicDistance, 2) && near(hit.mApplied.mMagicDirection, osg::Vec3f(0, 1, 0)),
            "The dominant shock must select its own radius and direction");

        hit.applyMagic(3, magic(osg::Vec3f(0, 0, 1), 20, Element::Frost, 3));
        check(hit.mApplied.mDeathElement == Element::Shock && near(hit.mApplied.mMagicDistance, 2),
            "A larger radius incorrectly determined the death element");
        hit.applyMagic(4, magic(osg::Vec3f(1, 0, 0), 1, Element::None, 15));
        check(
            hit.mApplied.mDeathElement == Element::None && !hit.mApplied.mShock && near(hit.mApplied.mMagicDistance, 0),
            "Stronger ordinary health damage failed to suppress elemental presentation");
        hit.applyMagic(5, magic(osg::Vec3f(1, 0, 0), 1, Element::Poison, 18));
        check(hit.mApplied.mDeathElement == Element::Poison, "Dominant poison did not select slime");
        hit.applyMagic(6, magic(osg::Vec3f(1, 0, 0), 1, Element::Fire, 22));
        check(hit.mApplied.mDeathElement == Element::Fire, "Dominant fire did not replace poison");
        hit.applyMagic(7, magic(osg::Vec3f(1, 0, 0), 1, Element::Frost, 25));
        check(hit.mApplied.mDeathElement == Element::Frost && !hit.mApplied.mShock,
            "Dominant frost retained another elemental presentation");
    }

    void testOnlyEffectiveDamageContributes()
    {
        MWMechanics::RagdollHit hit;
        hit.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 3, Element::Fire, 5));
        // These represent the effective amounts delivered by the post-protection
        // application hook: the configured spell magnitudes are intentionally absent.
        hit.applyMagic(2, magic(osg::Vec3f(0, 1, 0), 10, Element::Shock, 2));
        check(hit.mApplied.mDeathElement == Element::Fire && !hit.mApplied.mShock,
            "A weaker resisted shock effect retained convulsions");
        hit.applyMagic(3, magic(osg::Vec3f(0, 0, 1), 100, Element::Frost, 0));
        check(hit.mApplied.mDeathElement == Element::Fire && near(hit.mApplied.mMagicDistance, 3),
            "A fully resisted effect contributed a death element or impulse");
        hit.applyMagic(4, magic(osg::Vec3f(0, 0, 1), 100, Element::Poison, -8));
        hit.applyMagic(5, magic(osg::Vec3f(0, 0, 1), 100, Element::Poison,
            std::numeric_limits<float>::quiet_NaN()));
        check(hit.mMagic.size() == 2 && hit.mApplied.mDeathElement == Element::Fire,
            "Invalid/non-damaging contributions entered attribution");
        hit.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 3, Element::Fire, 0));
        check(hit.mApplied.mDeathElement == Element::Shock && hit.mApplied.mShock,
            "Removing a zero-damage contribution left stale dominant fire");
    }

    void testDistinctEffectsAndDurationTicks()
    {
        MWMechanics::RagdollHit hit;
        hit.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 6, Element::Fire, 5));
        for (int tick = 0; tick < 600; ++tick)
            hit.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 6, Element::Fire, 5));
        check(near(hit.mApplied.mMagicDistance, 6) && near(hit.mApplied.mDominantMagicDamage, 5),
            "Duration ticks multiplied one effect's damage or impulse");
        hit.applyMagic(2, magic(osg::Vec3f(0, 1, 0), 8, Element::Shock, 6));
        check(near(hit.mApplied.mMagicDistance, 8) && hit.mApplied.mShock,
            "Distinct effects inflated the blast or lost the stronger element");
        hit.applyMagic(2, magic(osg::Vec3f(0, 1, 0), 8, Element::Shock, 2));
        check(hit.mApplied.mDeathElement == Element::Fire && !hit.mApplied.mShock,
            "Replacing a weaker duration tick retained stale shock dominance");
        check(near(hit.mApplied.mMagicDistance, 6), "The newly dominant fire failed to select its own radius");
    }

    void testWeaponAndMagicOrder()
    {
        MWMechanics::RagdollHit spellFirst;
        spellFirst.mWeapon.mWeaponDirection = osg::Vec3f(0, 1, 0);
        spellFirst.mWeapon.mWeaponDistance = 2;
        spellFirst.mWeapon.mCrossbow = true;
        spellFirst.mWeapon.mPosition = osg::Vec3f(4, 5, 6);
        spellFirst.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 5, Element::Shock, 9));
        spellFirst.applyWeapon();

        MWMechanics::RagdollHit weaponFirst;
        weaponFirst.mWeapon = spellFirst.mWeapon;
        weaponFirst.applyWeapon();
        weaponFirst.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 5, Element::Shock, 9));
        for (const auto* hit : { &spellFirst, &weaponFirst })
        {
            const auto& applied = hit->mApplied;
            check(applied.mValid && applied.mCrossbow && applied.mShock
                    && applied.mDeathElement == Element::Shock && near(applied.mDominantMagicDamage, 9),
                "Asynchronous weapon/magic ordering lost elemental attribution");
            check(near(applied.mWeaponDistance, 2) && near(applied.mMagicDistance, 5),
                "Weapon and magic ordering dropped a distance contribution");
            check(near(applied.mWeaponDirection, osg::Vec3f(0, 1, 0))
                    && near(applied.mMagicDirection, osg::Vec3f(1, 0, 0)),
                "Weapon and magic ordering dropped separate impact directions");
        }
    }

    void testEqualEffectsAndContactIsolation()
    {
        MWMechanics::RagdollHit first;
        first.applyMagic(2, magic(osg::Vec3f(-1, 0, 0), 5, Element::Shock, 8));
        first.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 5, Element::Fire, 8));
        check(first.mApplied.mDeathElement == Element::Fire && !first.mApplied.mShock,
            "Equal damage did not preserve native effect order independently of delivery order");
        check(near(first.mApplied.mMagicDistance, 5), "Equal-area effects multiplied the spell's throw");
        check(near(first.mApplied.mMagicDirection.length(), 1), "Equal-area effects have an invalid direction");

        MWMechanics::RagdollHit second;
        second.applyMagic(1, magic(osg::Vec3f(0, 1, 0), 2, Element::Poison, 100));
        check(first.mApplied.mDeathElement == Element::Fire && near(first.mApplied.mDominantMagicDamage, 8),
            "An unrelated contact changed an earlier death element");
        check(second.mApplied.mDeathElement == Element::Poison && near(second.mApplied.mDominantMagicDamage, 100),
            "Independent effect identifiers collided across contacts");
    }

    void testExplosionDistanceAndDirection()
    {
        MWMechanics::RagdollMagicImpact context;
        context.mOrigin = osg::Vec3f(100, -200, 30);
        context.mDirection = osg::Vec3f(0, 0, -3);
        context.mRadius = 10;
        context.mExploded = true;
        const auto impact = MWMechanics::makeRagdollMagicImpact(
            context, context.mOrigin + osg::Vec3f(4 * Constants::UnitsPerMeter, 0, 0), 1, Element::Fire, 30);
        check(near(impact.mMagicDistance, 6), "10 metre blast at 4 metres did not produce a 6 metre throw");
        check(near(impact.mMagicDirection, osg::Vec3f(1, 0, 0)),
            "Indirect explosion used the projectile direction instead of the epicentre");
        check(near(impact.mPosition, context.mOrigin), "Explosion lost its original world-space epicentre");
        check(impact.mDeathElement == Element::Fire && near(impact.mDominantMagicDamage, 30),
            "Explosion construction lost effective damage or element");

        const auto vertical = MWMechanics::makeRagdollMagicImpact(
            context, context.mOrigin + osg::Vec3f(0, 0, 4 * Constants::UnitsPerMeter), 1, Element::Shock, 40);
        check(near(vertical.mMagicDirection, osg::Vec3f(0, 0, 1)) && near(vertical.mMagicDistance, 6),
            "Explosion direction discarded vertical separation");
        check(vertical.mShock, "Shock contribution was not marked for convulsions");

        const auto edge = MWMechanics::makeRagdollMagicImpact(
            context, context.mOrigin + osg::Vec3f(9 * Constants::UnitsPerMeter, 0, 0), 1, Element::Fire, 3);
        check(near(edge.mMagicDistance, 1), "Magic must fall to zero at the blast edge without a minimum throw");

        context.mExploded = false;
        context.mRadius = 0;
        const auto direct
            = MWMechanics::makeRagdollMagicImpact(context, osg::Vec3f(900, 800, 700), 1, Element::Frost, 6);
        check(near(direct.mMagicDistance, 0) && direct.mDeathElement == Element::Frost
                && near(direct.mPosition, context.mOrigin),
            "Zero-area magic must retain frost presentation without displacement");

        context.mExploded = true;
        context.mDirection = osg::Vec3f();
        context.mRadius = 10;
        const auto atCentre = MWMechanics::makeRagdollMagicImpact(context, context.mOrigin, 1, Element::Fire, 1);
        check(near(atCentre.mMagicDirection.length(), 1) && near(atCentre.mMagicDistance, 10),
            "Coincident explosion and victim produced an invalid direction");
    }

    void testRadiusBoundsAndHealthMagic()
    {
        MWMechanics::RagdollMagicImpact context;
        context.mRadius = 10;
        context.mExploded = true;
        context.mDirection.set(1, 0, 0);
        for (float distance : { 9.9f, 10.f, 11.f })
        {
            const auto impact = MWMechanics::makeRagdollMagicImpact(
                context, osg::Vec3f(distance * Constants::UnitsPerMeter, 0, 0), 1, Element::Fire, 10);
            check(near(impact.mMagicDistance, std::max(0.f, 10.f - distance)),
                "Blast boundary must not acquire a minimum impulse");
        }
        for (bool exploded : { false, true })
        {
            context.mExploded = exploded;
            const auto impact = MWMechanics::makeRagdollMagicImpact(context, osg::Vec3f(), 1, Element::None, 20);
            check(impact.mValid && near(impact.mDominantMagicDamage, 20) && near(impact.mMagicDistance, 0),
                "Health damage must participate in attribution without a magical throw");
        }
        context.mExploded = false;
        const auto direct
            = MWMechanics::makeRagdollMagicImpact(context, osg::Vec3f(999, 999, 999), 1, Element::Frost, 10);
        check(near(direct.mMagicDistance, 10) && near(direct.mMagicDirection, osg::Vec3f(1, 0, 0)),
            "Direct nonzero-area projectile must use its radius and incoming direction");
        for (float radius : { -1.f, std::numeric_limits<float>::quiet_NaN() })
        {
            context.mRadius = radius;
            const auto invalid = MWMechanics::makeRagdollMagicImpact(context, osg::Vec3f(), 1, Element::Frost, 10);
            check(near(invalid.mMagicDistance, 0), "Invalid radius produced a launch");
        }
    }

    void testPoisonAttractionAndMeleeMultipliers()
    {
        MWMechanics::RagdollMagicImpact context;
        context.mRadius = 10;
        context.mExploded = true;
        context.mOrigin.set(0, 0, 0);
        for (const auto [distance, expected] : { std::pair(3.f, 3.f), std::pair(9.9f, 5.05f), std::pair(11.f, 0.f) })
        {
            const auto impact = MWMechanics::makeRagdollMagicImpact(
                context, osg::Vec3f(distance * Constants::UnitsPerMeter, 0, 0), 1, Element::Poison, 20);
            check(impact.mMagicAttraction && near(impact.mMagicDistance, expected) && impact.mMagicDirection.x() < 0,
                "Poison must use bounded inward displacement instead of an outward explosion");
        }
        for (const float strength : { 0.f, 40.f, 70.f, 100.f, 150.f })
        {
            const float bonus = std::clamp((strength - 40.f) / 60.f, 0.f, 1.f);
            check(near(Ragdoll::meleeMultiplier(1, 1, 10, strength), 1 + bonus),
                "Light melee weapon has the wrong bonus");
            check(near(Ragdoll::meleeMultiplier(10, 1, 10, strength), 2 + bonus),
                "Heavy melee weapon has the wrong bonus");
            check(near(Ragdoll::meleeMultiplier(10, 1, 1, strength), 1 + bonus), "Equal weight limits divided by zero");
            check(near(Ragdoll::meleeMultiplier(10, 1, 10, strength, false), 1 + bonus),
                "Natural attack received a weapon bonus");
        }
        check(near(Ragdoll::meleeMultiplier(5.5f, 1, 10, 70), 2), "Melee bonuses were multiplied instead of added");
        check(Ragdoll::creatureProfile("centurion_spider", "r/SteamCenturion.nif").mExcluded,
            "Automaton classification depends on a translated display name");
        check(Ragdoll::creatureProfile("rat", "r/rat.nif").mMass
                < Ragdoll::creatureProfile("skeleton", "r/skeleton.nif").mMass,
            "Rat and skeleton mass profiles lost their ordering");
        check(Ragdoll::creatureProfile("ogrim", "r/ogrim.nif").mMass > 300, "Ogrim must be a very heavy creature");
        check(near(Ragdoll::creatureProfile("mod_lizard", "r/lizard.nif", { "lizard:25:0.9" }).mMass, 25),
            "Custom creature mass profile was ignored");
    }

    void testRestoredEffectPresentationHasNoInventedImpulse()
    {
        MWMechanics::RagdollMagicImpact context;
        context.mHasImpulse = false;
        const auto fire = MWMechanics::makeRagdollMagicImpact(context, osg::Vec3f(100, 200, 300), 1, Element::Fire, 8);
        check(fire.mValid && fire.mDeathElement == Element::Fire && near(fire.mDominantMagicDamage, 8),
            "A restored active fire effect lost its death presentation");
        check(near(fire.mMagicDistance, 0) && near(fire.mWeaponDistance, 0)
                && near(fire.mMagicDirection, osg::Vec3f()) && !fire.mCrossbow,
            "A restored effect invented an explosion direction, radius or weapon impulse");
        MWMechanics::RagdollHit spell;
        spell.applyMagic(1, fire);
        spell.applyMagic(
            2, MWMechanics::makeRagdollMagicImpact(context, osg::Vec3f(100, 200, 300), 1, Element::Frost, 10));
        check(spell.mApplied.mDeathElement == Element::Frost && near(spell.mApplied.mMagicDistance, 0),
            "Restored multi-effect spells lost exclusive strongest-element selection");
        spell.applyMagic(
            3, MWMechanics::makeRagdollMagicImpact(context, osg::Vec3f(100, 200, 300), 1, Element::None, 12));
        check(spell.mApplied.mDeathElement == Element::None,
            "Stronger restored ordinary health damage retained elemental presentation");
    }

    void testExplosionWaveGeometry()
    {
        MWMechanics::RagdollMagicImpact context;
        context.mExploded = true;
        context.mHasTargetPosition = true;
        context.mRadius = 20;
        context.mOrigin.set(800,-400,60);
        for (int i=0; i<16; ++i)
        {
            const float angle = i * 3.14159265f / 8;
            const osg::Vec3f outward(std::cos(angle), std::sin(angle), 0);
            context.mTargetPosition = context.mOrigin + outward * (4 * Constants::UnitsPerMeter);
            context.mDirection = -outward; // Caster aim must not reverse the spherical wave.
            const auto hit
                = MWMechanics::makeRagdollMagicImpact(context, context.mOrigin - outward * 500, 1, Element::Fire, 50);
            check(near(hit.mMagicDirection, outward) && near(hit.mMagicDistance,16),
                "Explosion direction changed with a later victim position or caster aim");
        }
    }

    void testSavedItemRotation()
    {
        for (int i=0; i<1000; ++i)
        {
            const float angles[3] = { i*0.17f, i*0.37f, i*0.61f };
            const auto before = Misc::Convert::makeOsgQuat(angles);
            const auto euler = Misc::toEulerAnglesZYX(before);
            const float saved[3] = { euler.x(), euler.y(), euler.z() };
            const auto after = Misc::Convert::makeOsgQuat(saved);
            const double dot = before.x()*after.x()+before.y()*after.y()+before.z()*after.z()+before.w()*after.w();
            check(std::abs(dot)>0.9999, "Native saved Euler angles changed a physical item's orientation");
        }
    }

    void testDelayedEffectsDoNotReuseExpiredContact()
    {
        MWMechanics::RagdollHit hit;
        hit.mWeapon.mWeaponDistance = 2;
        hit.mWeapon.mCrossbow = true;
        hit.mWeapon.mWeaponDirection = osg::Vec3f(0, 1, 0);
        hit.mAwaitingWeapon = true;
        hit.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 5, Element::Shock, 50));
        hit.beginMagicTick();
        check(hit.mApplied.mShock && near(hit.mApplied.mDominantMagicDamage, 50),
            "Magic bookkeeping lost an enchantment before asynchronous weapon damage arrived");
        hit.applyWeapon();
        hit.mAwaitingWeapon = false;
        hit.beginMagicTick();
        hit.applyMagic(2, magic(osg::Vec3f(0, 1, 0), 3, Element::Poison, 2));
        check(hit.mApplied.mDeathElement == Element::Poison && near(hit.mApplied.mDominantMagicDamage, 2)
                && !hit.mApplied.mShock && !hit.mApplied.mCrossbow && near(hit.mApplied.mWeaponDistance, 0),
            "A later duration tick reused an expired element or the original weapon blow");
        check(near(hit.mApplied.mMagicDistance, 3), "A delayed effect accumulated the old contact impulse");
    }
}

int main()
{
    try
    {
        testDominantDamageSelectsMotionAndElement();
        testOnlyEffectiveDamageContributes();
        testDistinctEffectsAndDurationTicks();
        testWeaponAndMagicOrder();
        testEqualEffectsAndContactIsolation();
        testExplosionDistanceAndDirection();
        testRadiusBoundsAndHealthMagic();
        testPoisonAttractionAndMeleeMultipliers();
        testDelayedEffectsDoNotReuseExpiredContact();
        testRestoredEffectPresentationHasNoInventedImpulse();
        testExplosionWaveGeometry();
        testSavedItemRotation();
        std::cout << "Ragdoll impact: 12 groups passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
