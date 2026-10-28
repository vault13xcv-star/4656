
#include "combat.hpp"
#include "weapontype.hpp"

#include <components/misc/strings/algorithm.hpp>

#include <array>

#include <components/misc/rng.hpp>
#include <components/misc/convert.hpp>
#include <components/settings/values.hpp>
#include <components/ragdoll/meleecontact.hpp>

#include <components/sceneutil/positionattitudetransform.hpp>

#include <components/esm3/loadench.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadsoun.hpp>

#include "../mwbase/dialoguemanager.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwphysics/itemhit.hpp"
#include "../mwphysics/raycasting.hpp"
#include "../mwrender/animation.hpp"
#include "../mwrender/camera.hpp"
#include "../mwrender/ragdoll.hpp"
#include "../mwrender/weaponanimation.hpp"
#include "../mwworld/datetimemanager.hpp"
#include <osg/ComputeBoundsVisitor>
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/globals.hpp"
#include "../mwworld/inventorystore.hpp"

#include "actorutil.hpp"
#include "difficultyscaling.hpp"
#include "movement.hpp"
#include "npcstats.hpp"
#include "pathfinding.hpp"
#include "spellcasting.hpp"
#include "spellresistance.hpp"

namespace
{

    void captureWoundContact(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim,
        const MWWorld::Ptr& projectile, Ragdoll::Impact& impact)
    {
        if (victim.isEmpty() || !victim.getClass().isNpc())
            return;
        const auto world = MWBase::Environment::get().getWorld();
        auto* animation = world->getAnimation(victim);
        if (!animation || !animation->getObjectRoot())
            return;
        const auto extents = world->getHalfExtents(victim);
        osg::Vec3f from, to;
        if (!projectile.isEmpty())
        {
            // Physics reports a contact on the actor's coarse collision shape.
            // Trace through it along the actual projectile flight to find the
            // visible body surface (or reject an anatomical miss).
            const float span = std::max(16.f, extents.length() * 2.f);
            from = impact.mWeaponPosition - impact.mWeaponDirection * span;
            to = impact.mWeaponPosition + impact.mWeaponDirection * span;
        }
        else if (!attacker.isEmpty())
        {
            const auto& position = attacker.getRefData().getPosition();
            from = world->getActorHeadTransform(attacker).getTrans();
            if (!std::isfinite(from.length2()) || from == osg::Vec3f())
                from = position.asVec3() + osg::Vec3f(0, 0, world->getHalfExtents(attacker, true).z() * 1.7f);
            auto direction = MWPhysics::itemAimDirection(position.rot[0], position.rot[2]);
            const auto* camera = world->getCamera();
            if (attacker == world->getPlayerPtr() && camera && !camera->isVanityOrPreviewModeEnabled())
                std::tie(from, direction) = MWPhysics::itemViewRay(camera->getViewMatrix());
            to = from + direction * ((victim.getRefData().getPosition().asVec3() - from).length()
                + extents.length() * 2.f);
        }
        else
            return;
        std::vector<MWWorld::Ptr> ignored;
        if (!attacker.isEmpty())
            ignored.push_back(attacker);
        MWPhysics::RayCastingResult surface{};
        if (world->castRenderingRay(surface, from, to, attacker == world->getPlayerPtr(), false, ignored)
            && surface.mHitObject == victim && surface.mHitBody != -2)
        {
            impact.mWeaponPosition = surface.mHitPos;
            impact.mWeaponHitBody = surface.mHitBody >= 0 ? surface.mHitBody
                : MWRender::RagdollController::findHitBody(*animation->getObjectRoot(), surface.mHitPos);
        }
        if (impact.mWeaponHitBody < 0)
        {
            const auto contact = MWRender::RagdollController::findHitContact(*animation->getObjectRoot(), from, to);
            impact.mWeaponHitBody = contact.mBody;
            if (contact.mBody >= 0)
                impact.mWeaponPosition = contact.mPosition;
        }
    }

}

namespace MWMechanics
{

    std::shared_ptr<RagdollHit> makeRagdollHit(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim,
        const MWWorld::Ptr& weapon, const MWWorld::Ptr& projectile, int attackType,
        const osg::Vec3f& hitPosition, const osg::Vec3f& impactDirection)
    {
        auto hit = std::make_shared<RagdollHit>();
        hit->mAwaitingWeapon = true;
        auto& impact = hit->mWeapon;
        impact.mPosition = hitPosition;
        impact.mWeaponPosition = hitPosition;
        impact.mHasWeaponContact = true;
        impact.mValid = true;
        osg::Vec3f direction = impactDirection;
        if (projectile.isEmpty() && !attacker.isEmpty())
        {
            const auto& rotation = attacker.getRefData().getPosition().rot;
            direction = osg::Quat(rotation[0], osg::Vec3f(-1, 0, 0), rotation[2], osg::Vec3f(0, 0, -1),
                0.f, osg::Vec3f(0, 1, 0)) * osg::Vec3f(0, 1, 0);
        }
        if (direction.length2() < 1e-6f && !attacker.isEmpty() && !victim.isEmpty())
            direction = victim.getRefData().getPosition().asVec3() - attacker.getRefData().getPosition().asVec3();
        if (direction.normalize() == 0.f)
            direction.set(0, 1, 0);
        const auto& settings = Settings::ragdoll();
        const auto* weaponData = !weapon.isEmpty() && weapon.getType() == ESM::Weapon::sRecordId
            ? &weapon.get<ESM::Weapon>()->mBase->mData : nullptr;
        const auto* ammoData = !projectile.isEmpty() && projectile.getType() == ESM::Weapon::sRecordId
            ? &projectile.get<ESM::Weapon>()->mBase->mData : nullptr;
        const int type = ammoData ? ammoData->mType : weaponData ? weaponData->mType : static_cast<int>(ESM::Weapon::HandToHand);
        impact.mTwoHandedCutting = projectile.isEmpty()
            && (type == ESM::Weapon::LongBladeTwoHand || type == ESM::Weapon::AxeTwoHand);
        impact.mTwoHandedHammer = projectile.isEmpty() && type == ESM::Weapon::BluntTwoClose;
        impact.mAttackType = !projectile.isEmpty() ? Ragdoll::AttackType::Projectile
            : attackType == ESM::Weapon::AT_Chop ? Ragdoll::AttackType::Chop
            : attackType == ESM::Weapon::AT_Slash ? Ragdoll::AttackType::Slash
            : attackType == ESM::Weapon::AT_Thrust ? Ragdoll::AttackType::Thrust
                                                  : Ragdoll::AttackType::Unknown;
        if (type == ESM::Weapon::LongBladeOneHand)
            impact.mWoundWeapon = Ragdoll::WoundWeapon::Blade;
        else if (type == ESM::Weapon::LongBladeTwoHand || type == ESM::Weapon::AxeOneHand
            || type == ESM::Weapon::AxeTwoHand)
            impact.mWoundWeapon = Ragdoll::WoundWeapon::HeavyBlade;
        else if (type == ESM::Weapon::BluntOneHand || type == ESM::Weapon::BluntTwoClose
            || type == ESM::Weapon::BluntTwoWide)
            impact.mWoundWeapon = Ragdoll::WoundWeapon::Blunt;
        else if (type == ESM::Weapon::Bolt || type == ESM::Weapon::MarksmanCrossbow)
            impact.mWoundWeapon = Ragdoll::WoundWeapon::Bolt;
        bool dagger = false;
        if (type == ESM::Weapon::ShortBladeOneHand && weaponData)
        {
            const std::string model = Misc::StringUtils::lowerCase(weapon.get<ESM::Weapon>()->mBase->mModel);
            if (model.find("dagger") != std::string::npos || model.find("tanto") != std::string::npos)
                dagger = true;
            else if (model.find("shortsword") == std::string::npos && model.find("short_sword") == std::string::npos
                && model.find("wakizashi") == std::string::npos)
                dagger = weaponData->mWeight <= static_cast<float>(settings.mDaggerMaxWeight);
        }
        if (type == ESM::Weapon::Bolt || type == ESM::Weapon::MarksmanCrossbow)
        {
            impact.mWeaponDistance = settings.mCrossbowDistance;
            impact.mCrossbow = true;
        }
        else if (type == ESM::Weapon::LongBladeTwoHand || type == ESM::Weapon::BluntTwoClose
            || type == ESM::Weapon::BluntTwoWide || type == ESM::Weapon::AxeTwoHand)
            impact.mWeaponDistance = settings.mTwoHandDistance;
        else if (type == ESM::Weapon::Arrow || type == ESM::Weapon::MarksmanBow)
            impact.mWeaponDistance = settings.mBowDistance;
        else if (type == ESM::Weapon::MarksmanThrown)
            impact.mWeaponDistance = settings.mThrownDistance;
        else if (type == ESM::Weapon::SpearTwoWide)
            impact.mWeaponDistance = settings.mSpearDistance;
        else if (type == ESM::Weapon::LongBladeOneHand || type == ESM::Weapon::BluntOneHand
            || type == ESM::Weapon::AxeOneHand || (type == ESM::Weapon::ShortBladeOneHand && !dagger))
            impact.mWeaponDistance = settings.mOneHandDistance;
        // Daggers and unarmed contacts retain their small native imbalance.
        if (projectile.isEmpty() && attackType == ESM::Weapon::AT_Slash)
        {
            // Native Morrowind slash animations sweep right-to-left. Keep a small
            // forward component and use the attacker's frame, not the victim's.
            direction = osg::Vec3f(-direction.y(), direction.x(), 0.f) + direction * 0.15f;
            direction.normalize();
        }
        else if (projectile.isEmpty() && attackType == ESM::Weapon::AT_Chop)
        {
            direction.z() -= 0.45f;
            direction.normalize();
        }
        impact.mWeaponDirection = direction;
        captureWoundContact(attacker, victim, projectile, impact);
        if (projectile.isEmpty() && !attacker.isEmpty() && attacker.getClass().isActor()
            && (!weaponData || (type >= ESM::Weapon::ShortBladeOneHand && type <= ESM::Weapon::SpearTwoWide)))
        {
            auto [minimum, maximum] = MWBase::Environment::get().getESMStore()->getMeleeWeaponWeightRange();
            if (settings.mWeaponMinWeight >= 0.f)
                minimum = settings.mWeaponMinWeight;
            if (settings.mWeaponMaxWeight >= 0.f)
                maximum = settings.mWeaponMaxWeight;
            const float strength
                = attacker.getClass().getCreatureStats(attacker).getAttribute(ESM::Attribute::Strength).getModified();
            impact.mWeaponMultiplier = Ragdoll::meleeMultiplier(
                weaponData ? weaponData->mWeight : 0.f, minimum, maximum, strength, weaponData != nullptr);
        }
        impact.mWeaponImpulseScale = type == ESM::Weapon::Bolt || type == ESM::Weapon::MarksmanCrossbow
            ? settings.mCrossbowImpulseScale.get()
            : type == ESM::Weapon::Arrow || type == ESM::Weapon::MarksmanBow ? settings.mBowImpulseScale.get()
            : type == ESM::Weapon::MarksmanThrown                            ? settings.mThrownImpulseScale.get()
                                                                             : settings.mMeleeImpulseScale.get();
        return hit;
    }

    bool applyOnStrikeEnchantment(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, const MWWorld::Ptr& object,
        const osg::Vec3f& hitPosition, const bool fromProjectile, const std::shared_ptr<RagdollHit>& ragdollHit)
    {
        const ESM::RefId enchantmentName = !object.isEmpty() ? object.getClass().getEnchantment(object) : ESM::RefId();
        if (!enchantmentName.empty())
        {
            const ESM::Enchantment* enchantment
                = MWBase::Environment::get().getESMStore()->get<ESM::Enchantment>().find(enchantmentName);
            if (enchantment->mData.mType == ESM::Enchantment::WhenStrikes)
            {
                MWMechanics::CastSpell cast(attacker, victim, fromProjectile);
                cast.mHitPosition = hitPosition;
                cast.mRagdollHit = ragdollHit;
                if (ragdollHit)
                    cast.mImpactDirection = ragdollHit->mWeapon.mWeaponDirection;
                const bool applied = cast.cast(object, false);
                // Apply magic effects directly instead of waiting a frame to allow soul trap to work on one-hit kills
                if (applied && !victim.isEmpty() && victim.getClass().isActor())
                    MWBase::Environment::get().getMechanicsManager()->updateMagicEffects(victim);
                return applied;
            }
        }
        return false;
    }

    float getCombatSkill(const MWWorld::Ptr& actor,const MWWorld::Ptr& weapon)
    {
        if(actor.isEmpty())return 100;
        const auto skill=weapon.isEmpty()?ESM::Skill::HandToHand:weapon.getClass().getEquipmentSkill(weapon);
        return actor.getClass().getSkill(actor,skill);
    }

    void updateActiveDefense(const MWWorld::Ptr& actor,float duration,bool canAct)
    {
        auto& stats=actor.getClass().getCreatureStats(actor);auto& state=stats.mCombat;
        const bool wasGuardHeld=state.mGuardHeld;
        const auto world=MWBase::Environment::get().getWorld();
        const double now=world->getTimeManager()->getSimulationTime();
        state.mRecoveryLeft=std::max(0.f,state.mRecoveryLeft-duration);
        const float agility=stats.getAttribute(ESM::Attribute::Agility).getModified();
        MWWorld::Ptr shield,weapon;
        if(actor.getClass().hasInventoryStore(actor))
        {
            auto& inv=actor.getClass().getInventoryStore(actor);
            const auto left=inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
            const auto right=inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
            if(left!=inv.end() && left->getType()==ESM::Armor::sRecordId)shield=*left;
            if(right!=inv.end() && right->getType()==ESM::Weapon::sRecordId)weapon=*right;
        }
        if(!weapon.isEmpty() && (getWeaponType(weapon.get<ESM::Weapon>()->mBase->mData.mType)->mFlags&ESM::WeaponType::TwoHanded))shield={};
        if(actor==getPlayer())
        {
            // One press selects one action. Holding RMB never repeats dodges;
            // changing equipment while holding it cannot initiate another dodge.
            Ragdoll::Combat::selectDefense(state,!shield.isEmpty());
        }
        const float skill=actor.getClass().getSkill(actor,ESM::Skill::Block);
        if(actor!=getPlayer())
        {
            state.mGuardRequested=false;
            std::vector<MWWorld::Ptr> targets;stats.getAiSequence().getCombatTargets(targets);
            for(const auto& target:targets)
            {
                const auto& attack=target.getClass().getCreatureStats(target).mCombat;
                const double reaction=.32-.16*Ragdoll::Combat::unit(skill)-.06*Ragdoll::Combat::unit(agility);
                if(attack.mAttackActive && now-attack.mAttackStarted>=reaction)state.mGuardRequested=true;
            }
        }
        auto fatigue=stats.getFatigue();
        if(stats.isDead() || stats.getKnockedDown() || stats.isParalyzed() || stats.getHitRecovery())state.mAttackActive=false;
        const bool allowed=canAct && !stats.isDead() && !stats.getKnockedDown() && !stats.isParalyzed() && !stats.getHitRecovery();
        if(!state.mGuardRequested)state.mGuardLatched=false;
        const bool canGuard=allowed && stats.getDrawState()==DrawState::Weapon && !shield.isEmpty();
        if(!canGuard || !state.mGuardRequested)
            state.mGuardHeld=false;
        else if(!state.mGuardHeld && !state.mGuardLatched)
        {
            state.mGuardLatched=true;
            Ragdoll::Combat::observeFatigue(state,fatigue.getCurrent(),agility,now,true);
            if(fatigue.getCurrent()>0 && now>=state.mGuardNextRaise)
            {
                const float cost=Ragdoll::Combat::guardRaiseCost(skill);
                fatigue.setCurrent(std::max(0.f,fatigue.getCurrent()-cost));
                if(fatigue.getCurrent()>0)
                {
                    state.mGuardHeld=true;state.mGuardStarted=now;
                    state.mGuardReady=now+Ragdoll::Combat::guardRaise(skill,agility);
                    state.mGuardNextRaise=state.mGuardReady+Ragdoll::Combat::guardRecovery(skill,agility);
                }
            }
        }
        if(state.mGuardHeld)
        {
            fatigue.setCurrent(std::max(0.f,fatigue.getCurrent()-duration*Ragdoll::Combat::guardHoldCost(skill)));
            if(fatigue.getCurrent()<=0)state.mGuardHeld=false;
        }
        if(state.mDodgeRequested)
        {
            state.mDodgeRequested=false;
            const float cost=Ragdoll::Combat::dodgeCost(agility);
            if(allowed && shield.isEmpty() && now>=state.mDodgeReady && fatigue.getCurrent()>0
                && fatigue.getCurrent()>=cost && world->isOnGround(actor)
                && !world->isSwimming(actor))
            {
                const auto& movement=actor.getClass().getMovementSettings(actor);
                if(movement.mPosition[2]<=0 && (movement.mPosition[1]<=0 || std::abs(movement.mPosition[0])>.01f))
                {
                    state.mDodgeDirection=Ragdoll::Combat::dodgeDirection({movement.mPosition[0],movement.mPosition[1],0});
                    state.mDodgeLeft=.24f;state.mDodgeReady=now+Ragdoll::Combat::dodgeCooldown(agility);
                    state.mGuardHeld=false;fatigue.setCurrent(std::max(0.f,fatigue.getCurrent()-cost));
                }
            }
        }
        if(wasGuardHeld && !state.mGuardHeld)
            state.mGuardNextRaise=std::max(state.mGuardNextRaise,now+Ragdoll::Combat::guardRecovery(skill,agility));
        Ragdoll::Combat::observeFatigue(state,fatigue.getCurrent(),agility,now,
            state.mAttackActive || state.mGuardHeld || state.mDodgeLeft>0);
        stats.setFatigue(fatigue);
    }

    bool allowHitStagger(const MWWorld::Ptr& actor,float healthDamage,float fatigueDamage)
    {
        auto& stats=actor.getClass().getCreatureStats(actor);
        return Ragdoll::Combat::allowStagger(stats.mCombat,healthDamage,fatigueDamage,
            stats.getHealth().getModified(),stats.getFatigue().getModified(),
            stats.getAttribute(ESM::Attribute::Agility).getModified(),
            MWBase::Environment::get().getWorld()->getTimeManager()->getSimulationTime());
    }

    bool isNonHostilePhysicalHit(const MWWorld::Ptr& attacker,const MWWorld::Ptr& target,const MWWorld::Ptr& player)
    {
        if(attacker.isEmpty() || target.isEmpty() || attacker==player || target==player
            || !attacker.getClass().isActor() || !target.getClass().isActor())return false;
        // Fight/disposition are aggression toward the player, not a faction
        // relationship between two bandits. Use actual combat targets in either
        // direction so guard/bandit fights and scripted hostility retain damage.
        return !attacker.getClass().getCreatureStats(attacker).getAiSequence().isInCombat(target)
            && !target.getClass().getCreatureStats(target).getAiSequence().isInCombat(attacker);
    }

    float blockMeleeAttack(const MWWorld::Ptr& attacker,const MWWorld::Ptr& blocker,const MWWorld::Ptr& weapon,
        float& damage,float attackStrength,double attackStart)
    {
        if(blocker.isEmpty() || !blocker.getClass().hasInventoryStore(blocker))return false;
        auto& stats=blocker.getClass().getCreatureStats(blocker);auto& guard=stats.mCombat;
        if(!guard.mGuardHeld || stats.getFatigue().getCurrent()<=0 || stats.getKnockedDown() || stats.isParalyzed())return false;
        auto& inv=blocker.getClass().getInventoryStore(blocker);
        auto shield=inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        if(shield==inv.end() || shield->getType()!=ESM::Armor::sRecordId)return false;
        if(attacker.isEmpty() || !blocker.getRefData().getBaseNode())return false;
        osg::Vec3f toward=attacker.getRefData().getPosition().asVec3()-blocker.getRefData().getPosition().asVec3();
        toward.z()=0;toward.normalize();
        osg::Vec3f facing=blocker.getRefData().getBaseNode()->getAttitude()*osg::Vec3f(0,1,0);
        if(toward*facing<.25f)return false;
        const double now=MWBase::Environment::get().getWorld()->getTimeManager()->getSimulationTime();
        if(attackStart<0)attackStart=attacker.getClass().getCreatureStats(attacker).mCombat.mAttackStarted;
        const float fraction=Ragdoll::Combat::blockedFraction(guard,attackStart,now);
        if(fraction<=0)return false;
        const float skill=blocker.getClass().getSkill(blocker,ESM::Skill::Block);
        const float blocked=damage*fraction;damage-=blocked;
        int health=shield->getClass().getItemHealth(*shield);
        health=std::max(0,health-Ragdoll::Combat::consumeWear(std::max(1.f,blocked)*Ragdoll::Combat::wear(skill),guard.mShieldWear));
        shield->getCellRef().setCharge(health);
        if(!health){inv.unequipItem(*shield);guard.mGuardHeld=false;}
        auto fatigue=stats.getFatigue();
        fatigue.setCurrent(std::max(0.f,fatigue.getCurrent()-Ragdoll::Combat::guardImpactCost(skill,blocked)));
        Ragdoll::Combat::observeFatigue(guard,fatigue.getCurrent(),
            stats.getAttribute(ESM::Attribute::Agility).getModified(),now,true);
        stats.setFatigue(fatigue);if(fatigue.getCurrent()<=0)guard.mGuardHeld=false;
        stats.setBlock(true);
        MWBase::Environment::get().getSoundManager()->playSound3D(blocker,ESM::RefId::stringRefId("Light Armor Hit"),1,1);
        if(blocker==getPlayer())blocker.getClass().skillUsageSucceeded(blocker,ESM::Skill::Block,ESM::Skill::Block_Success);
        return fraction;
    }

    bool isNormalWeapon(const MWWorld::Ptr& weapon)
    {
        if (weapon.isEmpty())
            return false;

        const int flags = weapon.get<ESM::Weapon>()->mBase->mData.mFlags;
        bool isSilver = flags & ESM::Weapon::Silver;
        bool isMagical = flags & ESM::Weapon::Magical;
        bool isEnchanted = !weapon.getClass().getEnchantment(weapon).empty();

        return !isSilver && !isMagical && (!isEnchanted || !Settings::game().mEnchantedWeaponsAreMagical);
    }

    void resistNormalWeapon(
        const MWWorld::Ptr& actor, const MWWorld::Ptr& attacker, const MWWorld::Ptr& weapon, float& damage)
    {
        if (weapon.isEmpty() || !isNormalWeapon(weapon))
            return;

        const MWMechanics::MagicEffects& effects = actor.getClass().getCreatureStats(actor).getMagicEffects();
        const float resistance = effects.getOrDefault(ESM::MagicEffect::ResistNormalWeapons).getMagnitude() / 100.f;
        const float weakness = effects.getOrDefault(ESM::MagicEffect::WeaknessToNormalWeapons).getMagnitude() / 100.f;

        damage *= 1.f - std::min(1.f, resistance - weakness);

        if (resistance - weakness >= 1.f && attacker == getPlayer())
            MWBase::Environment::get().getWindowManager()->messageBox("#{sMagicTargetResistsWeapons}");
    }

    void applyWerewolfDamageMult(const MWWorld::Ptr& actor, const MWWorld::Ptr& weapon, float& damage)
    {
        if (damage == 0 || weapon.isEmpty() || !actor.getClass().isNpc())
            return;

        const int flags = weapon.get<ESM::Weapon>()->mBase->mData.mFlags;
        bool isSilver = flags & ESM::Weapon::Silver;

        if (isSilver && actor.getClass().getNpcStats(actor).isWerewolf())
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            damage *= store.get<ESM::GameSetting>().find("fWereWolfSilverWeaponDamageMult")->mValue.getFloat();
        }
    }

    bool projectileHit(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, MWWorld::Ptr weapon,
        const MWWorld::Ptr& projectile, const osg::Vec3f& hitPosition, float attackStrength,
        const osg::Vec3f& impactDirection,float combatDamage,double attackStart)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::Store<ESM::GameSetting>& gmst = world->getStore().get<ESM::GameSetting>();

        bool validVictim = !victim.isEmpty() && victim.getClass().isActor();
        if (!victim.isEmpty() && victim.getClass().isItem(victim))
        {
            osg::Vec3f direction = impactDirection;
            if (direction.normalize() > 0)
            {
                const int type = projectile.isEmpty() ? int(ESM::Weapon::MarksmanThrown)
                                                      : projectile.get<ESM::Weapon>()->mBase->mData.mType;
                const auto& settings = Settings::ragdoll();
                const float scale = type == ESM::Weapon::Bolt ? settings.mCrossbowImpulseScale.get()
                    : type == ESM::Weapon::Arrow              ? settings.mBowImpulseScale.get()
                                                              : settings.mThrownImpulseScale.get();
                world->applyItemImpulse(victim, direction * (400.f + 600.f * attackStrength), hitPosition, scale);
            }
        }

        if(attacker.isEmpty() || !attacker.getClass().isActor())return false;
        // Return a stopped/deflected contact, never search through this actor.
        // This also prevents puncture bleeding and on-strike enchantments.
        if(isNonHostilePhysicalHit(attacker,victim,getPlayer()))return true;
        ESM::RefId weaponSkill = ESM::Skill::Marksman;
        if (!weapon.isEmpty())
            weaponSkill = weapon.getClass().getEquipmentSkill(weapon);

        float damage = 0.f;
        if (validVictim)
        {
            if (attacker == getPlayer())
                MWBase::Environment::get().getWindowManager()->setEnemy(victim);

            // The swept projectile has physically contacted this actor.

            {
                const auto& attack = weapon.get<ESM::Weapon>()->mBase->mData.mChop;
                damage = attack[0] + ((attack[1] - attack[0]) * attackStrength); // Bow/crossbow damage
            }
            {
                // Arrow/bolt damage
                // NB in case of thrown weapons, we are applying the damage twice since projectile == weapon
                const auto& attack = projectile.get<ESM::Weapon>()->mBase->mData.mChop;
                damage += attack[0] + ((attack[1] - attack[0]) * attackStrength);
            }
            adjustWeaponDamage(damage, weapon, attacker);
        }

        reduceWeaponCondition(damage, validVictim, weapon, attacker);
        const auto& attackStats=attacker.getClass().getCreatureStats(attacker);
        const float combatScale=combatDamage>=0?combatDamage:Ragdoll::Combat::damage(getCombatSkill(attacker,weapon),attackStats.getFatigue().getCurrent()<=0);
        damage*=combatScale;

        if (validVictim)
        {
            if (weapon == projectile || Settings::game().mOnlyAppropriateAmmunitionBypassesResistance
                || isNormalWeapon(weapon))
                resistNormalWeapon(victim, attacker, projectile, damage);
            applyWerewolfDamageMult(victim, projectile, damage);

            if (attacker == getPlayer())
                attacker.getClass().skillUsageSucceeded(attacker, weaponSkill, ESM::Skill::Weapon_SuccessfulHit);

            const MWMechanics::AiSequence& sequence = victim.getClass().getCreatureStats(victim).getAiSequence();
            bool unaware = attacker == getPlayer() && !sequence.isInCombat()
                && !MWBase::Environment::get().getMechanicsManager()->awarenessCheck(attacker, victim);
            bool knockedDown = victim.getClass().getCreatureStats(victim).getKnockedDown();
            if (knockedDown || unaware)
            {
                static const float fCombatKODamageMult = gmst.find("fCombatKODamageMult")->mValue.getFloat();
                damage *= fCombatKODamageMult;
                if (!knockedDown)
                    MWBase::Environment::get().getSoundManager()->playSound3D(
                        victim, ESM::RefId::stringRefId("critical damage"), 1.0f, 1.0f);
            }
        }

        auto ragdollHit = makeRagdollHit(attacker, victim, weapon, projectile, 0, hitPosition, impactDirection);
        const float beforeBlock=damage;
        const bool fullyBlocked=validVictim && blockMeleeAttack(attacker,victim,weapon,damage,attackStrength,attackStart)>=1;
        const float armorScale=combatScale*(beforeBlock>0?damage/beforeBlock:1.f);
        // Apply "On hit" effect of the projectile with the same contact identity.
        if(!fullyBlocked)applyOnStrikeEnchantment(attacker, victim, projectile, hitPosition, true, ragdollHit);
        if (!fullyBlocked && weapon != projectile)
            applyOnStrikeEnchantment(attacker, victim, weapon, hitPosition, true, ragdollHit);

        if (validVictim)
        {
            // Impact visuals have no inventory/world reference and cannot be
            // recovered. Do not mint a second ammunition item in the corpse.

            MWBase::Environment::get().getLuaManager()->onHit(attacker, victim, weapon, projectile, 0, attackStrength,
                damage, true, hitPosition, true, MWMechanics::DamageSourceType::Ranged,
                victim.getClass().getCreatureStats(victim).queueRagdollHit(ragdollHit),armorScale);
        }
        return fullyBlocked;
    }

    float getHitChance(const MWWorld::Ptr&,const MWWorld::Ptr&,int)
    {
        return 100.f; // Contact geometry is the sole hit/miss decision.
    }

    void applyElementalShields(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim)
    {
        // Don't let elemental shields harm the player in god mode.
        bool godmode = attacker == getPlayer() && MWBase::Environment::get().getWorld()->getGodModeState();
        if (godmode)
            return;
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        static const std::array<ESM::RefId, 3> elementalShieldEffects{ ESM::MagicEffect::FireShield,
            ESM::MagicEffect::LightningShield, ESM::MagicEffect::FrostShield };
        for (const auto elementalShieldEffect : elementalShieldEffects)
        {
            float magnitude = victim.getClass()
                                  .getCreatureStats(victim)
                                  .getMagicEffects()
                                  .getOrDefault(elementalShieldEffect)
                                  .getMagnitude();

            if (!magnitude)
                continue;

            CreatureStats& attackerStats = attacker.getClass().getCreatureStats(attacker);
            float saveTerm = attacker.getClass().getSkill(attacker, ESM::Skill::Destruction)
                + 0.2f * attackerStats.getAttribute(ESM::Attribute::Willpower).getModified()
                + 0.1f * attackerStats.getAttribute(ESM::Attribute::Luck).getModified();

            float fatigueMax = attackerStats.getFatigue().getModified();
            float fatigueCurrent = attackerStats.getFatigue().getCurrent();

            float normalisedFatigue = floor(fatigueMax) == 0 ? 1 : std::max(0.0f, (fatigueCurrent / fatigueMax));

            saveTerm *= 1.25f * normalisedFatigue;

            float x = std::max(0.f, saveTerm - Misc::Rng::roll0to99(prng));

            ESM::RefId element = ESM::MagicEffect::FireDamage;
            if (elementalShieldEffect == ESM::MagicEffect::LightningShield)
                element = ESM::MagicEffect::ShockDamage;
            if (elementalShieldEffect == ESM::MagicEffect::FrostShield)
                element = ESM::MagicEffect::FrostDamage;

            float elementResistance
                = MWMechanics::getEffectResistanceAttribute(element, &attackerStats.getMagicEffects());

            x = std::min(100.f, x + elementResistance);

            static const float fElementalShieldMult = MWBase::Environment::get()
                                                          .getESMStore()
                                                          ->get<ESM::GameSetting>()
                                                          .find("fElementalShieldMult")
                                                          ->mValue.getFloat();
            x = fElementalShieldMult * magnitude * (1.f - 0.01f * x);

            // Note swapped victim and attacker, since the attacker takes the damage here.
            x = scaleDamage(x, victim, attacker);

            MWMechanics::DynamicStat<float> health = attackerStats.getHealth();
            health.setCurrent(health.getCurrent() - x);
            attackerStats.setHealth(health);

            MWBase::Environment::get().getSoundManager()->playSound3D(
                attacker, ESM::RefId::stringRefId("Health Damage"), 1.0f, 1.0f);
        }
    }

    void reduceWeaponCondition(float damage, bool hit, MWWorld::Ptr& weapon, const MWWorld::Ptr& attacker)
    {
        if (weapon.isEmpty())
            return;

        if (!hit)
            damage = 0.f;

        const bool weaphashealth = weapon.getClass().hasItemHealth(weapon);
        if (weaphashealth)
        {
            int weaphealth = weapon.getClass().getItemHealth(weapon);

            bool godmode
                = attacker == MWMechanics::getPlayer() && MWBase::Environment::get().getWorld()->getGodModeState();

            // weapon condition does not degrade when godmode is on
            if (!godmode)
            {
                const float fWeaponDamageMult = MWBase::Environment::get()
                                                    .getESMStore()
                                                    ->get<ESM::GameSetting>()
                                                    .find("fWeaponDamageMult")
                                                    ->mValue.getFloat();
                float x = float(int(std::max(1.f, fWeaponDamageMult * damage)))*Ragdoll::Combat::wear(getCombatSkill(attacker,weapon));
                x=static_cast<float>(Ragdoll::Combat::consumeWear(x,attacker.getClass().getCreatureStats(attacker).mCombat.mWeaponWear));

                weaphealth -= std::min(int(x), weaphealth);
                weapon.getCellRef().setCharge(weaphealth);
            }

            // Weapon broken? unequip it
            if (weaphealth == 0)
                weapon = *attacker.getClass().getInventoryStore(attacker).unequipItem(weapon);
        }
    }

    void adjustWeaponDamage(float& damage, const MWWorld::Ptr& weapon, const MWWorld::Ptr& attacker)
    {
        if (weapon.isEmpty())
            return;

        const bool weaphashealth = weapon.getClass().hasItemHealth(weapon);
        if (weaphashealth)
        {
            damage *= weapon.getClass().getItemNormalizedHealth(weapon);
        }

        static const float fDamageStrengthBase = MWBase::Environment::get()
                                                     .getESMStore()
                                                     ->get<ESM::GameSetting>()
                                                     .find("fDamageStrengthBase")
                                                     ->mValue.getFloat();
        static const float fDamageStrengthMult = MWBase::Environment::get()
                                                     .getESMStore()
                                                     ->get<ESM::GameSetting>()
                                                     .find("fDamageStrengthMult")
                                                     ->mValue.getFloat();
        damage *= fDamageStrengthBase
            + (attacker.getClass().getCreatureStats(attacker).getAttribute(ESM::Attribute::Strength).getModified()
                * fDamageStrengthMult * 0.1f);
    }

    void getHandToHandDamage(
        const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, float& damage, bool& healthdmg, float attackStrength)
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        static const float minstrike = store.get<ESM::GameSetting>().find("fMinHandToHandMult")->mValue.getFloat();
        static const float maxstrike = store.get<ESM::GameSetting>().find("fMaxHandToHandMult")->mValue.getFloat();
        damage = static_cast<float>(attacker.getClass().getSkill(attacker, ESM::Skill::HandToHand));
        damage *= minstrike + ((maxstrike - minstrike) * attackStrength);

        MWMechanics::CreatureStats& otherstats = victim.getClass().getCreatureStats(victim);
        healthdmg = otherstats.isParalyzed() || otherstats.getKnockedDown();
        bool isWerewolf = (attacker.getClass().isNpc() && attacker.getClass().getNpcStats(attacker).isWerewolf());

        // Options in the launcher's combo box: unarmedFactorsStrengthComboBox
        // 0 = Do not factor strength into hand-to-hand combat.
        // 1 = Factor into werewolf hand-to-hand combat.
        // 2 = Ignore werewolves.
        const int factorStrength = Settings::game().mStrengthInfluencesHandToHand;
        if (factorStrength == 1 || (factorStrength == 2 && !isWerewolf))
        {
            damage
                *= attacker.getClass().getCreatureStats(attacker).getAttribute(ESM::Attribute::Strength).getModified()
                / 40.0f;
        }

        if (isWerewolf)
        {
            healthdmg = true;
            // GLOB instead of GMST because it gets updated during a quest
            damage *= MWBase::Environment::get().getWorld()->getGlobalFloat(MWWorld::Globals::sWerewolfClawMult);
        }
        if (healthdmg)
        {
            static const float fHandtoHandHealthPer
                = store.get<ESM::GameSetting>().find("fHandtoHandHealthPer")->mValue.getFloat();
            damage *= fHandtoHandHealthPer;
        }

        MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
        auto& prng = MWBase::Environment::get().getWorld()->getPrng();
        if (isWerewolf)
        {
            const ESM::Sound* sound = store.get<ESM::Sound>().searchRandom("WolfHit", prng);
            if (sound)
                sndMgr->playSound3D(victim, sound->mId, 1.0f, 1.0f);
        }
        else if (!healthdmg)
        {
            static const std::array<ESM::RefId, 2> sounds
                = { ESM::RefId::stringRefId("Hand To Hand Hit"), ESM::RefId::stringRefId("Hand To Hand Hit 2") };
            sndMgr->playSound3D(victim, sounds[Misc::Rng::rollDice(sounds.size(), prng)], 1.0f, 1.0f);
        }
    }

    void applyFatigueLoss(const MWWorld::Ptr& attacker, const MWWorld::Ptr& weapon, float attackStrength)
    {
        // somewhat of a guess, but using the weapon weight makes sense
        const MWWorld::Store<ESM::GameSetting>& store
            = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>();
        static const float fFatigueAttackBase = store.find("fFatigueAttackBase")->mValue.getFloat();
        static const float fFatigueAttackMult = store.find("fFatigueAttackMult")->mValue.getFloat();
        static const float fWeaponFatigueMult = store.find("fWeaponFatigueMult")->mValue.getFloat();
        CreatureStats& stats = attacker.getClass().getCreatureStats(attacker);
        MWMechanics::DynamicStat<float> fatigue = stats.getFatigue();
        const float normalizedEncumbrance = attacker.getClass().getNormalizedEncumbrance(attacker);

        bool godmode = attacker == MWMechanics::getPlayer() && MWBase::Environment::get().getWorld()->getGodModeState();

        if (!godmode)
        {
            float fatigueLoss = fFatigueAttackBase + normalizedEncumbrance * fFatigueAttackMult;
            if (!weapon.isEmpty())
                fatigueLoss += weapon.getClass().getWeight(weapon) * attackStrength * fWeaponFatigueMult;
            fatigueLoss*=Ragdoll::Combat::resource(getCombatSkill(attacker,weapon));
            fatigue.setCurrent(std::max(0.f,fatigue.getCurrent() - fatigueLoss));
            Ragdoll::Combat::observeFatigue(stats.mCombat,fatigue.getCurrent(),
                stats.getAttribute(ESM::Attribute::Agility).getModified(),
                MWBase::Environment::get().getWorld()->getTimeManager()->getSimulationTime(),true);
            stats.setFatigue(fatigue);
        }
    }

    float getFightDistanceBias(const MWWorld::Ptr& actor1, const MWWorld::Ptr& actor2)
    {
        osg::Vec3f pos1(actor1.getRefData().getPosition().asVec3());
        osg::Vec3f pos2(actor2.getRefData().getPosition().asVec3());

        float d = getAggroDistance(actor1, pos1, pos2);

        static const int iFightDistanceBase = MWBase::Environment::get()
                                                  .getESMStore()
                                                  ->get<ESM::GameSetting>()
                                                  .find("iFightDistanceBase")
                                                  ->mValue.getInteger();
        static const float fFightDistanceMultiplier = MWBase::Environment::get()
                                                          .getESMStore()
                                                          ->get<ESM::GameSetting>()
                                                          .find("fFightDistanceMultiplier")
                                                          ->mValue.getFloat();

        return (iFightDistanceBase - fFightDistanceMultiplier * d);
    }

    float getAggroDistance(const MWWorld::Ptr& actor, const osg::Vec3f& lhs, const osg::Vec3f& rhs)
    {
        if (canActorMoveByZAxis(actor))
            return distanceIgnoreZ(lhs, rhs);
        return distance(lhs, rhs);
    }

    float getDistanceToBounds(const MWWorld::Ptr& actor, const MWWorld::Ptr& target)
    {
        osg::Vec3f actorPos(actor.getRefData().getPosition().asVec3());
        osg::Vec3f targetPos(target.getRefData().getPosition().asVec3());
        MWBase::World* world = MWBase::Environment::get().getWorld();

        float dist = (targetPos - actorPos).length();
        dist -= world->getHalfExtents(actor).y();
        dist -= world->getHalfExtents(target).y();
        return dist;
    }

    float getMeleeWeaponReach(const MWWorld::Ptr& actor, const MWWorld::Ptr& weapon)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::Store<ESM::GameSetting>& store = world->getStore().get<ESM::GameSetting>();
        const float fCombatDistance = store.find("fCombatDistance")->mValue.getFloat();
        if (!weapon.isEmpty())
            return fCombatDistance * weapon.get<ESM::Weapon>()->mBase->mData.mReach;
        if (actor.getClass().isNpc())
            return fCombatDistance * store.find("fHandToHandReach")->mValue.getFloat();
        return fCombatDistance;
    }

    bool isInMeleeReach(const MWWorld::Ptr& actor, const MWWorld::Ptr& target, const float reach)
    {
        const float heightDiff = actor.getRefData().getPosition().pos[2] - target.getRefData().getPosition().pos[2];
        return std::abs(heightDiff) < reach && getDistanceToBounds(actor, target) < reach;
    }

    namespace
    {
        bool weaponSegment(const MWWorld::Ptr& actor,osg::Vec3f& base,osg::Vec3f& tip)
        {
            auto* animation=MWBase::Environment::get().getWorld()->getAnimation(actor);
            auto* weapon=dynamic_cast<MWRender::WeaponAnimation*>(animation);
            auto* node=weapon?weapon->getContactWeaponNode():nullptr;
            if(!node)return false;
            return Ragdoll::Combat::weaponSegment(*node,actor.getRefData().getPosition().asVec3(),base,tip);
        }
        std::pair<MWWorld::Ptr,osg::Vec3f> contactRay(const MWWorld::Ptr& actor,const osg::Vec3f& from,const osg::Vec3f& to)
        {
            const auto world=MWBase::Environment::get().getWorld();
            MWPhysics::RayCastingResult hit{};
            const std::array<MWWorld::Ptr,1> ignored{actor};
            world->castRenderingRay(hit,from,to,actor==getPlayer(),false,ignored);
            if(!hit.mHit)hit=world->getRayCasting()->castRay(from,to,{actor});
            else if(actor!=getPlayer() && (hit.mHitObject.isEmpty() || !hit.mHitObject.getClass().isActor()))
            {
                // The first-person player has no visible torso. A wall behind
                // the hidden player must not make the intervening body unhittable.
                const auto physical=world->getRayCasting()->castRay(from,to,{actor});
                if(physical.mHit && physical.mHitObject==getPlayer()
                    && (physical.mHitPos-from).length2()<(hit.mHitPos-from).length2())hit=physical;
            }
            if(hit.mHit && !hit.mHitObject.isEmpty() && hit.mHitObject.getClass().isActor()
                && !hit.mHitObject.getClass().getCreatureStats(hit.mHitObject).isDead()
                && world->getLOS(actor,hit.mHitObject))return {hit.mHitObject,hit.mHitPos};
            return {};
        }
    }
    void sampleMeleeContact(const MWWorld::Ptr& actor)
    {
        auto& stats=actor.getClass().getCreatureStats(actor);auto& combat=stats.mCombat;
        if(!combat.mAttackActive){combat.mHasWeaponSample=false;return;}
        osg::Vec3f base,tip;
        if(!weaponSegment(actor,base,tip)){combat.mHasWeaponSample=false;return;}
        if(combat.mAttackActive && combat.mContactWindow)
        {
            std::pair<MWWorld::Ptr,osg::Vec3f> hit;
            Ragdoll::Combat::sweepWeapon(base,tip,combat.mWeaponBase,combat.mWeaponTip,combat.mHasWeaponSample,
                [&](const osg::Vec3f& from,const osg::Vec3f& to){hit=contactRay(actor,from,to);return !hit.first.isEmpty();});
            if(!hit.first.isEmpty())
            {stats.mMeleeContact=hit.first;stats.mMeleeContactPoint=hit.second;
                combat.mContactTime=MWBase::Environment::get().getWorld()->getTimeManager()->getSimulationTime();}
        }
        combat.mHasWeaponSample=true;combat.mWeaponBase=base;combat.mWeaponTip=tip;
    }
    std::pair<MWWorld::Ptr,osg::Vec3f> getHitContact(const MWWorld::Ptr& actor,float reach)
    {
        auto& stats=actor.getClass().getCreatureStats(actor);const auto world=MWBase::Environment::get().getWorld();
        const double now=world->getTimeManager()->getSimulationTime();
        if(!stats.mMeleeContact.isEmpty() && stats.mCombat.mContactTime>=stats.mCombat.mAttackStarted
            && now-stats.mCombat.mContactTime<=.12 && world->getLOS(actor,stats.mMeleeContact))
            return {stats.mMeleeContact,stats.mMeleeContactPoint};
        osg::Vec3f base,tip;
        if(weaponSegment(actor,base,tip))return contactRay(actor,base,tip);
        // Unarmed/unsupported models use a narrow physical contact ray rather
        // than the old broad cone which could hit an untouched neighbour.
        auto origin=osg::Vec3f(world->getActorHeadTransform(actor).getTrans());
        const auto& pos=actor.getRefData().getPosition();
        auto direction=MWPhysics::itemAimDirection(pos.rot[0],pos.rot[2]);
        if(actor==getPlayer() && world->getCamera())
            std::tie(origin,direction)=MWPhysics::itemViewRay(world->getCamera()->getViewMatrix());
        return contactRay(actor,origin,origin+direction*reach);
    }

    std::pair<MWWorld::Ptr, osg::Vec3f> getItemHitContact(const MWWorld::Ptr& actor, float reach)
    {
        if (!Settings::ragdoll().mItemPhysics)
            return {};
        auto world = MWBase::Environment::get().getWorld();
        const auto& position = actor.getRefData().getPosition();
        osg::Vec3f origin = world->getActorHeadTransform(actor).getTrans();
        if (!std::isfinite(origin.length2()) || (origin - position.asVec3()).length2() > 1024.f * 1024.f
            || origin == osg::Vec3f())
            origin = position.asVec3() + osg::Vec3f(0, 0, world->getHalfExtents(actor, true).z() * 1.7f);
        osg::Vec3f direction = MWPhysics::itemAimDirection(position.rot[0], position.rot[2]);
        if (actor == world->getPlayerPtr())
        {
            const auto* camera = world->getCamera();
            if (camera && !camera->isVanityOrPreviewModeEnabled())
            {
                // Match the frame the player aimed at, including first-person
                // neck movement and camera mods. The GL camera looks down -Z.
                const auto [eye, forward] = MWPhysics::itemViewRay(camera->getViewMatrix());
                if (std::isfinite(eye.length2()) && (eye - position.asVec3()).length2() < 4096.f * 4096.f)
                {
                    if (camera->getMode() == MWRender::Camera::Mode::FirstPerson)
                    {
                        origin = eye;
                        direction = forward;
                    }
                    else
                    {
                        // Third-person aim may be over a shoulder. Aim at its
                        // visible surface, then test the physical path from the
                        // actor, so the camera cannot hit around/through a wall.
                        MWPhysics::RayCastingResult visible{};
                        if (world->castRenderingRay(
                                visible, eye, eye + forward * (reach + (eye - origin).length()), true, false))
                            direction = visible.mHitPos - origin;
                    }
                }
            }
        }
        const auto contact = MWPhysics::itemMeleeContact(*world->getRayCasting(), origin, direction, reach, actor,
            position.asVec3() + osg::Vec3f(0, 0, world->getHalfExtents(actor, true).z()));
        return { contact.mHitObject, contact.mHitPos };
    }

    bool friendlyHit(const MWWorld::Ptr& attacker, const MWWorld::Ptr& target, bool complain)
    {
        const MWWorld::Ptr& player = getPlayer();
        if (attacker != player)
            return false;

        std::set<MWWorld::Ptr> followersAttacker;
        MWBase::Environment::get().getMechanicsManager()->getActorsSidingWith(attacker, followersAttacker);
        if (followersAttacker.find(target) == followersAttacker.end())
            return false;

        MWMechanics::CreatureStats& statsTarget = target.getClass().getCreatureStats(target);
        if (statsTarget.getAiSequence().isInCombat())
            return true;
        statsTarget.friendlyHit();
        if (statsTarget.getFriendlyHits() >= 4)
            return false;

        if (complain)
            MWBase::Environment::get().getDialogueManager()->say(target, ESM::RefId::stringRefId("hit"));
        return true;
    }

}
