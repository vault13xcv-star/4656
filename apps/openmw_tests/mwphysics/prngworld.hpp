// Test-only World: provide deterministic AI timing without bootstrapping a game.
// Other World services fail loudly if a fixture accidentally depends on them.
#ifndef OPENMW_TESTS_PRNGWORLD_HPP
#define OPENMW_TESTS_PRNGWORLD_HPP
#include "apps/openmw/mwbase/world.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/timestamp.hpp"
#include <components/detournavigator/agentbounds.hpp>
#include <osg/Matrixf>
#include <osg/Vec4f>
#include <stdexcept>
namespace TestingOpenMW
{
    class PrngWorld final : public MWBase::World
    {
        Misc::Rng::Generator mPrng{1};
    public:
        void setRandomSeed(uint32_t seed) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void startNewGame(bool bypass) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void clear() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        size_t countSavedGameRecords() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        size_t countSavedGameCells() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void write(ESM::ESMWriter& writer, Loading::Listener& listener) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void readRecord(ESM::ESMReader& reader, uint32_t type) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void useDeathCamera() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void setWaterHeight(const float height) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool toggleWater() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool toggleWorld() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool toggleBorders() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Player& getPlayer() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr getPlayerPtr() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::ConstPtr getPlayerConstPtr() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::ESMStore& getStore() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const MWWorld::ESMStore& getStore() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const std::vector<int>& getESMVersions() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::LocalScripts& getLocalScripts() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isCellExterior() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isCellQuasiExterior() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void getDoorMarkers(MWWorld::CellStore& cell, std::vector<DoorMarker>& out) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void setGlobalInt(MWWorld::GlobalVariableName name, int value) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void setGlobalFloat(MWWorld::GlobalVariableName name, float value) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        int getGlobalInt(MWWorld::GlobalVariableName name) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getGlobalFloat(MWWorld::GlobalVariableName name) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        char getGlobalVariableType(MWWorld::GlobalVariableName name) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        std::string_view getCellName(const MWWorld::CellStore* cell = nullptr) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        std::string_view getCellName(const MWWorld::Cell& cell) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void removeRefScript(const MWWorld::CellRef* ref) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr getPtr(const ESM::RefId& name, bool activeOnly) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr searchPtr(const ESM::RefId& name, bool activeOnly, bool searchInContainers = true) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr findContainer(const MWWorld::ConstPtr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void enable(const MWWorld::Ptr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void disable(const MWWorld::Ptr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void advanceTime(double hours, bool incremental = false) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::TimeStamp getTimeStamp() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool toggleSky() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void changeWeather(const ESM::RefId& region, const unsigned int id) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void changeWeather(const ESM::RefId& region, const ESM::RefId& id) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const std::vector<MWWorld::Weather>& getAllWeather() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        int getCurrentWeatherScriptId() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const MWWorld::Weather& getCurrentWeather() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const MWWorld::Weather* getWeather(size_t index) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const MWWorld::Weather* getWeather(const ESM::RefId& id) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        int getNextWeatherScriptId() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const MWWorld::Weather* getNextWeather() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getWeatherTransition() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        unsigned int getNightDayMode() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        int getMasserPhase() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        int getSecundaPhase() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void setMoonColour(bool red) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void modRegion(const ESM::RefId& regionid, const std::vector<uint8_t>& chances) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void changeToInteriorCell( std::string_view cellName, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent = true) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void changeToCell( const ESM::RefId& cellId, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent = true) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr getFocusObject() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getDistanceToFocusObject() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getMaxActivationDistance() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void adjustPosition(const MWWorld::Ptr& ptr, bool force) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void fixPosition() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void deleteObject(const MWWorld::Ptr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void undeleteObject(const MWWorld::Ptr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr moveObject( const MWWorld::Ptr& ptr, const osg::Vec3f& position, bool movePhysics = true, bool moveToActive = false) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr moveObject(const MWWorld::Ptr& ptr, MWWorld::CellStore* newCell, const osg::Vec3f& position, bool movePhysics = true, bool keepActive = false) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr moveObjectBy(const MWWorld::Ptr& ptr, const osg::Vec3f& vec, bool moveToActive) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void scaleObject(const MWWorld::Ptr& ptr, float scale, bool force = false) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void rotateObject( const MWWorld::Ptr& ptr, const osg::Vec3f& rot, MWBase::RotationFlags flags = MWBase::RotationFlag_inverseOrder) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr placeObject( const MWWorld::ConstPtr& ptr, MWWorld::CellStore* cell, const ESM::Position& pos) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr safePlaceObject(const MWWorld::ConstPtr& ptr, const MWWorld::ConstPtr& referenceObject, MWWorld::CellStore* referenceCell, int direction, float distance) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void queueMovement(const MWWorld::Ptr& ptr, const osg::Vec3f& velocity) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void updateAnimatedCollisionShape(const MWWorld::Ptr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const MWPhysics::RayCastingInterface* getRayCasting() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        std::vector<osg::Vec3f> getRagdollCollisionTriangles(const osg::Vec3f& center, float radius) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void applyItemImpulse(const MWWorld::Ptr& ptr, const osg::Vec3f& impulse, const osg::Vec3f& point, float strengthScale = 1.f) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void applyItemExplosion(const osg::Vec3f& origin, float radius, float strength, float strengthScale = 1.f) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool castRenderingRay(MWPhysics::RayCastingResult& res, const osg::Vec3f& from, const osg::Vec3f& to, bool ignorePlayer, bool ignoreActors, std::span<const MWWorld::Ptr> ignoreList = {}) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void setActorCollisionMode(const MWWorld::Ptr& ptr, bool internal, bool external) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isActorCollisionEnabled(const MWWorld::Ptr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool toggleCollisionMode() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool toggleRenderMode(MWRender::RenderMode mode) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr placeObject( const MWWorld::Ptr& object, float cursorX, float cursorY, int amount, bool copy = true) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::Ptr dropObjectOnGround( const MWWorld::Ptr& actor, const MWWorld::Ptr& object, int amount, bool copy = true) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool canPlaceObject(float cursorX, float cursorY) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void processChangedSettings(const std::set<std::pair<std::string, std::string>>& settings) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isFlying(const MWWorld::Ptr& ptr) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isSlowFalling(const MWWorld::Ptr& ptr) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isSwimming(const MWWorld::ConstPtr& object) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isWading(const MWWorld::ConstPtr& object) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isSubmerged(const MWWorld::ConstPtr& object) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isUnderwater(const MWWorld::CellStore* cell, const osg::Vec3f& pos) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isUnderwater(const MWWorld::ConstPtr& object, const float heightRatio) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isWaterWalkingCastableOnTarget(const MWWorld::ConstPtr& target) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isOnGround(const MWWorld::Ptr& ptr) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        osg::Matrixf getActorHeadTransform(const MWWorld::ConstPtr& actor) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWRender::Camera* getCamera() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void togglePOV(bool force = false) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isFirstPerson() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isPreviewModeEnabled() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool toggleVanityMode(bool enable) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool vanityRotateCamera(const float* rot) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void applyDeferredPreviewRotationToPlayer(float dt) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void disableDeferredPreviewRotation() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void saveLoaded(const ESM::ESMReader& reader) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void setupPlayer() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void renderPlayer() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void activateDoor(const MWWorld::Ptr& door) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void activateDoor(const MWWorld::Ptr& door, MWWorld::DoorState state) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void getActorsStandingOn(const MWWorld::ConstPtr& object, std::vector<MWWorld::Ptr>& actors) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool getPlayerStandingOn(const MWWorld::ConstPtr& object) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool getActorStandingOn(const MWWorld::ConstPtr& object) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool getPlayerCollidingWith(const MWWorld::ConstPtr& object) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool getActorCollidingWith(const MWWorld::ConstPtr& object) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void hurtStandingActors(const MWWorld::ConstPtr& object, float dmgPerSecond) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void hurtCollidingActors(const MWWorld::ConstPtr& object, float dmgPerSecond) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getWindSpeed() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void getContainersOwnedBy(const MWWorld::ConstPtr& npc, std::vector<MWWorld::Ptr>& out) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void getItemsOwnedBy(const MWWorld::ConstPtr& npc, std::vector<MWWorld::Ptr>& out) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool getLOS(const MWWorld::ConstPtr& actor, const MWWorld::ConstPtr& targetActor) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getDistToNearestRayHit( const osg::Vec3f& from, const osg::Vec3f& dir, float maxDist, bool includeWater = false) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void enableActorCollision(const MWWorld::Ptr& actor, bool enable) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        int canRest() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWRender::Animation* getAnimation(const MWWorld::Ptr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const MWRender::Animation* getAnimation(const MWWorld::ConstPtr& ptr) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void reattachPlayerCamera() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void screenshot(osg::Image* image, int w, int h) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        ESM::RefId findExteriorPosition(std::string_view name, ESM::Position& pos) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        ESM::RefId findInteriorPosition(std::string_view name, ESM::Position& pos) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void enableTeleporting(bool enable) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isTeleportingEnabled() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void enableLevitation(bool enable) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isLevitationEnabled() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool getGodModeState() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool toggleGodMode() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool toggleScripts() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool getScriptsEnabled() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::SpellCastState startSpellCast(const MWWorld::Ptr& actor) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void castSpell(const MWWorld::Ptr& actor, bool scriptedSpell = false) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void launchMagicBolt(const ESM::RefId& spellId, const MWWorld::Ptr& caster, const osg::Vec3f& fallbackDirection, ESM::RefNum item) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void launchProjectile(MWWorld::Ptr& actor, MWWorld::Ptr& projectile, const osg::Vec3f& worldPos, const osg::Quat& orient, MWWorld::Ptr& bow, float speed, float attackStrength) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void updateProjectilesCasters() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void applyLoopingParticles(const MWWorld::Ptr& ptr) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const std::vector<std::string>& getContentFiles() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void breakInvisibility(const MWWorld::Ptr& actor) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool useTorches() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        const osg::Vec4f& getSunLightPosition() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getSunVisibility() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getSunPercentage() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getPhysicsFrameRateDt() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool findInteriorPositionInWorldSpace(const MWWorld::CellStore* cell, osg::Vec3f& result) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void teleportToClosestMarker(const MWWorld::Ptr& ptr, const ESM::RefId& id) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void listDetectedReferences(const MWWorld::Ptr& ptr, std::vector<MWWorld::Ptr>& out, DetectionType type) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void updateDialogueGlobals() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void confiscateStolenItems(const MWWorld::Ptr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void goToJail() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void spawnRandomCreature(const ESM::RefId& creatureList) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void spawnEffect(VFS::Path::NormalizedView model, const std::string& textureOverride, const osg::Vec3f& worldPos, float scale = 1.f, bool isMagicVFX = true, bool useAmbientLight = true, std::string_view effectId = {}, bool loop = false) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void removeEffect(std::string_view effectId) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isInStorm() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        osg::Vec3f getStormDirection() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void resetActors() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isWalkingOnWater(const MWWorld::ConstPtr& actor) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        osg::Vec3f aimToTarget( const MWWorld::ConstPtr& actor, const MWWorld::ConstPtr& target, bool isRangedCombat) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void addContainerScripts(const MWWorld::Ptr& reference, MWWorld::CellStore* cell) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void removeContainerScripts(const MWWorld::Ptr& reference) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isPlayerInJail() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void rest(double hours) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void setPlayerTraveling(bool traveling) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isPlayerTraveling() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void rotateWorldObject(const MWWorld::Ptr& ptr, const osg::Quat& rotate) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        float getTerrainHeightAt(const osg::Vec3f& worldPos, ESM::RefId worldspace) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        osg::Vec3f getHalfExtents(const MWWorld::ConstPtr& actor, bool rendering = false) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        std::filesystem::path exportSceneGraph(const MWWorld::Ptr& ptr) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void preloadEffects(const ESM::EffectList* effectList) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        DetourNavigator::Navigator* getNavigator() const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void updateActorPath(const MWWorld::ConstPtr& actor, const std::deque<osg::Vec3f>& path, const DetourNavigator::AgentBounds& agentBounds, const osg::Vec3f& start, const osg::Vec3f& end) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void removeActorPath(const MWWorld::ConstPtr& actor) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void setNavMeshNumberToRender(const std::size_t value) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        DetourNavigator::AgentBounds getPathfindingAgentBounds(const MWWorld::ConstPtr& actor) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool hasCollisionWithDoor( const MWWorld::ConstPtr& door, const osg::Vec3f& position, const osg::Vec3f& destination) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        bool isAreaOccupiedByOtherActor(const MWWorld::ConstPtr& actor, const osg::Vec3f& position) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void reportStats(unsigned int frameNumber, osg::Stats& stats) const override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        std::vector<MWWorld::Ptr> getAll(const ESM::RefId& id) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        Misc::Rng::Generator& getPrng() override { return mPrng; }
        MWRender::RenderingManager* getRenderingManager() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWRender::PostProcessor* getPostProcessor() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        MWWorld::DateTimeManager* getTimeManager() override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
        void setActorActive(const MWWorld::Ptr& ptr, bool value) override { throw std::logic_error("Unexpected World service in PRNG-only fixture"); }
    };
}
#endif
