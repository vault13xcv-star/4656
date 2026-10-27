#include "projectilemanager.hpp"

#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>

#include <osg/PositionAttitudeTransform>
#include <osg/ComputeBoundsVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <osgUtil/IntersectionVisitor>
#include <components/ragdoll/projectiles.hpp>
#include <components/ragdoll/blood.hpp>

#include <components/debug/debuglog.hpp>

#include <components/esm3/actoridconverter.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm3/projectilestate.hpp>

#include <components/esm/quaternion.hpp>
#include <components/esm/vector3.hpp>

#include <components/misc/constants.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/visitor.hpp>

#include <components/settings/values.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/worldmodel.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "datetimemanager.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/combat.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/spellcasting.hpp"

#include "../mwrender/animation.hpp"
#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/ragdoll.hpp"
#include "../mwrender/util.hpp"
#include "../mwrender/vismask.hpp"

#include "../mwsound/sound.hpp"

#include "../mwphysics/physicssystem.hpp"
#include "../mwphysics/projectile.hpp"

namespace
{
    bool actorSurface(MWRender::Animation& animation,const osg::Vec3f& from,const osg::Vec3f& to,
        osg::Vec3f& point)
    {
        auto* root=animation.getObjectRoot();
        if(!root)return false;
        osg::ref_ptr<osgUtil::LineSegmentIntersector> ray=new osgUtil::LineSegmentIntersector(from,to);
        ray->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);
        osgUtil::IntersectionVisitor visitor(ray);
        visitor.setTraversalMask(~MWRender::Mask_Effect);
        auto paths=root->getParentalNodePaths();
        if(!paths.empty())
        {
            paths.front().pop_back();
            visitor.pushModelMatrix(new osg::RefMatrix(osg::computeLocalToWorld(paths.front())));
        }
        root->accept(visitor);
        if(!ray->containsIntersections())return false;
        point=ray->getFirstIntersection().getWorldIntersectPoint();return true;
    }
    ESM::EffectList getMagicBoltData(std::vector<ESM::RefId>& projectileIDs, std::set<ESM::RefId>& sounds, float& speed,
        std::string& texture, std::string& sourceName, const ESM::RefId& id)
    {
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        const ESM::EffectList* effects;
        if (const ESM::Spell* spell = esmStore.get<ESM::Spell>().search(id)) // check if it's a spell
        {
            sourceName = spell->mName;
            effects = &spell->mEffects;
        }
        else // check if it's an enchanted item
        {
            MWWorld::ManualRef ref(esmStore, id);
            MWWorld::Ptr ptr = ref.getPtr();
            const ESM::Enchantment* ench = esmStore.get<ESM::Enchantment>().find(ptr.getClass().getEnchantment(ptr));
            sourceName = ptr.getClass().getName(ptr);
            effects = &ench->mEffects;
        }

        int count = 0;
        speed = 0.0f;
        ESM::EffectList projectileEffects;
        for (const ESM::IndexedENAMstruct& effect : effects->mList)
        {
            const ESM::MagicEffect* magicEffect
                = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(effect.mData.mEffectID);

            // Speed of multi-effect projectiles should be the average of the constituent effects,
            // based on observation of the original engine.
            speed += magicEffect->mData.mSpeed;
            count++;

            if (effect.mData.mRange != ESM::RT_Target)
                continue;

            if (magicEffect->mBolt.empty())
                projectileIDs.emplace_back(ESM::RefId::stringRefId("VFX_DefaultBolt"));
            else
                projectileIDs.push_back(magicEffect->mBolt);

            if (!magicEffect->mBoltSound.empty())
                sounds.emplace(magicEffect->mBoltSound);
            else
                sounds.emplace(MWBase::Environment::get()
                                   .getESMStore()
                                   ->get<ESM::Skill>()
                                   .find(magicEffect->mData.mSchool)
                                   ->mSchool->mBoltSound);
            projectileEffects.mList.push_back(effect);
        }

        if (count != 0)
            speed /= count;

        // the particle texture is only used if there is only one projectile
        if (projectileEffects.mList.size() == 1)
        {
            const ESM::MagicEffect* magicEffect
                = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(
                    effects->mList.begin()->mData.mEffectID);
            texture = magicEffect->mParticle;
        }

        // insert a VFX_Multiple projectile if there are multiple projectile effects
        if (projectileEffects.mList.size() > 1)
        {
            const ESM::RefId projectileId
                = ESM::RefId::stringRefId("VFX_Multiple" + std::to_string(effects->mList.size()));
            projectileIDs.insert(projectileIDs.begin(), projectileId);
        }

        return projectileEffects;
    }

    osg::Vec4 getMagicBoltLightDiffuseColor(const ESM::EffectList& effects)
    {
        // Calculate combined light diffuse color from magical effects
        osg::Vec4 lightDiffuseColor;
        for (const ESM::IndexedENAMstruct& enam : effects.mList)
        {
            const ESM::MagicEffect* magicEffect
                = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(enam.mData.mEffectID);
            lightDiffuseColor += magicEffect->getColor();
        }
        size_t numberOfEffects = effects.mList.size();
        lightDiffuseColor /= static_cast<float>(numberOfEffects);

        return lightDiffuseColor;
    }

    osg::Quat lookAt(const osg::Vec3f& pos)
    {
        // Rotate the forward vector towards the position (used for gravity-affected projectiles)
        // Can't use Quat::makeRotate as the shortest angle contains undesirable local roll
        const float dist = pos.length();
        if (dist < 1e-4f)
            return {};

        const osg::Vec3f dir = pos / dist;
        osg::Vec3f right = dir ^ osg::Z_AXIS;
        if (right.normalize() < 1e-4f)
            right = osg::X_AXIS;

        const osg::Vec3f up = right ^ dir;

        osg::Matrixf mat(right.x(), right.y(), right.z(), 0.f, dir.x(), dir.y(), dir.z(), 0.f, up.x(), up.y(), up.z(),
            0.f, 0.f, 0.f, 0.f, 1.f);

        osg::Quat orient;
        orient.set(mat);
        return orient;
    }
}

namespace MWWorld
{

    ProjectileManager::ProjectileManager(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
        MWRender::RenderingManager* rendering, MWPhysics::PhysicsSystem* physics)
        : mParent(parent)
        , mResourceSystem(resourceSystem)
        , mRendering(rendering)
        , mPhysics(physics)
        , mCleanupTimer(0.0f)
    {
    }

    /// Rotates an osg::PositionAttitudeTransform over time.
    class RotateCallback : public SceneUtil::NodeCallback<RotateCallback, osg::PositionAttitudeTransform*>
    {
    public:
        RotateCallback(const osg::Vec3f& axis = osg::Vec3f(0, -1, 0), float rotateSpeed = osg::PI * 2)
            : mAxis(axis)
            , mRotateSpeed(rotateSpeed)
        {
        }

        void operator()(osg::PositionAttitudeTransform* node, osg::NodeVisitor* nv)
        {
            double time = nv->getFrameStamp()->getSimulationTime();

            osg::Quat orient = osg::Quat(time * mRotateSpeed, mAxis);
            node->setAttitude(orient);

            traverse(node, nv);
        }

    private:
        osg::Vec3f mAxis;
        float mRotateSpeed;
    };

    void ProjectileManager::createModel(State& state, VFS::Path::NormalizedView model, const osg::Vec3f& pos,
        const osg::Quat& orient, bool rotate, bool createLight, osg::Vec4 lightDiffuseColor, const std::string& texture)
    {
        state.mNode = new osg::PositionAttitudeTransform;
        state.mNode->setNodeMask(MWRender::Mask_Effect);
        state.mNode->setPosition(pos);
        state.mNode->setAttitude(orient);

        osg::Group* attachTo = state.mNode;

        if (rotate)
        {
            osg::ref_ptr<osg::PositionAttitudeTransform> rotateNode(new osg::PositionAttitudeTransform);
            rotateNode->addUpdateCallback(new RotateCallback());
            state.mNode->addChild(rotateNode);
            attachTo = rotateNode;
        }

        osg::ref_ptr<osg::Node> projectile = mResourceSystem->getSceneManager()->getInstance(model, attachTo);

        if (state.mIdMagic.size() > 1)
        {
            for (size_t iter = 1; iter != state.mIdMagic.size(); ++iter)
            {
                std::ostringstream nodeName;
                nodeName << "Dummy" << std::setw(2) << std::setfill('0') << iter;
                const ESM::Weapon* weapon
                    = MWBase::Environment::get().getESMStore()->get<ESM::Weapon>().find(state.mIdMagic.at(iter));
                std::string nameToFind = nodeName.str();
                SceneUtil::FindByNameVisitor findVisitor(nameToFind);
                attachTo->accept(findVisitor);
                if (findVisitor.mFoundNode)
                    mResourceSystem->getSceneManager()->getInstance(
                        Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(weapon->mModel)),
                        findVisitor.mFoundNode);
            }
        }

        if (createLight)
        {
            osg::ref_ptr<osg::Light> projectileLight(new osg::Light);
            projectileLight->setAmbient(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
            projectileLight->setDiffuse(lightDiffuseColor);
            projectileLight->setSpecular(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
            projectileLight->setConstantAttenuation(0.f);
            projectileLight->setLinearAttenuation(0.1f);
            projectileLight->setQuadraticAttenuation(0.f);
            projectileLight->setPosition(osg::Vec4(pos, 1.0));

            SceneUtil::LightSource* projectileLightSource = new SceneUtil::LightSource;
            projectileLightSource->setNodeMask(MWRender::Mask_Lighting);
            projectileLightSource->setRadius(66.f);

            state.mNode->addChild(projectileLightSource);
            projectileLightSource->setLight(projectileLight);
        }

        state.mNode->addCullCallback(new SceneUtil::LightListCallback);

        mParent->addChild(state.mNode);

        state.mEffectAnimationTime = std::make_shared<MWRender::EffectAnimationTime>();

        SceneUtil::AssignControllerSourcesVisitor assignVisitor(state.mEffectAnimationTime);
        state.mNode->accept(assignVisitor);

        MWRender::overrideFirstRootTexture(VFS::Path::toNormalized(texture), mResourceSystem, *projectile);
    }

    void ProjectileManager::update(State& state, float duration)
    {
        state.mEffectAnimationTime->addTime(duration);
    }

    void ProjectileManager::launchMagicBolt(
        const ESM::RefId& spellId, const Ptr& caster, const osg::Vec3f& fallbackDirection, ESM::RefNum item)
    {
        osg::Vec3f pos = caster.getRefData().getPosition().asVec3();
        if (caster.getClass().isActor())
        {
            // Note: we ignore the collision box offset, this is required to make some flying creatures work as
            // intended.
            pos.z() += mPhysics->getRenderingHalfExtents(caster).z() * 2 * Constants::TorsoHeight;
        }

        // Actors can't cast target spells underwater
        if (caster.getClass().isActor() && MWBase::Environment::get().getWorld()->isUnderwater(caster.getCell(), pos))
            return;

        osg::Quat orient;
        if (caster.getClass().isActor())
            orient = osg::Quat(caster.getRefData().getPosition().rot[0], osg::Vec3f(-1, 0, 0))
                * osg::Quat(caster.getRefData().getPosition().rot[2], osg::Vec3f(0, 0, -1));
        else
            orient.makeRotate(osg::Vec3f(0, 1, 0), osg::Vec3f(fallbackDirection));

        MagicBoltState state;
        state.mSpellId = spellId;
        state.mCasterHandle = caster;
        state.mItem = item;
        MWBase::Environment::get().getWorldModel()->registerPtr(caster);
        state.mCaster = caster.getCellRef().getRefNum();

        std::string texture;

        state.mEffects = getMagicBoltData(
            state.mIdMagic, state.mSoundIds, state.mSpeed, texture, state.mSourceName, state.mSpellId);

        // Non-projectile should have been removed by getMagicBoltData
        if (state.mEffects.mList.empty())
            return;

        if (!caster.getClass().isActor() && fallbackDirection.length2() <= 0)
        {
            Log(Debug::Warning) << "Unable to launch magic bolt (direction to target is empty)";
            return;
        }

        MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), state.mIdMagic.at(0));
        MWWorld::Ptr ptr = ref.getPtr();

        osg::Vec4 lightDiffuseColor = getMagicBoltLightDiffuseColor(state.mEffects);

        VFS::Path::Normalized model = ptr.getClass().getCorrectedModel(ptr);
        createModel(state, model, pos, orient, true, true, lightDiffuseColor, texture);

        MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
        for (const auto& soundid : state.mSoundIds)
        {
            MWBase::Sound* sound
                = sndMgr->playSound3D(pos, soundid, 1.0f, 1.0f, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
            if (sound)
                state.mSounds.push_back(sound);
        }

        // in case there are multiple effects, the model is a dummy without geometry. Use the second effect for physics
        // shape
        if (state.mIdMagic.size() > 1)
        {
            model = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(
                MWBase::Environment::get().getESMStore()->get<ESM::Weapon>().find(state.mIdMagic[1])->mModel));
        }
        state.mProjectileId = mPhysics->addProjectile(caster, pos, model, true);
        state.mToDelete = false;
        mMagicBolts.push_back(std::move(state));
    }

    void ProjectileManager::launchProjectile(const Ptr& actor, const ConstPtr& projectile, const osg::Vec3f& pos,
        const osg::Quat& orient, const Ptr& bow, float speed, float attackStrength)
    {
        ProjectileState state;
        state.mCaster = actor.getCellRef().getRefNum();
        state.mBowId = bow.getCellRef().getRefId();
        state.mVelocity = orient * osg::Vec3f(0, 1, 0) * speed;
        state.mIdArrow = projectile.getCellRef().getRefId();
        state.mCasterHandle = actor;
        state.mAttackStrength = attackStrength;

        MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), projectile.getCellRef().getRefId());
        MWWorld::Ptr ptr = ref.getPtr();

        const VFS::Path::Normalized model = ptr.getClass().getCorrectedModel(ptr);
        createModel(state, model, pos, orient, false, false, osg::Vec4(0, 0, 0, 0));
        if (!ptr.getClass().getEnchantment(ptr).empty())
            SceneUtil::addEnchantedGlow(state.mNode, mResourceSystem, ptr.getClass().getEnchantmentColor(ptr));

        state.mProjectileId = mPhysics->addProjectile(actor, pos, model, false);
        state.mToDelete = false;
        mProjectiles.push_back(std::move(state));
    }

    void ProjectileManager::updateCasters()
    {
        for (auto& state : mProjectiles)
        {
            state.mCasterHandle = state.getCaster();
            mPhysics->setCaster(state.mProjectileId, state.mCasterHandle);
        }

        for (auto& state : mMagicBolts)
        {
            if (!state.mCaster.isSet())
                continue;

            state.mCasterHandle = state.getCaster();
            if (state.mCasterHandle.isEmpty())
            {
                Log(Debug::Error) << "Couldn't find caster with ID " << state.mCaster;
                cleanupMagicBolt(state);
                continue;
            }
            mPhysics->setCaster(state.mProjectileId, state.mCasterHandle);
        }
    }

    void ProjectileManager::update(float dt)
    {
        periodicCleanup(dt);
        moveProjectiles(dt);
        moveMagicBolts(dt);
    }

    void ProjectileManager::periodicCleanup(float dt)
    {
        mCleanupTimer -= dt;
        if (mCleanupTimer <= 0.0f)
        {
            mCleanupTimer = 2.0f;

            auto isCleanable = [](const ProjectileManager::State& state) -> bool {
                const float farawayThreshold = 72000.0f;
                osg::Vec3 playerPos = MWMechanics::getPlayer().getRefData().getPosition().asVec3();
                return (state.mNode->getPosition() - playerPos).length2() >= farawayThreshold * farawayThreshold;
            };

            for (auto& projectileState : mProjectiles)
            {
                if (isCleanable(projectileState))
                    cleanupProjectile(projectileState);
            }

            for (auto& magicBoltState : mMagicBolts)
            {
                if (isCleanable(magicBoltState))
                    cleanupMagicBolt(magicBoltState);
            }
        }
    }

    void ProjectileManager::moveMagicBolts(float duration)
    {
        const bool normaliseRaceSpeed = Settings::game().mNormaliseRaceSpeed;
        for (auto& magicBoltState : mMagicBolts)
        {
            if (magicBoltState.mToDelete)
                continue;

            auto* projectile = mPhysics->getProjectile(magicBoltState.mProjectileId);
            if (!projectile->isActive())
                continue;
            // If the actor caster is gone, the magic bolt needs to be removed from the scene during the next frame.
            MWWorld::Ptr caster = magicBoltState.getCaster();
            if (!caster.isEmpty() && caster.getClass().isActor())
            {
                if (caster.getCellRef().getCount() <= 0 || caster.getClass().getCreatureStats(caster).isDead())
                {
                    cleanupMagicBolt(magicBoltState);
                    continue;
                }
            }

            const auto& store = *MWBase::Environment::get().getESMStore();
            osg::Quat orient = magicBoltState.mNode->getAttitude();
            static float fTargetSpellMaxSpeed
                = store.get<ESM::GameSetting>().find("fTargetSpellMaxSpeed")->mValue.getFloat();
            float speed = fTargetSpellMaxSpeed * magicBoltState.mSpeed;
            if (!normaliseRaceSpeed && !caster.isEmpty() && caster.getClass().isNpc())
            {
                const auto npc = caster.get<ESM::NPC>()->mBase;
                const auto race = store.get<ESM::Race>().find(npc->mRace);
                speed *= npc->isMale() ? race->mData.mMaleWeight : race->mData.mFemaleWeight;
            }
            osg::Vec3f direction = orient * osg::Vec3f(0, 1, 0);
            direction.normalize();
            projectile->setVelocity(direction * speed);

            update(magicBoltState, duration);

            for (const auto& sound : magicBoltState.mSounds)
            {
                sound->setVelocity(direction * speed);
            }

            // For AI actors, get combat targets to use in the ray cast. Only those targets will return a positive hit
            // result.
            std::vector<MWWorld::Ptr> targetActors;
            if (!caster.isEmpty() && caster.getClass().isActor() && caster != MWMechanics::getPlayer())
                caster.getClass().getCreatureStats(caster).getAiSequence().getCombatTargets(targetActors);
            projectile->setValidTargets(targetActors);
        }
    }

    void ProjectileManager::moveProjectiles(float duration)
    {
        for (auto& projectileState : mProjectiles)
        {
            if (projectileState.mToDelete)
                continue;

            auto* projectile = mPhysics->getProjectile(projectileState.mProjectileId);
            if (!projectile->isActive())
                continue;
            // gravity constant - must be way lower than the gravity affecting actors, since we're not
            // simulating aerodynamics at all
            projectileState.mVelocity
                -= osg::Vec3f(0, 0, Constants::GravityConst * Constants::UnitsPerMeter) * duration;

            projectile->setVelocity(projectileState.mVelocity);

            const auto* record=MWBase::Environment::get().getESMStore()->get<ESM::Weapon>().find(projectileState.mIdArrow);
            if(record->mData.mType==ESM::Weapon::MarksmanThrown)
                projectileState.mSpin += duration*12.f;
            projectileState.mNode->setAttitude(osg::Quat(projectileState.mSpin,osg::Vec3f(1,0,0))
                *lookAt(projectileState.mVelocity));

            update(projectileState, duration);

            MWWorld::Ptr caster = projectileState.getCaster();

            // For AI actors, get combat targets to use in the ray cast. Only those targets will return a positive hit
            // result.
            std::vector<MWWorld::Ptr> targetActors;
            if (!caster.isEmpty() && caster.getClass().isActor() && caster != MWMechanics::getPlayer())
                caster.getClass().getCreatureStats(caster).getAiSequence().getCombatTargets(targetActors);
            projectile->setValidTargets(targetActors);
        }
    }

    void ProjectileManager::processHits()
    {
        for (auto& projectileState : mProjectiles)
        {
            if (projectileState.mToDelete)
                continue;

            auto* projectile = mPhysics->getProjectile(projectileState.mProjectileId);

            const osg::Vec3f previous=projectileState.mNode->getPosition();
            const auto pos = projectile->getSimulationPosition();
            MWWorld::Ptr corpse;
            osg::Vec3f corpsePoint;
            const osg::Vec3f end=projectile->isActive()?pos:osg::Vec3f(Misc::Convert::toOsg(projectile->getHitPosition()));
            float nearest=(end-previous).length2()+.01f;
            // Corpse collision is disabled by the actor controller. Intersect
            // the visible corpse as well, bounded by the first native solid hit.
            std::vector<MWWorld::Ptr> nearby;
            MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
                (previous+end)*.5f,(end-previous).length()*.5f+768.f,nearby);
            for(const auto& actor:nearby)
            {
                if(actor==projectileState.getCaster() || !actor.getClass().getCreatureStats(actor).isDead())continue;
                if(auto* animation=mRendering->getAnimation(actor))
                {
                    osg::Vec3f point;
                    bool contact=actorSurface(*animation,previous,end,point);
                    if(!contact && actor.getClass().isNpc() && animation->getObjectRoot())
                    {
                        const auto hit=MWRender::RagdollController::findHitContact(*animation->getObjectRoot(),previous,end);
                        contact=hit.mBody>=0;if(contact)point=hit.mPosition;
                    }
                    if(contact && (point-previous).length2()<nearest)
                    {corpse=actor;corpsePoint=point;nearest=(point-previous).length2();}
                }
            }
            projectileState.mNode->setPosition(pos);
            if (projectile->isActive() && corpse.isEmpty())continue;

            const auto target = corpse.isEmpty()?projectile->getTarget():corpse;
            auto caster = projectileState.getCaster();
            assert(target != caster);

            if (caster.isEmpty())
                caster = target;

            // Try to get a Ptr to the bow that was used. It might no longer exist.
            MWWorld::ManualRef projectileRef(*MWBase::Environment::get().getESMStore(), projectileState.mIdArrow);
            MWWorld::Ptr bow = projectileRef.getPtr();
            if (!caster.isEmpty() && projectileState.mIdArrow != projectileState.mBowId)
            {
                MWWorld::InventoryStore& inv = caster.getClass().getInventoryStore(caster);
                MWWorld::ContainerStoreIterator invIt = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                if (invIt != inv.end() && invIt->getCellRef().getRefId() == projectileState.mBowId)
                    bow = *invIt;
            }

            const osg::Vec3f hitPosition = corpse.isEmpty()?osg::Vec3f(Misc::Convert::toOsg(projectile->getHitPosition())):corpsePoint;

            if (projectile->getHitWater() && corpse.isEmpty())
                mRendering->emitWaterRipple(hitPosition);

            if(target.isEmpty() || !target.getClass().isActor() || !target.getClass().getCreatureStats(target).isDead())
                MWMechanics::projectileHit(caster, target, bow, projectileRef.getPtr(), hitPosition,
                    projectileState.mAttackStrength,projectileState.mVelocity);
            if(!projectile->getHitWater() || !corpse.isEmpty())
                retainProjectile(projectileState,target,hitPosition,corpse.isEmpty()
                    ?osg::Vec3f(Misc::Convert::toOsg(projectile->getHitNormal())):osg::Vec3f());
            projectileState.mToDelete = true;
        }
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        for (auto& magicBoltState : mMagicBolts)
        {
            if (magicBoltState.mToDelete)
                continue;

            auto* projectile = mPhysics->getProjectile(magicBoltState.mProjectileId);

            const auto pos = projectile->getSimulationPosition();
            magicBoltState.mNode->setPosition(pos);
            for (const auto& sound : magicBoltState.mSounds)
                sound->setPosition(pos);

            const Ptr caster = magicBoltState.getCaster();

            const MWBase::World& world = *MWBase::Environment::get().getWorld();
            const bool active = projectile->isActive();
            if (active && !world.isUnderwater(caster.getCell(), pos))
                continue;

            const Ptr target = !active ? projectile->getTarget() : Ptr();

            assert(target != caster);

            MWMechanics::CastSpell cast(caster, target);
            cast.mHitPosition = !active ? Misc::Convert::makeOsgVec3f(projectile->getHitPosition()) : pos;
            cast.mImpactDirection = magicBoltState.mNode->getAttitude() * osg::Vec3f(0, 1, 0);
            cast.mId = magicBoltState.mSpellId;
            cast.mSourceName = magicBoltState.mSourceName;
            cast.mItem = magicBoltState.mItem;
            // Grab original effect list so the indices are correct
            const ESM::EffectList* effects;
            if (const ESM::Spell* spell = esmStore.get<ESM::Spell>().search(magicBoltState.mSpellId))
                effects = &spell->mEffects;
            else
            {
                MWWorld::ManualRef ref(esmStore, magicBoltState.mSpellId);
                const MWWorld::Ptr& ptr = ref.getPtr();
                effects = &esmStore.get<ESM::Enchantment>().find(ptr.getClass().getEnchantment(ptr))->mEffects;
            }
            cast.inflict(target, *effects, ESM::RT_Target);

            magicBoltState.mToDelete = true;
        }

        for (auto& projectileState : mProjectiles)
        {
            if (projectileState.mToDelete)
                cleanupProjectile(projectileState);
        }

        for (auto& magicBoltState : mMagicBolts)
        {
            if (magicBoltState.mToDelete)
                cleanupMagicBolt(magicBoltState);
        }
        mProjectiles.erase(std::remove_if(mProjectiles.begin(), mProjectiles.end(),
                               [](const State& state) { return state.mToDelete; }),
            mProjectiles.end());
        mMagicBolts.erase(
            std::remove_if(mMagicBolts.begin(), mMagicBolts.end(), [](const State& state) { return state.mToDelete; }),
            mMagicBolts.end());
    }

    void ProjectileManager::retainProjectile(ProjectileState& state,const MWWorld::Ptr& target,
        const osg::Vec3f& contact,const osg::Vec3f& normal)
    {
        auto residue=std::make_unique<Residue>();
        residue->mNode=state.mNode;
        residue->mCell=MWMechanics::getPlayer().getCell();
        osg::ComputeBoundsVisitor bounds;
        for(unsigned i=0;i<state.mNode->getNumChildren();++i)state.mNode->getChild(i)->accept(bounds);
        auto box=bounds.getBoundingBox();
        if(!box.valid())box=osg::BoundingBox(-.5f,-8.f,-.5f,.5f,8.f,.5f);
        osg::Vec3f direction=state.mVelocity;
        if(direction.normalize()<.001f)direction={0,1,0};
        osg::Vec3f point=contact;
        const bool actor=!target.isEmpty() && target.getClass().isActor();
        if(actor)
        {
            residue->mVictim=target;
            auto* animation=mRendering->getAnimation(target);
            if(animation)
            {
                osg::Vec3f surface;
                if(actorSurface(*animation,contact-direction*8.f,contact+direction*80.f,surface))point=surface;
                else if(auto* root=animation->getObjectRoot())
                {
                    const auto hit=MWRender::RagdollController::findHitContact(*root,
                        contact-direction*8.f,contact+direction*80.f);
                    if(hit.mBody>=0)point=hit.mPosition;
                }
                residue->mBone=animation->findProjectileBone(point);
            }
            float tip=-1e9f;
            for(unsigned i=0;i<8;++i)
                tip=std::max(tip,float((state.mNode->getAttitude()*box.corner(i))*direction));
            state.mNode->setPosition(point+direction*(3.f-tip));
            osg::Matrixf bone;
            if(animation && animation->getProjectileBoneMatrix(residue->mBone,bone))
            {
                residue->mLocal=osg::Matrixf::rotate(state.mNode->getAttitude())
                    *osg::Matrixf::translate(state.mNode->getPosition())*osg::Matrixf::inverse(bone);
                residue->mWoundPosition=point;
                residue->mWoundLocal=point*osg::Matrixf::inverse(bone);
            }
            else residue->mVictim=MWWorld::Ptr();
            bool organic=target.getClass().isNpc();
            if(target.getType()==ESM::Creature::sRecordId)
            {
                const auto& record=*target.get<ESM::Creature>()->mBase;
                organic=Ragdoll::creatureHasBlood(record.mId.toString(),record.mOriginal.toString(),record.mModel);
            }
            if(organic && !residue->mVictim.isEmpty())residue->mBleedLeft=5.f*Settings::ragdoll().mStatusDurationScale.get();
        }
        else
        {
            const auto n=normal.length2()>.001f?normal:osg::Vec3f(0,0,1);
            float reach=0;
            for(unsigned i=0;i<8;++i)reach=std::max(reach,float(-(state.mNode->getAttitude()*box.corner(i))*n));
            // Keep the complete shaft outside the wall before the rigid body
            // takes over, rather than starting a long hull inside the solid.
            state.mNode->setPosition(contact+n*(reach+.5f));
            const auto center=box.center();
            osg::Vec3f half=(box._max-box._min)*.5f;
            for(int i=0;i<3;++i)half[i]=std::max(.2f,half[i]);
            ManualRef ref(*MWBase::Environment::get().getESMStore(),state.mIdArrow);
            mPhysics->addProjectileDebris(reinterpret_cast<std::uintptr_t>(residue.get()),center,half,
                state.mNode->getPosition(),state.mNode->getAttitude(),ref.getPtr().getClass().getWeight(ref.getPtr()),
                Ragdoll::projectileBounce(state.mVelocity,n));
        }
        if(mResidues.size()>=1024)
        {removeResidue(*mResidues.front());mResidues.erase(mResidues.begin());}
        mResidues.push_back(std::move(residue));
        state.mNode=nullptr;
    }

    void ProjectileManager::removeResidue(Residue& residue)
    {
        mPhysics->removeProjectileDebris(reinterpret_cast<std::uintptr_t>(&residue));
        mParent->removeChild(residue.mNode);
    }

    void ProjectileManager::updateResidues(float dt)
    {
        const auto* cell=MWMechanics::getPlayer().getCell();
        const auto now=std::chrono::steady_clock::now();
        for(auto it=mResidues.begin();it!=mResidues.end();)
        {
            auto& residue=**it;
            if(residue.mCell!=cell || std::chrono::duration<float>(now-residue.mCreated).count()>=600.f
                || (!residue.mVictim.isEmpty() && (residue.mVictim.getCellRef().getCount()<=0
                    || !residue.mVictim.getRefData().isEnabled())))
            {removeResidue(residue);it=mResidues.erase(it);continue;}
            if(!residue.mVictim.isEmpty())
            {
                osg::Matrixf bone;
                auto* animation=mRendering->getAnimation(residue.mVictim);
                osg::Vec3f corpsePosition;
                if(animation && !residue.mBone.starts_with("ragdoll ") && animation->getRagdollPosition(corpsePosition))
                {
                    // The corpse can split into separately simulated halves.
                    // Re-anchor to an actual body once, then follow that body.
                    residue.mBone=animation->findProjectileBone(residue.mWoundPosition);
                    if(animation->getProjectileBoneMatrix(residue.mBone,bone))
                    {
                        residue.mLocal=osg::Matrixf::rotate(residue.mNode->getAttitude())
                            *osg::Matrixf::translate(residue.mNode->getPosition())*osg::Matrixf::inverse(bone);
                        residue.mWoundLocal=residue.mWoundPosition*osg::Matrixf::inverse(bone);
                    }
                }
                if(animation && animation->getProjectileBoneMatrix(residue.mBone,bone))
                {
                    const auto world=residue.mLocal*bone;
                    residue.mNode->setPosition(world.getTrans());residue.mNode->setAttitude(world.getRotate());
                    residue.mWoundPosition=residue.mWoundLocal*bone;
                }
            }
            else
            {
                osg::Vec3f position;osg::Quat rotation;
                if(mPhysics->getProjectileDebrisPose(reinterpret_cast<std::uintptr_t>(&residue),position,rotation))
                {residue.mNode->setPosition(position);residue.mNode->setAttitude(rotation);}
            }
            if(residue.mBleedLeft>0 || residue.mBlood)
            {
                if(Settings::ragdoll().mBloodAmount.get()<=0)
                {
                    residue.mBlood.reset();residue.mBleedLeft=std::max(0.f,residue.mBleedLeft-dt);
                    residue.mBleedAge+=dt;++it;continue;
                }
                const osg::Vec3f direction=residue.mNode->getAttitude()*osg::Vec3f(0,1,0);
                // The entry point follows the same bone as the shaft. Its
                // world-space model tip is recovered from the attachment.
                const auto wound=residue.mWoundPosition;
                residue.mSinceSurface+=dt;
                if(!residue.mSurface)residue.mSurface=std::make_shared<MWRender::BloodTrail::Surface>();
                if(residue.mSinceSurface>=.5f)
                {
                    auto surface=mRendering->getBloodSurface(residue.mVictim.getCell(),wound,768.f);
                    if(surface){*residue.mSurface=std::move(surface);residue.mSinceSurface=0;}
                }
                if(!residue.mBlood)
                {
                    const auto surface=residue.mSurface;
                    const auto id=residue.mVictim.getCell()->getCell()->getId().serializeText();
                    residue.mBlood=std::make_unique<MWRender::BloodTrail>(*mParent,
                        [surface](const auto& a,const auto& b,auto& p,auto& n){return surface->mRaycast&&surface->mRaycast(a,b,p,n);},
                        [this,id](const auto& stamp){mRendering->addBloodDecal(id,stamp);},
                        [surface](const auto& p,const auto& n,float r){return surface->mProjector?surface->mProjector(p,n,r):std::vector<osg::Vec3f>{};},
                        [surface]{return bool(*surface);},mRendering->getBloodProjectionBudget());
                }
                const std::vector<MWRender::BloodTrail::Source> sources=residue.mBleedLeft>0
                    ?std::vector<MWRender::BloodTrail::Source>{{wound,-direction,.5f,false,false,.5f}}
                    :std::vector<MWRender::BloodTrail::Source>{};
                residue.mBlood->update(dt,sources,Settings::ragdoll().mBloodAmount.get(),residue.mBleedAge);
                residue.mBleedAge+=dt;residue.mBleedLeft=std::max(0.f,residue.mBleedLeft-dt);
                if(residue.mBleedLeft<=0 && !residue.mBlood->particleCount() && !residue.mBlood->queuedImpacts())residue.mBlood.reset();
            }
            ++it;
        }
    }

    void ProjectileManager::cleanupProjectile(ProjectileManager::ProjectileState& state)
    {
        if(state.mNode)mParent->removeChild(state.mNode);
        if(state.mProjectileId>=0)mPhysics->removeProjectile(state.mProjectileId);
        state.mToDelete = true;
    }

    void ProjectileManager::cleanupMagicBolt(ProjectileManager::MagicBoltState& state)
    {
        mParent->removeChild(state.mNode);
        mPhysics->removeProjectile(state.mProjectileId);
        state.mToDelete = true;
        for (size_t soundIter = 0; soundIter != state.mSounds.size(); soundIter++)
        {
            MWBase::Environment::get().getSoundManager()->stopSound(state.mSounds.at(soundIter));
        }
    }

    void ProjectileManager::clear()
    {
        for(auto& residue:mResidues)removeResidue(*residue);
        mResidues.clear();
        for (auto& mProjectile : mProjectiles)
            cleanupProjectile(mProjectile);
        mProjectiles.clear();

        for (auto& mMagicBolt : mMagicBolts)
            cleanupMagicBolt(mMagicBolt);
        mMagicBolts.clear();
    }

    void ProjectileManager::write(ESM::ESMWriter& writer, Loading::Listener& progress) const
    {
        for (const ProjectileState& projectile : mProjectiles)
        {
            writer.startRecord(ESM::REC_PROJ);

            ESM::ProjectileState state;
            state.mId = projectile.mIdArrow;
            state.mPosition = ESM::Vector3(osg::Vec3f(projectile.mNode->getPosition()));
            state.mOrientation = ESM::Quaternion(osg::Quat(projectile.mNode->getAttitude()));
            state.mCaster = projectile.mCaster;

            state.mBowId = projectile.mBowId;
            state.mVelocity = projectile.mVelocity;
            state.mAttackStrength = projectile.mAttackStrength;

            state.save(writer);

            writer.endRecord(ESM::REC_PROJ);
        }

        for (const MagicBoltState& bolt : mMagicBolts)
        {
            writer.startRecord(ESM::REC_MPRJ);

            ESM::MagicBoltState state;
            state.mId = bolt.mIdMagic.at(0);
            state.mPosition = ESM::Vector3(osg::Vec3f(bolt.mNode->getPosition()));
            state.mOrientation = ESM::Quaternion(osg::Quat(bolt.mNode->getAttitude()));
            state.mCaster = bolt.mCaster;
            state.mItem = bolt.mItem;
            state.mSpellId = bolt.mSpellId;
            state.mSpeed = bolt.mSpeed;

            state.save(writer);

            writer.endRecord(ESM::REC_MPRJ);
        }
    }

    bool ProjectileManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type == ESM::REC_PROJ)
        {
            ESM::ProjectileState esm;
            esm.load(reader);

            ProjectileState state;
            state.mCaster = esm.mCaster;
            state.mBowId = esm.mBowId;
            state.mVelocity = esm.mVelocity;
            state.mIdArrow = esm.mId;
            state.mAttackStrength = esm.mAttackStrength;
            state.mToDelete = false;

            VFS::Path::Normalized model;
            try
            {
                MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), esm.mId);
                MWWorld::Ptr ptr = ref.getPtr();
                model = ptr.getClass().getCorrectedModel(ptr);

                state.mProjectileId
                    = mPhysics->addProjectile(state.getCaster(), osg::Vec3f(esm.mPosition), model, false);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to add projectile for " << esm.mId
                                    << " while reading projectile record: " << e.what();
                return true;
            }

            createModel(state, model, osg::Vec3f(esm.mPosition), osg::Quat(esm.mOrientation), false, false,
                osg::Vec4(0, 0, 0, 0));

            mProjectiles.push_back(std::move(state));
            return true;
        }
        if (type == ESM::REC_MPRJ)
        {
            ESM::MagicBoltState esm;
            esm.load(reader);

            MagicBoltState state;
            state.mIdMagic.push_back(esm.mId);
            state.mSpellId = esm.mSpellId;
            state.mCaster = esm.mCaster;
            state.mToDelete = false;
            state.mItem = esm.mItem;
            std::string texture;

            try
            {
                state.mEffects = getMagicBoltData(
                    state.mIdMagic, state.mSoundIds, state.mSpeed, texture, state.mSourceName, state.mSpellId);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to recreate magic projectile for " << esm.mId << " and spell "
                                    << state.mSpellId << " while reading projectile record: " << e.what();
                return true;
            }

            state.mSpeed = esm.mSpeed; // speed is derived from non-projectile effects as well as
                                       // projectile effects, so we can't calculate it from the save
                                       // file's effect list, which is already trimmed of non-projectile
                                       // effects. We need to use the stored value.

            VFS::Path::Normalized model;
            try
            {
                MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), state.mIdMagic.at(0));
                MWWorld::Ptr ptr = ref.getPtr();
                model = ptr.getClass().getCorrectedModel(ptr);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to get model for " << state.mIdMagic.at(0)
                                    << " while reading projectile record: " << e.what();
                return true;
            }

            osg::Vec4 lightDiffuseColor = getMagicBoltLightDiffuseColor(state.mEffects);
            createModel(state, model, osg::Vec3f(esm.mPosition), osg::Quat(esm.mOrientation), true, true,
                lightDiffuseColor, texture);
            state.mProjectileId = mPhysics->addProjectile(state.getCaster(), osg::Vec3f(esm.mPosition), model, true);

            MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
            for (const auto& soundid : state.mSoundIds)
            {
                MWBase::Sound* sound = sndMgr->playSound3D(
                    esm.mPosition, soundid, 1.0f, 1.0f, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
                if (sound)
                    state.mSounds.push_back(sound);
            }

            mMagicBolts.push_back(std::move(state));
            return true;
        }

        return false;
    }

    size_t ProjectileManager::countSavedGameRecords() const
    {
        return mMagicBolts.size() + mProjectiles.size();
    }

    void ProjectileManager::saveLoaded(const ESM::ESMReader& reader)
    {
        // Can't do this in readRecord because the vectors might get reallocated as they grow
        if (reader.mActorIdConverter)
        {
            for (ProjectileState& projectile : mProjectiles)
                reader.mActorIdConverter->convert(projectile.mCaster, projectile.mCaster.mIndex);
            for (MagicBoltState& bolt : mMagicBolts)
                reader.mActorIdConverter->convert(bolt.mCaster, bolt.mCaster.mIndex);
        }
    }

    MWWorld::Ptr ProjectileManager::State::getCaster()
    {
        if (!mCasterHandle.isEmpty())
            return mCasterHandle;

        return MWBase::Environment::get().getWorldModel()->getPtr(mCaster);
    }

}
