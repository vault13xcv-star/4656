#include "brutalblood.hpp"
#include "vismask.hpp"
#include <components/ragdoll/blood.hpp>
#include <components/sceneutil/depth.hpp>
#include <osg/BlendFunc>
#include <osg/Geode>
#include <osg/Depth>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>
#include <algorithm>
#include <array>
#include <memory>
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <cmath>

namespace MWRender
{
    namespace
    {
        osg::Vec3f unit(osg::Vec3f v, osg::Vec3f fallback = { 0, 0, 1 })
        {
            return std::isfinite(v.length2()) && v.normalize() > .0001f ? v : fallback;
        }
        osg::Matrixf worldMatrix(osg::Node& root)
        {
            const auto paths = root.getParentalNodePaths();
            return paths.empty() ? osg::Matrixf::identity() : osg::Matrixf(osg::computeLocalToWorld(paths.front()));
        }
        osg::ref_ptr<osg::Geometry> geometry(osg::Vec3Array* vertices, osg::Vec3Array* normals,
            osg::Vec4Array* colors)
        {
            osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
            result->setUseDisplayList(false);
            result->setUseVertexBufferObjects(true);
            result->setDataVariance(osg::Object::DYNAMIC);
            result->setVertexArray(vertices);
            result->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
            result->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            result->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, vertices->size()));
            return result;
        }
        bool validStamp(const BloodStamp& stamp)
        {
            if (stamp.mTriangles.empty() || stamp.mTriangles.size() % 3
                || stamp.mTriangles.size() > ESM::BrutalBloodState::MaxVerticesPerStain
                || !std::isfinite(stamp.mRadius) || stamp.mRadius <= 0 || stamp.mRadius > 256
                || !std::isfinite(stamp.mCenter.length2()) || !std::isfinite(stamp.mNormal.length2())
                || std::abs(stamp.mNormal.length2() - 1.f) > .05f) return false;
            return std::all_of(stamp.mTriangles.begin(), stamp.mTriangles.end(),
                [](const auto& v) { return std::isfinite(v.length2()); });
        }
        std::uint32_t nextSeed(std::uint32_t& state)
        {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            if (!state) state = 0x9f673bbdu;
            return state;
        }
        osg::Vec4f stainTint(std::uint32_t seed, bool textured)
        {
            // Use the saved visual seed, independently of the variant/mirror
            // bits: a stain keeps its shade when chunks rebuild or games load.
            seed ^= 0xd1b54a35u;
            const float brightness = .92f + .16f * ((nextSeed(seed) & 65535u) / 65535.f);
            const float blue = .95f + .10f * ((nextSeed(seed) & 65535u) / 65535.f);
            // Texture RGB remains detailed, with a restrained burgundy tint.
            // The untextured fallback uses the same palette at material scale.
            const osg::Vec3f base = textured ? osg::Vec3f(.58f, .42f, .64f) : osg::Vec3f(.22f, .008f, .028f);
            return { base.x() * brightness, base.y() * brightness, base.z() * brightness * blue, .94f };
        }
    }

    static osg::ref_ptr<osg::StateSet> makeWetGoreState(bool decal)
    {
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        constexpr unsigned on = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        constexpr unsigned off = osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        state->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::LEQUAL, 0, 1, !decal), on);
        state->setMode(GL_LIGHTING, off);
        state->setMode(GL_CULL_FACE, off);
        state->setMode(GL_ALPHA_TEST, off);
        state->setMode(GL_BLEND, decal ? on : off);
        if (decal)
            state->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), on);
        state->setRenderingHint(decal ? osg::StateSet::TRANSPARENT_BIN : osg::StateSet::OPAQUE_BIN);
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nuniform mat4 projectionMatrix; varying vec4 tint; varying vec3 n; varying vec3 p; varying vec2 uv;varying vec2 bloodInfo;"
            "void main(){mat4 proj=projectionMatrix; if(proj[0][0]==0.0)proj=gl_ProjectionMatrix;"
            "vec4 eye=gl_ModelViewMatrix*gl_Vertex;gl_Position=proj*eye;n=gl_NormalMatrix*gl_Normal;"
            "p=eye.xyz;tint=gl_Color;uv=gl_MultiTexCoord0.xy;bloodInfo=gl_MultiTexCoord1.xy;}"));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT, decal ?
            "#version 120\nuniform float brutalBloodTime;uniform float brutalBloodLifetime;"
            "uniform sampler2D brutalBloodAtlas;"
            "varying vec4 tint;varying vec3 n;varying vec3 p;varying vec2 uv;varying vec2 bloodInfo;"
            "void main(){vec3 N=normalize(n);vec3 L=normalize(vec3(.3,.4,.8));"
            "float age=max(0.0,brutalBloodTime-bloodInfo.x);float dry=min(1.0,age/90.0);"
            "vec4 c=tint;"
            "if(bloodInfo.y>.5){vec4 texel=texture2D(brutalBloodAtlas,uv);"
            "vec3 pigment=vec3(texel.r,texel.g,max(texel.b,texel.r*.11));"
            "c=vec4(pigment*tint.rgb,texel.a*tint.a);}"
            "c.rgb*=1.0-.5*dry;c.a*=clamp((brutalBloodLifetime-age)/10.0,0.0,1.0);"
            "if(c.a<.003)discard;"
            "float spec=pow(max(0.0,dot(reflect(-L,N),normalize(-p))),24.0);"
            "gl_FragColor=vec4(c.rgb*(.62+.38*abs(dot(N,L)))+vec3(.035)*spec*c.a,c.a);}"
            :
            "#version 120\nvarying vec4 tint;varying vec3 n;varying vec3 p;varying vec2 uv;"
            "void main(){vec3 N=normalize(n);vec3 L=normalize(vec3(.3,.4,.8));"
            "float fold=.92+.08*sin(uv.x*110.0);float vein=pow(max(0.0,sin(uv.x*47.0+sin(uv.y*17.0)*2.0)),24.0);"
            "vec3 flesh=mix(tint.rgb,vec3(.23,.025,.045),vein*.5);"
            "float spec=pow(max(0.0,dot(reflect(-L,N),normalize(-p))),28.0);"
            "gl_FragColor=vec4(flesh*fold*(.5+.5*abs(dot(N,L)))+vec3(.32)*spec,1.0);}"));
        state->setAttributeAndModes(program, on);
        if (decal)
        {
            state->addUniform(new osg::Uniform("brutalBloodTime", 0.f));
            state->addUniform(new osg::Uniform("brutalBloodLifetime", 600.f));
            state->addUniform(new osg::Uniform("brutalBloodAtlas", 0));
        }
        return state;
    }

    osg::ref_ptr<osg::StateSet> createWetGoreState(bool decal)
    {
        // Share programs, not per-cell clocks or editable material state. Repeated
        // kills must not compile another identical shader for every gore node.
        static const std::array states{ makeWetGoreState(false), makeWetGoreState(true) };
        return new osg::StateSet(*states[decal], osg::CopyOp::SHALLOW_COPY);
    }

    BloodDecals::BloodDecals(osg::Group& parent)
        : mParent(&parent), mTimeUniform(new osg::Uniform("brutalBloodTime", 0.f))
        , mLifetimeUniform(new osg::Uniform("brutalBloodLifetime", 600.f)) {}
    BloodDecals::~BloodDecals() { clear(); }
    std::size_t BloodDecals::size() const { return mStains.size(); }

    void BloodDecals::setTextures(const std::vector<osg::ref_ptr<osg::Image>>& floor,
        const std::vector<osg::ref_ptr<osg::Image>>& wall)
    {
        struct Tile { osg::ref_ptr<osg::Image> mImage; bool mFloor; };
        std::vector<Tile> tiles;
        for (const auto* group : { &floor, &wall })
        {
            unsigned accepted = 0;
            for (const auto& image : *group)
            {
                if (!image || !image->data() || image->s() <= 0 || image->t() <= 0
                    || image->r() != 1 || image->isCompressed()) continue;
                tiles.push_back({ image, group == &floor });
                if (++accepted == 8) break;
            }
        }
        mFloorTiles.clear(); mWallTiles.clear(); mAtlas = nullptr;
        if (!tiles.empty())
        {
            // A fixed tile size bounds GPU/CPU memory independently of uploaded
            // image dimensions. Letterboxing retains each PNG's aspect ratio.
            constexpr int tileSize = 512, gutter = 4, inner = tileSize - gutter * 2;
            const int columns = tiles.size() <= 2 ? 2 : 4;
            int rows = 1;
            while (rows * columns < int(tiles.size())) rows *= 2;
            const int width = columns * tileSize, height = rows * tileSize;
            osg::ref_ptr<osg::Image> atlas = new osg::Image;
            atlas->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            atlas->setOrigin(osg::Image::BOTTOM_LEFT);
            std::fill_n(atlas->data(), atlas->getTotalSizeInBytes(), 0);
            for (std::size_t index = 0; index < tiles.size(); ++index)
            {
                const auto& tile = tiles[index];
                const auto& source = *tile.mImage;
                const int x = int(index) % columns * tileSize, y = int(index) / columns * tileSize;
                const float scale = std::min(float(inner) / source.s(), float(inner) / source.t());
                const int w = std::max(1, int(source.s() * scale)), h = std::max(1, int(source.t() * scale));
                const int left = x + (tileSize - w) / 2, bottom = y + (tileSize - h) / 2;
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                    {
                        const float sx = w > 1 ? float(i) / (w - 1) * (source.s() - 1) : 0;
                        float sy = h > 1 ? float(j) / (h - 1) * (source.t() - 1) : 0;
                        // Resource::ImageManager uses TOP_LEFT convention; GL
                        // atlas UVs increase upward. Normalize exactly once.
                        if (source.getOrigin() == osg::Image::TOP_LEFT) sy = source.t() - 1 - sy;
                        const int x0 = int(sx), y0 = int(sy);
                        const int x1 = std::min(x0 + 1, source.s() - 1), y1 = std::min(y0 + 1, source.t() - 1);
                        const float fx = sx - x0, fy = sy - y0;
                        const auto a = source.getColor(x0, y0) * (1 - fx) + source.getColor(x1, y0) * fx;
                        const auto b = source.getColor(x0, y1) * (1 - fx) + source.getColor(x1, y1) * fx;
                        atlas->setColor(a * (1 - fy) + b * fy, left + i, bottom + j);
                    }
                const osg::Vec4f rect(float(x + .5f) / width, float(y + .5f) / height,
                    float(x + tileSize - .5f) / width, float(y + tileSize - .5f) / height);
                (tile.mFloor ? mFloorTiles : mWallTiles).push_back(rect);
            }
            mAtlas = new osg::Texture2D(atlas);
            mAtlas->setResizeNonPowerOfTwoHint(false);
            mAtlas->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            mAtlas->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            // Whole-atlas mipmaps would eventually mix wall/floor artwork
            // across gutters. Linear minification keeps variants isolated.
            mAtlas->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            mAtlas->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            mAtlas->setUseHardwareMipMapGeneration(false);
        }
        constexpr unsigned on = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        for (auto& [cell, batch] : mBatches)
            for (auto& [id, chunk] : batch.mChunks)
            {
                chunk.mDirty = true;
                if (!chunk.mNode) continue;
                auto* state = chunk.mNode->getOrCreateStateSet();
                if (mAtlas) state->setTextureAttributeAndModes(0, mAtlas, on);
                else state->removeTextureAttribute(0, osg::StateAttribute::TEXTURE);
            }
        mSinceBatch = 1;
    }

    void BloodDecals::setCellActive(const std::string& cell, bool active)
    {
        auto& batch = mBatches[cell];
        batch.mActive = active;
        for (auto& [id, chunk] : batch.mChunks)
            if (chunk.mNode) chunk.mNode->setNodeMask(active ? unsigned(Mask_Effect) : 0u);
    }

    void BloodDecals::dirty(const Stain& stain)
    {
        mBatches[stain.mCell].mChunks[stain.mSequence / ChunkStains].mDirty = true;
    }

    void BloodDecals::add(const std::string& cell, const BloodStamp& stamp)
    {
        if (!validStamp(stamp)) return;
        if (stamp.mPool)
            for (auto it = mStains.rbegin(); it != mStains.rend()
                && std::distance(mStains.rbegin(), it) < 256; ++it)
                if (it->mCell == cell && it->mStamp.mPool && it->mStamp.mNormal * stamp.mNormal > .95f
                    && (it->mStamp.mCenter - stamp.mCenter).length2() < std::pow(std::min(stamp.mRadius, it->mStamp.mRadius) * .7f, 2))
                {
                    if (stamp.mRadius > it->mStamp.mRadius
                        && mVertexCount - it->mStamp.mTriangles.size() + stamp.mTriangles.size() <= MaxVertices)
                    {
                        const auto seed = it->mStamp.mVisualSeed;
                        mVertexCount -= it->mStamp.mTriangles.size();
                        it->mStamp = stamp;
                        it->mStamp.mVisualSeed = seed;
                        mVertexCount += it->mStamp.mTriangles.size();
                    }
                    it->mAge = 0;
                    dirty(*it);
                    return;
                }
        // Bounded memory even with hundreds of simultaneous gore emitters.
        while (!mStains.empty() && (mStains.size() >= MaxStains
            || mVertexCount + stamp.mTriangles.size() > MaxVertices))
        {
            dirty(mStains.front());
            mVertexCount -= mStains.front().mStamp.mTriangles.size();
            mStains.pop_front(); // O(1), including when a fountain fills the cap.
        }
        mStains.push_back({ { cell, stamp, 0 }, mNextSequence++ });
        if (!mStains.back().mStamp.mVisualSeed) mStains.back().mStamp.mVisualSeed = nextSeed(mRandom);
        mVertexCount += stamp.mTriangles.size();
        dirty(mStains.back());
    }

    void BloodDecals::update(float duration, float lifetime)
    {
        if (!std::isfinite(lifetime)) lifetime = 600;
        mLifetime = std::clamp(lifetime, 300.f, 1800.f);
        mLifetimeUniform->set(mLifetime);
        if (!std::isfinite(duration) || duration <= 0) return;
        mTime += duration;
        mTimeUniform->set(static_cast<float>(mTime));
        mSinceBatch += duration;
        for (auto& stain : mStains)
        {
            stain.mAge += duration;
        }
        mStains.erase(std::remove_if(mStains.begin(), mStains.end(),
            [&](const auto& stain) {
                if (stain.mAge < mLifetime) return false;
                mVertexCount -= stain.mStamp.mTriangles.size();
                dirty(stain);
                return true;
            }), mStains.end());
        if (mSinceBatch >= .1f)
        {
            rebuild();
            mSinceBatch = 0;
        }
    }

    void BloodDecals::rebuild()
    {
        // Prepare dirty chunks, distribute stains in ONE pass, then finalize.
        // Scanning the full cache once for each dirty chunk becomes quadratic
        // when a crowded fight adds blood to many cells/chunks simultaneously.
        std::vector<std::pair<Chunk*, bool>> dirtyChunks;
        for (auto& [cell, batch] : mBatches)
            for (auto& [id, chunk] : batch.mChunks)
            {
                if (!chunk.mDirty) continue;
                if (!chunk.mGeometry)
                {
                    chunk.mGeometry = geometry(new osg::Vec3Array, new osg::Vec3Array, new osg::Vec4Array);
                    chunk.mGeometry->setTexCoordArray(0, new osg::Vec2Array);
                    chunk.mGeometry->setTexCoordArray(1, new osg::Vec2Array);
                }
                static_cast<osg::Vec3Array*>(chunk.mGeometry->getVertexArray())->clear();
                static_cast<osg::Vec3Array*>(chunk.mGeometry->getNormalArray())->clear();
                static_cast<osg::Vec4Array*>(chunk.mGeometry->getColorArray())->clear();
                static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(0))->clear();
                static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(1))->clear();
                dirtyChunks.emplace_back(&chunk, batch.mActive);
            }
        if (dirtyChunks.empty()) return;
        for (const auto& stain : mStains)
        {
            auto& chunk = mBatches.find(stain.mCell)->second.mChunks.find(stain.mSequence / ChunkStains)->second;
            if (!chunk.mDirty) continue;
            auto* vertices = static_cast<osg::Vec3Array*>(chunk.mGeometry->getVertexArray());
            auto* normals = static_cast<osg::Vec3Array*>(chunk.mGeometry->getNormalArray());
            auto* colors = static_cast<osg::Vec4Array*>(chunk.mGeometry->getColorArray());
            auto* coords = static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(0));
            auto* births = static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(1));
            const auto& stamp = stain.mStamp;
            const bool floor = stamp.mNormal.z() >= .7f;
            const auto& tiles = floor ? mFloorTiles : mWallTiles;
            const bool textured = mAtlas && !tiles.empty();
            const auto seed = stamp.mVisualSeed;
            const auto tint = stainTint(seed, textured);
            const osg::Vec4f tile = textured ? tiles[(seed >> 2) % tiles.size()] : osg::Vec4f(0, 0, 1, 1);
            // On walls V follows projected world-up, independent of impact
            // direction. Floor artwork may rotate only by exactly 180 degrees.
            const osg::Vec3f up = floor ? osg::Vec3f(0, 1, 0) : osg::Vec3f(0, 0, 1);
            const auto vertical = unit(up - stamp.mNormal * (up * stamp.mNormal), { 0, 1, 0 });
            const auto horizontal = unit(vertical ^ stamp.mNormal, { 1, 0, 0 });
            for (const auto& vertex : stamp.mTriangles)
            {
                const auto relative = vertex - stamp.mCenter;
                float u = std::clamp(.5f + (relative * horizontal) / (stamp.mRadius * 2), 0.f, 1.f);
                float v = std::clamp(.5f + (relative * vertical) / (stamp.mRadius * 2), 0.f, 1.f);
                if (floor && (seed & 2)) { u = 1 - u; v = 1 - v; }
                if (seed & 1) u = 1 - u;
                vertices->push_back(vertex);
                normals->push_back(stamp.mNormal);
                colors->push_back(tint);
                coords->push_back({ tile.x() + u * (tile.z() - tile.x()), tile.y() + v * (tile.w() - tile.y()) });
                births->push_back({ static_cast<float>(mTime - stain.mAge), textured ? 1.f : 0.f });
            }
        }
        constexpr unsigned on = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        for (const auto& [chunk, active] : dirtyChunks)
        {
            auto* vertices = chunk->mGeometry->getVertexArray();
            if (!vertices->getNumElements()) continue;
            if (!chunk->mNode)
            {
                chunk->mNode = new osg::Geode;
                chunk->mNode->setName("Brutal Morrowind blood stains");
                auto state = createWetGoreState(true);
                state->addUniform(mTimeUniform); state->addUniform(mLifetimeUniform);
                if (mAtlas) state->setTextureAttributeAndModes(0, mAtlas, on);
                chunk->mNode->setStateSet(state);
                chunk->mNode->addDrawable(chunk->mGeometry);
                mParent->addChild(chunk->mNode);
            }
            vertices->dirty();
            chunk->mGeometry->getNormalArray()->dirty();
            chunk->mGeometry->getColorArray()->dirty();
            chunk->mGeometry->getTexCoordArray(0)->dirty();
            chunk->mGeometry->getTexCoordArray(1)->dirty();
            static_cast<osg::DrawArrays*>(chunk->mGeometry->getPrimitiveSet(0))->setCount(vertices->getNumElements());
            chunk->mGeometry->dirtyBound();
            chunk->mNode->setNodeMask(active ? unsigned(Mask_Effect) : 0u);
            chunk->mDirty = false;
        }
        for (auto& [cell, batch] : mBatches)
            for (auto it = batch.mChunks.begin(); it != batch.mChunks.end();)
            {
                if (it->second.mDirty && !it->second.mGeometry->getVertexArray()->getNumElements())
                {
                    if (it->second.mNode) mParent->removeChild(it->second.mNode);
                    it = batch.mChunks.erase(it);
                }
                else ++it;
            }
    }
    void BloodDecals::clear()
    {
        for (auto& [cell, batch] : mBatches)
            for (auto& [id, chunk] : batch.mChunks)
                if (chunk.mNode) mParent->removeChild(chunk.mNode);
        mBatches.clear();
        mStains.clear();
        mVertexCount = 0;
        mNextSequence = 0; mTime = 0; mSinceBatch = 1;
        mTimeUniform->set(0.f);
    }
    std::vector<BloodStainState> BloodDecals::saveState() const { return { mStains.begin(), mStains.end() }; }
    void BloodDecals::restoreState(const std::vector<BloodStainState>& state)
    {
        // Preserve visibility flags established while loading active cells.
        for (auto& [cell, batch] : mBatches)
        {
            for (auto& [id, chunk] : batch.mChunks)
                if (chunk.mNode) mParent->removeChild(chunk.mNode);
            batch.mChunks.clear();
        }
        mStains.clear();
        mVertexCount = 0;
        mNextSequence = 0; mTime = 0; mSinceBatch = 1;
        mTimeUniform->set(0.f);
        for (const auto& stain : state)
        {
            if (!std::isfinite(stain.mAge) || stain.mAge < 0 || stain.mAge >= 1800) continue;
            if (mStains.size() >= MaxStains) break;
            if (!validStamp(stain.mStamp) || mVertexCount + stain.mStamp.mTriangles.size() > MaxVertices) continue;
            // Loading is not a new deposit: never merge/rejuvenate saved pools.
            mStains.push_back({ stain, mNextSequence++ });
            if (!mStains.back().mStamp.mVisualSeed) mStains.back().mStamp.mVisualSeed = nextSeed(mRandom);
            mVertexCount += stain.mStamp.mTriangles.size();
            dirty(mStains.back());
        }
        rebuild();
    }

    ESM::BrutalBloodState BloodDecals::getSaveRecord() const
    {
        ESM::BrutalBloodState record;
        for (const auto& stain : mStains)
        {
            ESM::BrutalBloodStain saved;
            saved.mCell = stain.mCell; saved.mAge = stain.mAge; saved.mRadius = stain.mStamp.mRadius; saved.mPool = stain.mStamp.mPool;
            saved.mVisualSeed = stain.mStamp.mVisualSeed;
            for (unsigned i = 0; i < 3; ++i) { saved.mCenter[i] = stain.mStamp.mCenter[i]; saved.mNormal[i] = stain.mStamp.mNormal[i]; }
            for (const auto& v : stain.mStamp.mTriangles) saved.mTriangles.push_back({ v.x(), v.y(), v.z() });
            record.mStains.push_back(std::move(saved));
        }
        return record;
    }
    void BloodDecals::loadSaveRecord(const ESM::BrutalBloodState& record)
    {
        std::vector<BloodStainState> state;
        for (const auto& saved : record.mStains)
        {
            BloodStainState stain;
            stain.mCell = saved.mCell; stain.mAge = saved.mAge; stain.mStamp.mRadius = saved.mRadius; stain.mStamp.mPool = saved.mPool;
            stain.mStamp.mVisualSeed = saved.mVisualSeed;
            stain.mStamp.mCenter = { saved.mCenter[0], saved.mCenter[1], saved.mCenter[2] };
            stain.mStamp.mNormal = { saved.mNormal[0], saved.mNormal[1], saved.mNormal[2] };
            for (const auto& v : saved.mTriangles) stain.mStamp.mTriangles.push_back({ v[0], v[1], v[2] });
            state.push_back(std::move(stain));
        }
        restoreState(state);
    }

    BloodTrail::BloodTrail(osg::Group& root, Raycast raycast, Deposit deposit)
        : mRoot(&root), mTransform(new osg::MatrixTransform)
        , mGeometry(geometry(new osg::Vec3Array, new osg::Vec3Array, new osg::Vec4Array))
        , mRaycast(std::move(raycast)), mDeposit(std::move(deposit))
    {
        mTransform->setName("Brutal Morrowind ballistic blood");
        mTransform->setNodeMask(Mask_Effect);
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->setStateSet(createWetGoreState());
        geode->addDrawable(mGeometry);
        mTransform->addChild(geode);
        root.addChild(mTransform);
    }
    BloodTrail::~BloodTrail() { mRoot->removeChild(mTransform); }
    float BloodTrail::random()
    {
        mRandom ^= mRandom << 13; mRandom ^= mRandom >> 17; mRandom ^= mRandom << 5;
        return (mRandom & 65535u) / 65535.f;
    }
    BloodTrail::Raycast BloodTrail::collisionRaycast(const std::vector<osg::Vec3f>& triangles)
    {
        if (triangles.empty() || triangles.size() % 3) return {};
        for (const auto& v : triangles) if (!std::isfinite(v.length2())) return {};
        struct Mesh
        {
            btTriangleMesh triangles;
            std::unique_ptr<btBvhTriangleMeshShape> shape;
        };
        auto mesh = std::make_shared<Mesh>();
        const auto bullet = [](const osg::Vec3f& v) { return btVector3(v.x(), v.y(), v.z()); };
        for (std::size_t i = 0; i < triangles.size(); i += 3)
            mesh->triangles.addTriangle(bullet(triangles[i]), bullet(triangles[i + 1]), bullet(triangles[i + 2]));
        mesh->shape = std::make_unique<btBvhTriangleMeshShape>(&mesh->triangles, true);
        return [mesh, bullet](const auto& a, const auto& b, auto& p, auto& n) {
            btCollisionObject object;
            object.setCollisionShape(mesh->shape.get()); object.setWorldTransform(btTransform::getIdentity());
            auto from = btTransform::getIdentity(), to = from;
            from.setOrigin(bullet(a)); to.setOrigin(bullet(b));
            btCollisionWorld::ClosestRayResultCallback hit(bullet(a), bullet(b));
            btCollisionWorld::rayTestSingle(from, to, &object, mesh->shape.get(), object.getWorldTransform(), hit);
            if (!hit.hasHit()) return false;
            p.set(float(hit.m_hitPointWorld.x()), float(hit.m_hitPointWorld.y()), float(hit.m_hitPointWorld.z()));
            n.set(float(hit.m_hitNormalWorld.x()), float(hit.m_hitNormalWorld.y()), float(hit.m_hitNormalWorld.z()));
            return true;
        };
    }
    void BloodTrail::stamp(const osg::Vec3f& position, const osg::Vec3f& normal, float radius, bool pool)
    {
        if (!mDeposit || !mRaycast) return;
        BloodStamp stamp;
        stamp.mCenter = position;
        stamp.mNormal = unit(normal);
        stamp.mRadius = radius * DecalScale * (.88f + .24f * random());
        radius = stamp.mRadius;
        stamp.mPool = pool;
        const osg::Vec3f up = stamp.mNormal.z() >= .7f ? osg::Vec3f(0, 1, 0) : osg::Vec3f(0, 0, 1);
        const auto v = unit(up - stamp.mNormal * (up * stamp.mNormal), { 0, 1, 0 });
        const auto u = unit(v ^ stamp.mNormal, { 1, 0, 0 });
        std::vector<osg::Vec3f> rim;
        std::vector<bool> valid;
        // An eight-point square perimeter preserves the entire artwork and
        // halves the old circular fan's raycasts/vertices. Every point still
        // projects onto the actual surface, clipping at corners and ledges.
        constexpr std::array<std::array<float, 2>, 8> perimeter{ {
            { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 },
            { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }
        } };
        for (const auto& point : perimeter)
        {
            const auto p = position + (u * point[0] + v * point[1]) * radius;
            osg::Vec3f projected, n;
            const bool hit = mRaycast(p + stamp.mNormal * 2.f, p - stamp.mNormal * 3.f, projected, n)
                && unit(n) * stamp.mNormal > .8f && std::abs((projected - position) * stamp.mNormal) < 2.f;
            valid.push_back(hit);
            rim.push_back(projected + stamp.mNormal * .06f);
        }
        for (unsigned i = 0; i < rim.size(); ++i)
        {
            const auto next = (i + 1) % rim.size();
            if (!valid[i] || !valid[next]) continue;
            stamp.mTriangles.insert(stamp.mTriangles.end(), { position + stamp.mNormal * .06f, rim[i], rim[next] });
        }
        if (!stamp.mTriangles.empty()) mDeposit(stamp);
    }
    void BloodTrail::update(float duration, const std::vector<Source>& sources, float amount, float age)
    {
        if (!std::isfinite(duration) || duration <= 0) return;
        const float dt = std::min(duration, .1f);
        amount = Ragdoll::bloodEmissionScale(amount);
        mRemainders.resize(sources.size(), 0);
        mPoolTimes.resize(sources.size(), 0);
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            const auto& source = sources[i];
            if (!std::isfinite(source.mPosition.length2()) || !std::isfinite(source.mDirection.length2())
                || !std::isfinite(source.mPressure)) continue;
            const float pulse = .55f + .45f * std::pow(std::max(0.f, std::sin(age * 11.f)), 2.f);
            mRemainders[i] += dt * amount * (source.mNeck ? 60.f : 14.f);
            int count = static_cast<int>(mRemainders[i]);
            mRemainders[i] -= count;
            for (int j = 0; j < count && mDrops.size() < 192; ++j)
            {
                const osg::Vec3f jitter(random() - .5f, random() - .5f, random() - .5f);
                const auto direction = unit(source.mDirection + jitter * (source.mNeck ? .22f : .8f));
                const float speed = source.mNeck ? (200.f + 170.f * pulse) : (55.f + random() * 100.f);
                mDrops.push_back({ source.mPosition, direction * speed * std::clamp(source.mPressure, .15f, 1.f),
                    0, .3f + random() * .55f });
                ++mEmitted;
            }
            // Slow dripping from a grounded wound grows a local pool. Each rim
            // point is projected independently, preventing stains across corners.
            mPoolTimes[i] += dt;
            if (amount > 0 && count > 0 && mPoolTimes[i] >= .25f)
            {
                mPoolTimes[i] = 0;
                osg::Vec3f point, normal;
                if (mRaycast && mRaycast(source.mPosition, source.mPosition - osg::Vec3f(0, 0, 18), point, normal)
                    && normal.z() > .65f)
                    stamp(point, normal, std::min(28.f, (3.f + std::sqrt(std::max(0.f, age)) * 5.f) * std::sqrt(amount)), true);
            }
        }
        // Sweep every ballistic segment; no blood travels through a wall between frames.
        unsigned splats = 0;
        for (auto& drop : mDrops)
        {
            const auto next = drop.mPosition + drop.mVelocity * dt + osg::Vec3f(0, 0, -313.6f * dt * dt);
            osg::Vec3f point, normal;
            if (mRaycast && mRaycast(drop.mPosition, next, point, normal))
            {
                // A low frame rate must not turn hundreds of simultaneous
                // impacts into thousands of perimeter raycasts in one update.
                if (amount > 0 && splats++ < 8)
                    stamp(point, normal, (1.f + random() * 3.2f) * std::sqrt(amount), false);
                drop.mAge = 4;
            }
            else
            {
                drop.mPosition = next;
                drop.mVelocity.z() -= 627.2f * dt;
                drop.mAge += dt;
            }
        }
        mDrops.erase(std::remove_if(mDrops.begin(), mDrops.end(), [](const auto& p) { return p.mAge > 2.f; }), mDrops.end());
        auto* vertices = static_cast<osg::Vec3Array*>(mGeometry->getVertexArray());
        auto* normals = static_cast<osg::Vec3Array*>(mGeometry->getNormalArray());
        auto* colors = static_cast<osg::Vec4Array*>(mGeometry->getColorArray());
        if (vertices->empty() && mDrops.empty()) return;
        vertices->clear(); normals->clear(); colors->clear();
        for (const auto& drop : mDrops)
        {
            const auto axis = unit(drop.mVelocity);
            const auto side = unit(axis ^ osg::Vec3f(0, 0, 1), { 1, 0, 0 }) * drop.mSize;
            const auto other = unit(axis ^ side) * drop.mSize;
            const auto tip = drop.mPosition + axis * std::min(4.f, drop.mVelocity.length() * .009f);
            const auto tail = drop.mPosition - axis * drop.mSize;
            for (const auto& edge : { std::pair(side, other), std::pair(other, -side), std::pair(-side, -other), std::pair(-other, side) })
                for (const auto& triangle : { std::array<osg::Vec3f, 3>{ tip, drop.mPosition + edge.first, drop.mPosition + edge.second },
                         std::array<osg::Vec3f, 3>{ tail, drop.mPosition + edge.second, drop.mPosition + edge.first } })
                {
                    const auto n = unit((triangle[1] - triangle[0]) ^ (triangle[2] - triangle[0]));
                    for (const auto& p : triangle) { vertices->push_back(p); normals->push_back(n); colors->push_back({ .26f, .007f, .028f, 1 }); }
                }
        }
        vertices->dirty(); normals->dirty(); colors->dirty();
        static_cast<osg::DrawArrays*>(mGeometry->getPrimitiveSet(0))->setCount(vertices->size());
        mGeometry->dirtyBound();
        mTransform->setMatrix(osg::Matrixf::inverse(worldMatrix(*mRoot)));
    }
}
