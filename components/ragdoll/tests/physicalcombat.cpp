#include <components/ragdoll/physicalcombat.hpp>
#include <components/sceneutil/hitlocation.hpp>
#include <iostream>
#include <limits>
#include <osg/Group>
#include <stdexcept>

using namespace PhysicalCombat;
namespace
{
    void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    void near(float a, float b, const char* message) { check(std::abs(a - b) < .0001f, message); }
    void timingAndBalance()
    {
        for (const auto handling : { Handling::Heavy, Handling::LightBlade, Handling::Mixed })
        {
            Weapon weapon{ 35, 100, 1, handling };
            Actor novice{ 30, 30, 10, 100 }, expert{ 100, 100, 100, 100 };
            const auto low = timing(weapon, novice), high = timing(weapon, expert);
            check(low.mWindup >= .72f && low.mStrike >= .95f, "Novice attacks became unrealistically slow");
            check(high.mWindup > low.mWindup && high.mStrike > low.mStrike, "Stats and matching skill do not accelerate attacks");
            check(high.mWindup <= 1.75f && high.mStrike <= 1.85f && high.mMinimumCycle >= .36f,
                "Expert attack cadence exceeds the balance limits");
            expert.mFatigue = 0;
            const auto tired = timing(weapon, expert);
            near(tired.mWindup * 2, high.mWindup, "Exhausted windup is not exactly half speed");
            near(tired.mStrike * 2, high.mStrike, "Exhausted strike is not exactly half speed");
            near(tired.mRecovery * 2, high.mRecovery, "Exhausted recovery is not exactly half speed");
            near(tired.mMinimumCycle, high.mMinimumCycle * 2, "Exhaustion bypasses the cadence limit");
        }
        Weapon heavy{ 25, 80, 1, Handling::Heavy };
        check(timing(heavy, { 100, 30, 30, 1 }).mWindup > timing(heavy, { 30, 100, 30, 1 }).mWindup,
            "Heavy weapons do not prioritize strength");
        const Weapon hammer{ 50, 140, 1, Handling::Heavy };
        check(timing(hammer, { 100, 40, 30, 1 }).mWindup > timing(hammer, { 40, 40, 30, 1 }).mWindup
            && timing(hammer, { 100, 40, 30, 1 }).mStrike > timing(hammer, { 40, 40, 30, 1 }).mStrike,
            "The slow-speed floor swallowed the strength bonus on typical heavy weapons");
        Weapon light{ 12, 65, 1, Handling::LightBlade };
        check(timing(light, { 30, 100, 30, 1 }).mWindup > timing(light, { 100, 30, 30, 1 }).mWindup,
            "Light blades do not prioritize agility");
        Weapon mixed{ 20, 70, 1, Handling::Mixed };
        near(timing(mixed, { 100, 20, 30, 1 }).mStrike, timing(mixed, { 20, 100, 30, 1 }).mStrike,
            "Mixed weapons do not use equal strength/agility shares");
        check(timing({ 35, 100, 1, Handling::Mixed }, {}).mWindup < timing(mixed, {}).mWindup,
            "Weapon weight/length do not slow acceleration");
        const float bad = std::numeric_limits<float>::quiet_NaN();
        const auto malformed = timing({ bad, bad, bad, Handling::Heavy }, { bad, bad, bad, 1 });
        check(std::isfinite(malformed.mStrike) && std::isfinite(malformed.mWindup), "Malformed records poisoned animation time");
        check(skillDamage(100) > skillDamage(10) && skillDamage(10) > .5f, "Weapon skill does not affect damage sensibly");
        check(speedDamage(900, 500) > speedDamage(300, 500), "Impact speed does not affect damage");
        check(speedDamage(1900, 500) > speedDamage(1300, 500) && speedDamage(1300, 500) > speedDamage(1000, 500),
            "The damage limit swallowed running/falling bonuses on an expert swing");
        check(impulse(heavy, 900) > impulse(light, 300), "Heavy fast strikes do not transfer greater momentum");
        std::cout << "PASS weapon handling, 50/50 attributes, additive skill, bounded novice/expert cadence and exact exhaustion\n";
    }
    void guards()
    {
        for (bool tired : { false, true })
        for (float skill : { 0.f, 50.f, 100.f })
        {
            Guard guard;
            guard.mRequested = true;
            float spent = 0;
            const float duration = raiseTime(skill, tired);
            for (int i = 0; i < 100; ++i) spent += guard.update(duration / 100, skill, tired, true);
            check(guard.mProgress > .999f, "Shield did not finish raising at its skill/fatigue deadline");
            near(spent, 7 - skill * .05f, "Holding a shield charged the raise cost more than once");
            near(guard.damageMultiplier(tired), tired ? .5f : 0, "Timed block has incorrect damage transmission");
            for (int i = 0; i < 30; ++i) guard.update(.01f, skill, tired, true);
            near(guard.damageMultiplier(tired), tired ? .9f : .5f, "Early block has incorrect damage transmission");
            guard.mRequested = false; guard.update(.1f, skill, tired, true);
            near(guard.damageMultiplier(tired), 1, "Released shield still blocked damage");
            guard.mRequested = true;
            guard.update(.01f, skill, tired, false);
            near(guard.damageMultiplier(tired), 1, "Unequipped/unavailable shield still blocked damage");
        }
        near(raiseTime(50, true), raiseTime(50, false) * 2, "Exhaustion does not double shield raise time");
        check(shieldWear(50, 100) < shieldWear(50, 10) && shieldWear(0, 100) == 0,
            "Block skill does not reduce actual shield wear");
        check(shieldFatigue(50, 20, 1, 100) < shieldFatigue(50, 20, 1, 10), "Block skill does not reduce stamina cost");
        Guard single; single.mRequested = true;
        for (int i = 0; i < 30; ++i) single.update(.01f, 0, false, true);
        near(single.damageMultiplier(false), 0, "A second contact inside the same timing window bypassed a raised shield");
        single.update(.1f, 0, false, true); single.update(.1f, 0, false, true); single.update(.1f, 0, false, true);
        near(single.damageMultiplier(false), .5f, "Holding the shield renewed its perfect-block window");
        std::cout << "PASS timed/early shield transmission 0/50% and exhausted 50/90%, raise time, one-time cost and release\n";
    }
    void motionAndContacts()
    {
        const float fistRange = approachRange(8, 64, 16);
        const float daggerRange = approachRange(25, 64, 16);
        const float swordRange = approachRange(70, 64, 16);
        const float spearRange = approachRange(110, 64, 16);
        check(fistRange < daggerRange && daggerRange < swordRange && swordRange < spearRange,
            "AI approach distance does not follow the physical grip-to-tip reach");
        check(swordRange + 16 + 8 < 38.4f + 70 && daggerRange < 40 && spearRange < 100,
            "AI stops outside the arm/blade contact envelope");
        check(approachRange(0, 0, 100) == 4, "Short limbs can give AI a negative attack range");
        State lightMotor, heavyMotor;
        Weapon light{ 5, 50, 1, Handling::LightBlade }, heavy{ 50, 140, 1, Handling::Heavy };
        for (int frame = 0; frame < 120; ++frame)
        {
            const float angle = frame / 120.f;
            const Blade target{ {}, { std::sin(angle) * 70, std::cos(angle) * 70, 0 } };
            lightMotor.driveWeapon(target, 1.f / 120, light, 40);
            heavyMotor.driveWeapon(target, 1.f / 120, heavy, 40);
        }
        check(heavyMotor.mWeaponLag.length() > lightMotor.mWeaponLag.length() * 2,
            "Weapon mass and length do not produce actual procedural inertia");
        for (int frame = 0; frame < 600; ++frame)
            heavyMotor.driveWeapon({ {}, heavyMotor.mLastBladeDirection * 70 }, 1.f / 120, heavy, 40);
        check(heavyMotor.mWeaponLag.length() < .001f && heavyMotor.mWeaponLagVelocity.length() < .001f,
            "The living weapon motor did not settle after the swing");
        const Blade before{ { -20, -30, 0 }, { 20, -30, 0 } }, after{ { -20, 30, 0 }, { 20, 30, 0 } };
        const Blade rotationStart{ {}, { 0, 100, 0 } }, rotationEnd{ {}, { 100, 0, 0 } };
        check(pointVelocity(rotationStart, rotationEnd, 1, .1f).length()
            > pointVelocity(rotationStart, rotationEnd, .1f, .1f).length() * 9,
            "A hilt contact incorrectly uses tip speed");
        const auto charging = pointVelocity(before, after, .5f, .1f);
        check((charging - osg::Vec3f(0, 600, 0)).length() < .01f,
            "Moving with the victim invented relative impact speed");
        const Blade falling{ before.mBase - osg::Vec3f(0, 0, 60), before.mTip - osg::Vec3f(0, 0, 60) };
        check(pointVelocity(before, falling, .5f, .1f).z() < -599,
            "Downward actor movement did not enter the weapon contact velocity");
        bool middle = false, tip = false;
        unsigned rays = 0;
        sweep(before, after, [&](const auto& a, const auto& b, float, float, float) {
            ++rays;
            if (a.y() >= 0 || b.y() <= 0) return;
            const auto contact = a + (b - a) * (-a.y() / (b.y() - a.y()));
            middle |= std::abs(contact.x()) < 3;
            tip |= contact.x() > 17;
        });
        check(middle && tip && rays <= 29, "Bounded blade sweep misses the shaft/tip or has unbounded cost");
        State reaction;
        check(!reaction.receive({ 0, 1, 0 }, 1000, 70, 70, 1), "A weak hit knocked a healthy actor down");
        check(reaction.mPush.y() > 0 && reaction.mLeanVelocity.x() > 0, "Contact did not produce directional recoil");
        check(reaction.receive({ 0, 1, 0 }, 10000, 70, 30, 1), "A heavy blow did not cause loss of balance");
        for (int i = 0; i < 600; ++i) reaction.advance(1.f / 60);
        check(reaction.mPush.length() < .001f && reaction.mLean.length() < .001f, "Physical reaction failed to settle");
        osg::ref_ptr<osg::Group> shield = new osg::Group;
        shield->setUserValue(SceneUtil::HitBodyTag, -2);
        shield->setUserValue(SceneUtil::ShieldContactTag, true);
        check(SceneUtil::isShieldContact({ shield }) && SceneUtil::getHitBody({ shield }) == -2,
            "Shield contacts are confused with anatomical cuts/weapon clashes");
        std::cout << "PASS blade shaft/tip sweeps, bounded work, directional impulse, deterministic knockdown and settling\n";
    }
}
int main()
{
    try { timingAndBalance(); guards(); motionAndContacts(); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
