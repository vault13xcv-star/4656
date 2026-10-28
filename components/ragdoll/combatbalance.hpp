#ifndef OPENMW_COMPONENTS_RAGDOLL_COMBATBALANCE_HPP
#define OPENMW_COMPONENTS_RAGDOLL_COMBATBALANCE_HPP
#include <algorithm>
#include <cmath>
#include <osg/Vec3f>
#include <osg/Quat>
namespace Ragdoll::Combat
{
    inline float unit(float value) { return std::isfinite(value)?std::clamp(value/100.f,0.f,1.f):0.f; }
    inline float damage(float skill,bool exhausted) { return (.30f+.70f*unit(skill))*(exhausted?.5f:1.f); }
    inline float resource(float skill) { return 1.25f-.75f*unit(skill); }
    inline float wear(float skill) { return .5f+.80f*(1.f-unit(skill)); }
    inline float preparation(float skill,float agility,bool exhausted)
    { return (1.f+(1.f-unit(skill))*(.20f+.15f*(1.f-unit(agility))))*(exhausted?1.5f:1.f); }
    inline float recovery(float skill,float agility,bool exhausted)
    { return (1.f+(1.f-unit(skill))*(.35f+.15f*(1.f-unit(agility))))*(exhausted?1.5f:1.f); }
    inline float spread(float skill,float agility)
    { return (1.f-unit(skill))*(1.5f-.5f*unit(agility)); } // degrees, horizontal only
    inline osg::Vec3f spreadShot(const osg::Vec3f& velocity,float skill,float agility,float first,float second)
    {
        const float yaw=spread(skill,agility)*(first-second);
        return osg::Quat(yaw*3.14159265358979323846/180.,osg::Vec3f(0,0,1))*velocity;
    }
    inline osg::Vec3f ballisticAim(const osg::Vec3f& delta,float speed,float gravity=627.2f)
    {
        const float range=std::hypot(delta.x(),delta.y());
        if(range<.001f || speed<=0)return delta;
        const double v2=double(speed)*speed;
        const double discriminant=v2*v2-double(gravity)*(gravity*range*range+2*delta.z()*v2);
        const float tangent=discriminant>=0?float((v2-std::sqrt(discriminant))/(gravity*range)):1.f;
        osg::Vec3f result(delta.x(),delta.y(),range*tangent);return result;
    }
    inline float guardRaise(float agility) { return .18f-.09f*unit(agility); }
    inline float dodgeCost(float agility) { return 14.f-5.f*unit(agility); }
    inline float dodgeCooldown(float agility) { return .90f-.40f*unit(agility); }
    inline osg::Vec3f dodgeDirection(osg::Vec3f input)
    {
        input.z()=0;input.y()=std::min(0.f,input.y());
        if(input.length2()<.01f)return {0,-1,0};
        input.normalize();return input;
    }
    struct State
    {
        bool mGuardRequested=false,mGuardHeld=false,mGuardLatched=false,mDodgeRequested=false;
        bool mAttackActive=false,mHasWeaponSample=false,mContactWindow=false;
        double mGuardStarted=-1,mGuardReady=-1,mAttackStarted=-1,mLastAttack=-1,mDodgeReady=0;
        float mDodgeLeft=0,mWeaponWear=0,mShieldWear=0,mRecoveryLeft=0;
        osg::Vec3f mDodgeDirection,mWeaponBase,mWeaponTip;
        double mContactTime=-1;
    };
    inline float blockedFraction(const State& guard,double attackStart,double now)
    {
        if(!guard.mGuardHeld || now<guard.mGuardReady)return 0;
        return attackStart>=0 && guard.mGuardStarted>=attackStart && guard.mGuardStarted<=now?1.f:.30f;
    }
    inline osg::Vec3f dodgeVelocity(State& state,float dt)
    {
        if(dt<=0 || state.mDodgeLeft<=0)return {};
        const float step=std::min(dt,state.mDodgeLeft);
        state.mDodgeLeft=std::max(0.f,state.mDodgeLeft-dt);
        return state.mDodgeDirection*(160.f/.24f)*(step/dt);
    }
    inline int consumeWear(float amount,float& remainder)
    {
        remainder+=std::max(0.f,amount);
        const int whole=static_cast<int>(remainder);remainder-=whole;return whole;
    }
}
#endif
