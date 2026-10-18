#include <apps/openmw/mwmechanics/ragdollimpact.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
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

    Ragdoll::Impact magic(const osg::Vec3f& direction, float distance, bool shock = false)
    {
        Ragdoll::Impact result;
        result.mMagicDirection = direction;
        result.mMagicDistance = distance;
        result.mPosition = osg::Vec3f(10, 20, 30);
        result.mShock = shock;
        result.mValid = true;
        return result;
    }

    void testDistinctEffectsAndDurationTicks()
    {
        MWMechanics::RagdollHit hit;
        hit.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 6));
        for (int tick = 0; tick < 600; ++tick)
            hit.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 6));
        check(near(hit.mApplied.mMagicDistance, 6), "Duration ticks multiplied one effect's impulse");
        check(near(hit.mApplied.mMagicDirection, osg::Vec3f(1, 0, 0)), "Duration changed the impact direction");

        hit.applyMagic(2, magic(osg::Vec3f(0, 1, 0), 8, true));
        check(near(hit.mApplied.mMagicDistance, 8), "A multi-effect spell exceeded its strongest throw");
        check(near(hit.mApplied.mMagicDirection, osg::Vec3f(0, 1, 0)),
            "The strongest spell effect lost its individual direction");
        check(hit.mApplied.mShock, "Applied shock did not contribute to the contact");

        hit.applyMagic(2, magic(osg::Vec3f(0, 1, 0), 8, false));
        check(!hit.mApplied.mShock, "Replacing a contribution retained stale shock state");
        check(near(hit.mApplied.mMagicDistance, 8), "Replacing a contribution added another impulse");
        hit.applyMagic(3, magic(osg::Vec3f(1, 0, 0), 2, true));
        check(hit.mApplied.mShock && near(hit.mApplied.mMagicDistance, 8),
            "A weaker applied shock effect lost convulsions or inflated the blast");
    }

    void testWeaponAndMagicOrder()
    {
        MWMechanics::RagdollHit spellFirst;
        spellFirst.mWeapon.mWeaponDirection = osg::Vec3f(0, 1, 0);
        spellFirst.mWeapon.mWeaponDistance = 2;
        spellFirst.mWeapon.mCrossbow = true;
        spellFirst.mWeapon.mPosition = osg::Vec3f(4, 5, 6);
        spellFirst.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 5, true));
        spellFirst.applyWeapon();

        MWMechanics::RagdollHit weaponFirst;
        weaponFirst.mWeapon = spellFirst.mWeapon;
        weaponFirst.applyWeapon();
        weaponFirst.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 5, true));
        for (const auto* hit : { &spellFirst, &weaponFirst })
        {
            const auto& applied = hit->mApplied;
            check(applied.mValid && applied.mCrossbow && applied.mShock,
                "Weapon and magic ordering dropped contact flags");
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
        first.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 5));
        first.applyMagic(2, magic(osg::Vec3f(-1, 0, 0), 5));
        check(near(first.mApplied.mMagicDistance, 5), "Equal-area effects multiplied or cancelled the spell's throw");
        check(near(first.mApplied.mMagicDirection.length(), 1), "Equal-area effects have an invalid direction");

        MWMechanics::RagdollHit second;
        second.applyMagic(1, magic(osg::Vec3f(0, 1, 0), 2, true));
        check(near(first.mApplied.mMagicDistance, 5) && !first.mApplied.mShock,
            "An unrelated contact altered an earlier contact");
        check(near(second.mApplied.mMagicDistance, 2) && second.mApplied.mShock,
            "Independent effect identifiers collided across contacts");
    }

    void testExplosionDistanceAndDirection()
    {
        MWMechanics::RagdollMagicImpact context;
        context.mOrigin = osg::Vec3f(100, -200, 30);
        context.mDirection = osg::Vec3f(0, 0, -3);
        context.mRadius = 10;
        context.mExploded = true;
        const auto impact = MWMechanics::makeRagdollMagicImpact(context,
            context.mOrigin + osg::Vec3f(4 * Constants::UnitsPerMeter, 0, 0), 2, 1, false);
        check(near(impact.mMagicDistance, 6), "10 metre blast at 4 metres did not produce a 6 metre throw");
        check(near(impact.mMagicDirection, osg::Vec3f(1, 0, 0)),
            "Indirect explosion used the projectile direction instead of the epicentre");
        check(near(impact.mPosition, context.mOrigin), "Explosion lost its original world-space epicentre");

        const auto vertical = MWMechanics::makeRagdollMagicImpact(context,
            context.mOrigin + osg::Vec3f(0, 0, 4 * Constants::UnitsPerMeter), 2, 1, true);
        check(near(vertical.mMagicDirection, osg::Vec3f(0, 0, 1)) && near(vertical.mMagicDistance, 6),
            "Explosion direction discarded vertical separation");
        check(vertical.mShock, "Lethal shock was not marked for convulsions");

        const auto edge = MWMechanics::makeRagdollMagicImpact(context,
            context.mOrigin + osg::Vec3f(9 * Constants::UnitsPerMeter, 0, 0), 2, 1, false);
        check(near(edge.mMagicDistance, 2), "Magic minimum throw was not respected near the blast edge");

        context.mExploded = false;
        context.mRadius = 0;
        const auto direct = MWMechanics::makeRagdollMagicImpact(context, osg::Vec3f(900, 800, 700), 2, 1, false);
        check(near(direct.mMagicDirection, osg::Vec3f(0, 0, -1)) && near(direct.mMagicDistance, 2),
            "Direct projectile lost its incoming direction or minimum throw");

        context.mExploded = true;
        context.mDirection = osg::Vec3f();
        const auto atCentre = MWMechanics::makeRagdollMagicImpact(context, context.mOrigin, 2, 1, false);
        check(near(atCentre.mMagicDirection.length(), 1) && near(atCentre.mMagicDistance, 2),
            "Coincident explosion and victim produced an invalid direction");
    }

    void testDelayedEffectsDoNotReuseExpiredContact()
    {
        MWMechanics::RagdollHit hit;
        hit.mWeapon.mWeaponDistance = 2;
        hit.mWeapon.mCrossbow = true;
        hit.mWeapon.mWeaponDirection = osg::Vec3f(0, 1, 0);
        hit.mAwaitingWeapon = true;
        hit.applyMagic(1, magic(osg::Vec3f(1, 0, 0), 5, true));
        hit.beginMagicTick();
        check(hit.mApplied.mShock && near(hit.mApplied.mMagicDistance, 5),
            "Magic bookkeeping lost an enchantment before asynchronous weapon damage arrived");
        hit.applyWeapon();
        hit.mAwaitingWeapon = false;
        hit.beginMagicTick();
        hit.applyMagic(2, magic(osg::Vec3f(0, 1, 0), 3));
        check(!hit.mApplied.mShock && !hit.mApplied.mCrossbow && near(hit.mApplied.mWeaponDistance, 0),
            "A later duration tick reused expired shock or the original weapon blow");
        check(near(hit.mApplied.mMagicDistance, 3), "A delayed effect accumulated the old contact impulse");
    }
}

int main()
{
    try
    {
        testDistinctEffectsAndDurationTicks();
        testWeaponAndMagicOrder();
        testEqualEffectsAndContactIsolation();
        testExplosionDistanceAndDirection();
        testDelayedEffectsDoNotReuseExpiredContact();
        std::cout << "Ragdoll impact: 5 groups passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
