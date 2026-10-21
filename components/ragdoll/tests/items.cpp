#include "../items.hpp"
#include <btBulletDynamicsCommon.h>
#include <iostream>
#include <stdexcept>

namespace
{
    void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    btTransform at(btScalar x, btScalar y, btScalar z)
    { auto t = btTransform::getIdentity(); t.setOrigin(btVector3(x,y,z)); return t; }
    Ragdoll::StaticCollider floor()
    {
        Ragdoll::StaticCollider c;
        c.mShape = std::make_shared<btStaticPlaneShape>(btVector3(0,0,1), 0);
        return c;
    }
    void massAndRotation()
    {
        Ragdoll::ItemSimulation sim;
        btBoxShape box(btVector3(2,3,5));
        sim.add(1, box, at(0,0,100), 1);
        sim.add(2, box, at(100,0,100), 20);
        sim.add(3, box, at(0,10,100), 1);
        sim.add(1, box, at(0,0,100), 1); // Scene insertion also applies the initial scale.
        check(!sim.hasActiveBodies(), "Authored shelf contents moved before an impact");
        sim.impulse(1, btVector3(200,0,0), btVector3(0,0,103));
        sim.impulse(2, btVector3(200,0,0), btVector3(100,0,103));
        check(btFabs(sim.velocity(1).x()/sim.velocity(2).x()-20) < 1e-6, "Mass did not affect the same momentum");
        const auto poses = sim.advance(1.0/60);
        check(poses.size() == 2 && poses.front().mTransform.getRotation().getAngle() > 0.001,
            "Off-centre hit must rotate an item");
        check(sim.advance(0).empty(), "Paused simulation advanced");
        std::cout << "PASS item masses, resting placement, off-centre impulse and pause\n";
    }
    void shelfFloorAndWake()
    {
        Ragdoll::ItemSimulation sim;
        btBoxShape box(btVector3(2,2,2));
        auto shelf = floor();
        shelf.mShape = std::make_shared<btBoxShape>(btVector3(12,12,1));
        shelf.mTransform = at(0,0,40);
        sim.replaceStaticColliders({ floor(), shelf });
        sim.add(1, box, at(0,0,43.1), 1);
        sim.impulse(1, btVector3(180,0,20), btVector3(0,0,43));
        btTransform final = at(0,0,43);
        for (int i=0; i<1200; ++i)
            for (const auto& p : sim.advance(1.0/120)) if (p.mId == 1) final = p.mTransform;
        check(final.getOrigin().x()>15 && final.getOrigin().z()>1.5 && final.getOrigin().z()<5,
            "An item failed to leave its shelf or fell through the floor");
        sim.impulse(1, btVector3(0,100,120), final.getOrigin());
        check(sim.hasActiveBodies() && sim.velocity(1).z()>100, "A settled item could not be hit again");
        sim.remove(1);
        check(!sim.contains(1) && sim.advance(0.1).empty(), "Picked-up item retained a ghost body");
        std::cout << "PASS shelf fall, floor collision, wake and pickup\n";
    }
    void itemContactsAndThinWalls()
    {
        for (const double dt : { 1.0/30, 1.0/60, 1.0/120 })
        {
            Ragdoll::ItemSimulation sim;
            btBoxShape box(btVector3(2,2,2));
            sim.replaceStaticColliders({ floor() });
            sim.add(1, box, at(-12,0,2.1), 1);
            sim.add(2, box, at(0,0,2.1), 1);
            sim.impulse(1, btVector3(120,0,0), btVector3(-12,0,2));
            bool woke = false;
            for (int i=0; i<int(1/dt); ++i)
            {
                sim.advance(dt);
                woke |= sim.velocity(2).x() > 10;
            }
            check(woke, "Moving clutter failed to wake/push another item");
            auto wall = floor();
            wall.mShape = std::make_shared<btBoxShape>(btVector3(.1,100,100));
            sim.replaceStaticColliders({ floor(), wall });
            sim.remove(2);
            sim.move(1, at(-12,0,20));
            sim.impulse(1, btVector3(5000,0,0), btVector3(-12,0,20), 3);
            for (int i=0; i<int(0.5/dt); ++i)
                for (const auto& p : sim.advance(dt)) check(p.mTransform.getOrigin().x()<0, "Fast item tunneled through wall");
        }
        std::cout << "PASS item-to-item contacts and thin wall CCD at 30/60/120 FPS\n";
    }

    void deadlinesAndRestore()
    {
        Ragdoll::ItemSimulation sim;
        btBoxShape box(btVector3(2, 2, 2));
        sim.add(1, box, at(0, 0, 100000), 1);
        sim.add(2, box, at(100, 0, 100000), 1);
        sim.wake(1);
        for (int i = 0; i < 240; ++i)
            sim.advance(1.0 / 120);
        sim.wake(2);
        for (int i = 0; i < 720; ++i)
            sim.advance(1.0 / 120);
        sim.impulse(1, btVector3(100, 0, 0), sim.state(1).mTransform.getOrigin());
        check(btFabs(sim.state(1).mTimeLeft - 2) < 1e-5, "hit during activity restarted the deadline");
        const auto saved = sim.state(2);
        const auto paused = sim.state(1);
        sim.advance(0);
        check(sim.state(1).mTimeLeft == paused.mTimeLeft, "pause consumed the item's timer");
        for (int i = 0; i < 240; ++i)
            sim.advance(1.0 / 120);
        check(!sim.state(1).mActive && sim.velocity(1).fuzzyZero() && sim.state(2).mActive,
            "individual ten second deadlines did not stop the first moving item alone");
        const auto frozen = sim.state(1).mTransform;
        sim.advance(0.1);
        check(sim.state(1).mTransform == frozen, "frozen item continued to fall");
        sim.impulse(1, btVector3(120, 0, 0), frozen.getOrigin());
        check(sim.state(1).mActive && btFabs(sim.state(1).mTimeLeft - 10) < 1e-5,
            "new external impact failed to start a fresh window");

        Ragdoll::ItemSimulation restored;
        restored.add(2, box, saved.mTransform, 1);
        restored.restore(2, saved);
        check(restored.state(2).mTransform == saved.mTransform && restored.velocity(2) == saved.mLinearVelocity,
            "restore lost transform or velocity");
        for (int i = 0; i < 480; ++i)
            restored.advance(1.0 / 120);
        check(!restored.state(2).mActive,
            "restore restarted the complete lifetime instead of the remaining four seconds");
        std::cout << "PASS independent deadlines, pause, new impacts and remaining-time restore\n";
    }

    void boundedActivation()
    {
        Ragdoll::ItemSimulation sim({ 2, 10 });
        sim.setObserverPosition(btVector3(0, 0, 100000));
        btBoxShape box(btVector3(2, 2, 2));
        sim.add(1, box, at(1000, 0, 100000), 1);
        sim.add(2, box, at(10, 0, 100000), 1);
        sim.add(3, box, at(20, 0, 100000), 1);
        sim.wake(1);
        sim.wake(2);
        sim.wake(3);
        check(sim.activeCount() == 2 && !sim.state(1).mActive && sim.state(2).mActive && sim.state(3).mActive,
            "overflow did not freeze the distant old item and admit the new one");
        check(sim.contains(1) && sim.velocity(1).fuzzyZero(), "eviction deleted an item or retained velocity");

        Ragdoll::ItemSimulation hundred;
        for (int id = 1; id <= 101; ++id)
        {
            hundred.add(id, box, at(id * 20, 0, 100000), 1);
            hundred.wake(id);
            check(hundred.activeCount() <= 100, "101st activation exceeded the hard item budget");
        }
        check(hundred.activeCount() == 100 && hundred.contains(100), "budget silently removed world items");
        std::cout << "PASS item budgets, overflow priority and 101st activation\n";
    }

    void actorContact()
    {
        Ragdoll::ItemSimulation sim;
        btBoxShape box(btVector3(2, 2, 2));
        btCapsuleShape actor(3, 8);
        sim.add(1, box, at(0, 0, 30), 1);
        sim.pushKinematic(actor, at(-10, 0, 30), at(0, 0, 30), 0.1);
        check(sim.state(1).mActive && sim.velocity(1).x() > 0,
            "resolved actor movement failed to wake/push a placed item");
        const auto before = sim.state(1).mTimeLeft;
        sim.advance(0.1);
        check(sim.state(1).mTimeLeft < before, "actor contact did not retain a finite independent deadline");
        std::cout << "PASS kinematic actor-to-item contact\n";
    }
    void scaledImpulses()
    {
        btBoxShape box(btVector3(1, 1, 1));
        for (const btScalar mass : { btScalar(.05), btScalar(1), btScalar(20) })
            for (const btScalar strength : { btScalar(500), btScalar(5000) })
                for (const btScalar scale : { btScalar(.5), btScalar(1), btScalar(3) })
                {
                    Ragdoll::ItemSimulation sim;
                    sim.add(1, box, at(0, 0, 10000), mass);
                    sim.impulse(1, btVector3(strength, 0, 0), btVector3(0, 0, 10000), scale);
                    const auto expected = std::min(btScalar(1200), strength / mass) * scale;
                    check(std::abs(sim.velocity(1).x() - expected) < .001,
                        "Item slider was erased by the baseline speed cap");
                    const auto start = sim.state(1).mTransform.getOrigin().x();
                    sim.advance(1.0 / 120);
                    const auto travel = sim.state(1).mTransform.getOrigin().x() - start;
                    check(travel > expected / 120 * .99 && travel < expected / 120 * 1.01,
                        "Actual item motion did not follow its scaled impulse");
                    sim.add(2, box, at(100, 0, 10000), mass);
                    sim.impulse(2, btVector3(strength, 0, 0), btVector3(100, 0, 10010), scale);
                    check(std::abs(sim.state(2).mAngularVelocity.length() - 25 * scale) < .001,
                        "Off-centre item spin did not scale after its baseline angular cap");
                }
        std::cout << "PASS physical item 0.5x/1x/3x impulses and travel for light/heavy/saturated hits\n";
    }

    void liveSettingsAndTripleBudget()
    {
        Ragdoll::ItemSimulation sim({ 300, 10 });
        btBoxShape box(btVector3(2, 2, 2));
        for (std::uintptr_t id = 1; id <= 300; ++id)
        {
            sim.add(id, box, at(id * 10, 0, 1000), 1);
            sim.wake(id);
        }
        check(sim.activeCount() == 300, "UI +200% item budget was still capped at 100");
        sim.elapseWithoutPhysics(2);
        sim.setSettings({ 50, 30 });
        check(sim.activeCount() == 50 && sim.contains(1), "live budget reduction removed items or exceeded its limit");
        std::uintptr_t active = 0;
        for (std::uintptr_t id = 1; id <= 300; ++id)
            if (sim.state(id).mActive)
                active = id;
        check(active != 0 && std::abs(sim.state(active).mTimeLeft - 28) < .001,
            "live duration adjustment renewed an item window instead of preserving age");
        sim.setSettings({ 300, 1 });
        check(sim.activeCount() == 0, "shortening duration below elapsed time did not freeze items");
        std::cout << "PASS 300 items, live budget shrink and duration changes without restarting timers\n";
    }
}
int main()
{
    try
    {
        massAndRotation();
        shelfFloorAndWake();
        itemContactsAndThinWalls();
        deadlinesAndRestore();
        boundedActivation();
        actorContact();
        scaledImpulses();
        liveSettingsAndTripleBudget();
    }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
