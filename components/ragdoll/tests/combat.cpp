#include <components/ragdoll/combatbalance.hpp>
#include <components/ragdoll/meleecontact.hpp>

#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool condition, const char* message)
    {
        if(!condition)throw std::runtime_error(message);
    }
    bool near(float a,float b,float tolerance=.0001f){return std::abs(a-b)<tolerance;}
    osg::ref_ptr<osg::Geode> box(const osg::Vec3f& center,const osg::Vec3f& size)
    {
        osg::ref_ptr<osg::Geode> geode=new osg::Geode;
        geode->addDrawable(new osg::ShapeDrawable(new osg::Box(center,size.x(),size.y(),size.z())));
        return geode;
    }
    void balance()
    {
        using namespace Ragdoll::Combat;
        require(damage(0,false)>=.30f && near(damage(100,false),1),"Low skill or mastery damage is wrong");
        require(near(wear(100),.5f) && near(resource(100),.5f),"Mastery does not halve wear/stamina");
        require(wear(0)<=1.30f && resource(0)<=1.25f,"Beginning characters pay excessive resource costs");
        for(int skill=0;skill<=100;++skill)
        for(int agility:{0,40,100})
        {
            require(near(damage(skill,true),damage(skill,false)*.5f),"Exhaustion does not halve damage");
            require(near(preparation(skill,agility,true),preparation(skill,agility,false)*1.5f)
                && near(recovery(skill,agility,true),recovery(skill,agility,false)*1.5f),
                "Exhaustion does not increase preparation/recovery duration by exactly 50 percent");
            require(preparation(skill,agility,false)>=1 && recovery(skill,agility,false)>=1,
                "Skill/agility creates attacks faster than the original mastery speed");
            require(skill==100 || (damage(skill+1,false)>damage(skill,false)
                && preparation(skill+1,agility,false)<preparation(skill,agility,false)
                && recovery(skill+1,agility,false)<recovery(skill,agility,false)),"Skill progression is not monotonic");
        }
        require(near(preparation(100,0,false),1) && near(recovery(100,100,false),1),"Mastery is not original tempo");
        // Representative full-fatigue skill30/agility40/luck40 attacker against
        // an agility20/luck20 mudcrab: original expected contact damage is .45.
        // Weight a cycle as 50% wind-up, 20% strike, 30% recovery.
        const float cycle=.5f*preparation(30,40,false)+.2f+.3f*recovery(30,40,false);
        require(std::abs(damage(30,false)/cycle/.45f-1)<.10f,"Early-game time-to-kill exceeds the calibration band");
        const float staminaPerFight=.45f/damage(30,false)*resource(30);
        const float wearPerFight=.45f/damage(30,false)*wear(30);
        require(staminaPerFight>.8f && staminaPerFight<1.2f && wearPerFight>.8f && wearPerFight<1.2f,
            "Early-game stamina/wear per fight exceeds the calibration band");
        float carry=0;int loss=0;
        for(int hit=0;hit<1000;++hit)loss+=consumeWear(wear(100),carry);
        require(loss==500 && carry==0,"Mastery loses fractional durability savings");
        std::cout<<"PASS balance endpoints, monotonic stats, exact exhaustion and representative early-game effort\n";
    }
    void defense()
    {
        using namespace Ragdoll::Combat;
        State guard;
        guard.mGuardHeld=true;guard.mGuardStarted=10.1;guard.mGuardReady=10.2;
        require(blockedFraction(guard,10,10.15)==0,"Shield blocked before it was raised");
        require(blockedFraction(guard,10,10.3)==1,"Timely shield does not fully block");
        require(near(blockedFraction(guard,10.5,10.8),.3f),"Holding shield fully blocks the next attack");
        guard.mGuardHeld=false;
        require(blockedFraction(guard,10,10.3)==0,"Lowered shield blocks damage");
        require(guardRaise(100)<guardRaise(0) && dodgeCost(100)<dodgeCost(0)
            && dodgeCooldown(100)<dodgeCooldown(0),"Agility does not improve active defenses");
        for(auto input:{osg::Vec3f(0,-1,0),osg::Vec3f(1,0,0),osg::Vec3f(-1,-1,0),osg::Vec3f(1,1,1),osg::Vec3f()})
        for(int fps:{15,30,60,144})
        {
            State dash;dash.mDodgeDirection=dodgeDirection(input);dash.mDodgeLeft=.24f;
            osg::Vec3f distance;
            for(int frame=0;frame<fps;++frame)distance+=dodgeVelocity(dash,1.f/fps)/float(fps);
            require(near(distance.length(),160,.002f) && distance.y()<=0 && distance.z()==0,
                "Dodge distance depends on framerate, goes forward, or adds vertical velocity");
        }
        std::cout<<"PASS timed/early/held/lowered shield and frame-independent backward/lateral dodges\n";
    }
    void ranged()
    {
        using namespace Ragdoll::Combat;
        const osg::Vec3f original(0,800,250);
        for(int agility:{0,40,100})
        for(int skill:{0,30,100})
        for(int i=0;i<=20;++i)
        {
            const auto result=spreadShot(original,skill,agility,i/20.f,1.f-i/20.f);
            require(near(result.z(),original.z()) && near(result.length(),original.length(),.001f),
                "Ranged spread changes vertical aim or launch energy");
            const float angle=std::abs(std::atan2(result.x(),result.y())*180/3.14159265f);
            require(angle<=spread(skill,agility)+.0001f,"Horizontal spread exceeds the skill-dependent bound");
            if(skill==100)require((original-result).length2()==0,"Mastery still deviates from aim");
        }
        require(spread(0,0)<=1.5f && spread(30,100)<spread(30,0),"Low-skill spread is extreme or ignores agility");
        for(float speed:{500,1000,1500})
        for(float height:{-40,0,40})
        {
            const osg::Vec3f target(30,100,height);
            auto velocity=ballisticAim(target,speed);velocity.normalize();velocity*=speed;
            const float time=std::hypot(target.x(),target.y())/std::hypot(velocity.x(),velocity.y());
            const auto impact=velocity*time-osg::Vec3f(0,0,.5f*627.2f*time*time);
            require((target-impact).length()<.002f,"NPC low-arc aim misses a reachable physical projectile target");
        }
        require(std::isfinite(ballisticAim({0,10000,0},100).length2()),"Unreachable ballistic aim became nonfinite");
        std::cout<<"PASS horizontal-only bounded spread, zero mastery spread and full-gravity ballistic aim\n";
    }
    void blade()
    {
        using namespace Ragdoll::Combat;
        osg::ref_ptr<osg::Group> root=new osg::Group;
        osg::ref_ptr<osg::MatrixTransform> parent=new osg::MatrixTransform;
        parent->setMatrix(osg::Matrixf::translate(100,20,30));root->addChild(parent);
        osg::ref_ptr<osg::MatrixTransform> weapon=new osg::MatrixTransform;
        weapon->setMatrix(osg::Matrixf::translate(0,5,0));parent->addChild(weapon);
        weapon->addChild(box({}, {2,60,2}));
        osg::Vec3f base,tip;
        require(weaponSegment(*weapon,{100,20,30},base,tip),"Attached weapon has no usable segment");
        require((base-osg::Vec3f(100,-5,30)).length()<.001f && (tip-osg::Vec3f(100,55,30)).length()<.001f,
            "Weapon segment lost/doubled its own or parent transform");
        require(!weaponSegment(*weapon,{2000,0,0},base,tip),"First-person coordinate mismatch is accepted as a remote blade");
        auto target=box({0,10,0},{3,3,3});
        unsigned rays=0;
        const auto ray=[&](const osg::Vec3f& from,const osg::Vec3f& to)
        {
            ++rays;
            osg::ref_ptr<osgUtil::LineSegmentIntersector> query=new osgUtil::LineSegmentIntersector(from,to);
            osgUtil::IntersectionVisitor visitor(query);target->accept(visitor);
            return query->containsIntersections();
        };
        require(sweepWeapon({-20,20,0},{20,20,0},{-20,0,0},{20,0,0},true,ray),
            "A fast blade tunnels through the target between animation frames");
        require(rays<=6,"Blade sweep exceeds the bounded work budget");
        require(!sweepWeapon({40,20,0},{80,20,0},{40,0,0},{80,0,0},true,ray),
            "Blade sweep hits an untouched neighbouring target");
        require(!sweepWeapon({-20,20,0},{20,20,0},{-20,0,0},{20,0,0},false,ray),
            "A new attack reuses stale weapon motion");
        std::cout<<"PASS actual OSG weapon transforms and bounded blade sweeps through real collision geometry\n";
    }
}
int main()
{
    try{balance();defense();ranged();blade();}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
