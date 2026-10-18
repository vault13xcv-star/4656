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
            sim.impulse(1, btVector3(5000,0,0), btVector3(-12,0,20));
            for (int i=0; i<int(0.5/dt); ++i)
                for (const auto& p : sim.advance(dt)) check(p.mTransform.getOrigin().x()<0, "Fast item tunneled through wall");
        }
        std::cout << "PASS item-to-item contacts and thin wall CCD at 30/60/120 FPS\n";
    }
}
int main()
{
    try { massAndRotation(); shelfFloorAndWake(); itemContactsAndThinWalls(); }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
