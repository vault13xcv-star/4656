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
        require(near(wear(100),.5f) && near(resource(100),.875f),"Mastery wear or calibrated stamina endpoint is wrong");
        require(wear(0)<=1.30f && near(resource(1),3.f*(1.25f-.75f*.01f)),"Skill-1 stamina is not three times its previous cost");
        for(int agility:{1,50,100})
            require(near(preparation(1,agility,false),3) && near(recovery(1,agility,false),3)
                && near(preparation(100,agility,false),1) && near(recovery(100,agility,false),1),
                "Tempo misses the exact skill-1/100 boundaries");
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
                && (skill==0 || preparation(skill+1,agility,false)<preparation(skill,agility,false)
                && recovery(skill+1,agility,false)<recovery(skill,agility,false))),"Skill progression is not monotonic");
        }
        require(near(preparation(100,0,false),1) && near(recovery(100,100,false),1),"Mastery is not original tempo");
        require(preparation(50,100,false)<preparation(50,1,false),"Agility no longer improves intermediate combat tempo");
        require(resource(50)>resource(100) && resource(1)>resource(50),"Stamina cost progression is not monotonic");
        float carry=0;int loss=0;
        for(int hit=0;hit<1000;++hit)loss+=consumeWear(wear(100),carry);
        require(loss==500 && carry==0,"Mastery loses fractional durability savings");
        std::cout<<"PASS balance endpoints, monotonic stats, exact exhaustion and exact rebalance endpoints\n";
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
        require(guardRaise(100,100)<guardRaise(1,1) && dodgeCost(100)<dodgeCost(0)
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
    void exhaustionAndStagger()
    {
        using namespace Ragdoll::Combat;
        for(float agility:{1,100})
        {
            State state;
            const float wait=agility==1?10.f:3.f;
            require(near(fatigueDelay(agility),wait),"Agility fatigue-delay endpoint changed");
            require(!allowFatigueRecovery(state,0,agility,0),"Empty fatigue immediately regenerates");
            for(int frame=1;frame<60;++frame)
                require(!allowFatigueRecovery(state,0,agility,wait*frame/60),"Observing fatigue renewed or skipped the waiting period");
            require(allowFatigueRecovery(state,0,agility,wait),"Fatigue never resumes at its deadline");
            observeFatigue(state,0,agility,wait,true);
            require(!allowFatigueRecovery(state,0,agility,wait*2-.001),"An exhausted attack/raise did not restart the deadline");
            require(allowFatigueRecovery(state,0,agility,wait*2),"Repeated effort created a permanent fatigue lock");
            observeFatigue(state,8,agility,wait*2);
            require(!state.mFatigueDepleted,"Explicit positive fatigue restoration retained exhaustion lock");
            observeFatigue(state,0,agility,30);
            require(near(float(restedFatigueSeconds(state,0,agility,30,3600)),3600-wait),
                "Waiting/resting discards an entire hour or ignores the recovery delay");
        }
        State spam;
        float fatigue=100;
        for(int attack=0;attack<100;++attack)
        {
            const double now=attack*.25;
            fatigue=std::max(0.f,fatigue-4*resource(50));
            observeFatigue(spam,fatigue,50,now,true);
            if(fatigue==0)require(!allowFatigueRecovery(spam,0,50,now+.2),"Continuous exhausted dagger spam regenerated fatigue");
        }
        require(fatigue==0 && allowFatigueRecovery(spam,0,50,spam.mFatigueRestoreAt),"Retreating failed to unlock fatigue after spam");
        State draw;
        for(int frame=0;frame<1200;++frame)
        {
            const double now=frame/60.;
            observeFatigue(draw,0,100,now,true);
            require(!allowFatigueRecovery(draw,0,100,now),"A prolonged exhausted bow draw bypasses the recovery lock");
        }
        require(allowFatigueRecovery(draw,0,100,draw.mFatigueRestoreAt),"Cancelling an exhausted wind-up never unlocks regeneration");
        State victim;
        for(int hit=0;hit<100;++hit)
            require(!allowStagger(victim,1,0,200,200,50,hit*.1),"Tiny dagger hits still stun-lock a strong opponent");
        require(allowStagger(victim,20,0,200,200,50,20),"A substantial hit cannot stagger");
        require(!allowStagger(victim,20,0,200,200,50,20.5),"Repeated substantial hits extend a flinch without a counterattack window");
        require(allowStagger(victim,20,0,200,200,50,21),"Flinch resistance never expires");
        std::cout<<"PASS exhausted-spam lock, attack/raise resets, agility deadlines, rest, explicit restoration and anti-stunlock\n";
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
    try{balance();defense();exhaustionAndStagger();ranged();blade();}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
