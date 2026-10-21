#include "ragdollappearance.hpp"

#include "vismask.hpp"
#include <components/sceneutil/depth.hpp>

#include <osg/Billboard>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/LineWidth>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

namespace MWRender
{
    osg::ref_ptr<osg::ShapeDrawable> createWoundDrawable(osg::Shape* shape)
    {
        osg::ref_ptr<osg::ShapeDrawable> drawable = new osg::ShapeDrawable(shape);
        drawable->setColor(osg::Vec4(.35f, .015f, .02f, 1));
        auto* state = drawable->getOrCreateStateSet();
        constexpr auto on = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        constexpr auto off = osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        state->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::LEQUAL, 0, 1, true), on);
        state->setMode(GL_BLEND, off);
        state->setMode(GL_ALPHA_TEST, off);
        state->setMode(GL_LIGHTING, off);
        state->setRenderingHint(osg::StateSet::OPAQUE_BIN);
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nuniform mat4 projectionMatrix; varying vec4 tint; varying vec3 normal; void main(){"
            "mat4 p=projectionMatrix; if(p[0][0]==0.0) p=gl_ProjectionMatrix;"
            "gl_Position=p*gl_ModelViewMatrix*gl_Vertex; tint=gl_Color; normal=gl_NormalMatrix*gl_Normal;}"));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nvarying vec4 tint; varying vec3 normal; void main(){"
            "float light=0.45+0.55*abs(dot(normalize(normal),normalize(vec3(0.3,0.4,0.8))));"
            "gl_FragColor=vec4(tint.rgb*light,1.0);}"));
        state->setAttributeAndModes(program, on);
        return drawable;
    }

    namespace
    {
        constexpr int sTextureSize = 512;
        constexpr int sPatternSize = 256;
        constexpr int sBlendSteps = 24;
        constexpr unsigned sTextureOverride
            = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;

        osg::Matrixf rootWorldMatrix(osg::Node& root)
        {
            const auto paths = root.getParentalNodePaths();
            return paths.empty() ? osg::Matrixf::identity() : osg::Matrixf(osg::computeLocalToWorld(paths.front()));
        }

        float noise(int x, int y)
        {
            unsigned hash = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(y) * 668265263u;
            hash = (hash ^ (hash >> 13)) * 1274126177u;
            return static_cast<float>(hash & 65535u) / 65535.f;
        }

        float smoothNoise(float x, float y)
        {
            const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
            float u = x - ix, v = y - iy;
            u = u * u * (3.f - 2.f * u);
            v = v * v * (3.f - 2.f * v);
            const float a = noise(ix, iy) * (1 - u) + noise(ix + 1, iy) * u;
            const float b = noise(ix, iy + 1) * (1 - u) + noise(ix + 1, iy + 1) * u;
            return a * (1 - v) + b * v;
        }

        float clouds(float x, float y)
        {
            return smoothNoise(x, y) * .53f + smoothNoise(x * 2, y * 2) * .27f + smoothNoise(x * 4, y * 4) * .13f
                + smoothNoise(x * 8, y * 8) * .07f;
        }

        struct SurfacePattern
        {
            std::vector<osg::Vec4f> mPixels;
            explicit SurfacePattern(Ragdoll::DeathElement element)
            {
                std::vector<float> patches(sPatternSize * sPatternSize);
                for (int y = 0; y < sPatternSize; ++y)
                    for (int x = 0; x < sPatternSize; ++x)
                        patches[y * sPatternSize + x] = clouds(x / 35.f + 7.1f, y / 35.f - 3.3f);
                float threshold = 0;
                if (element == Ragdoll::DeathElement::Fire)
                {
                    auto sorted = patches;
                    const auto uncovered = sorted.begin() + sorted.size() / 10;
                    std::nth_element(sorted.begin(), uncovered, sorted.end());
                    threshold = *uncovered; // 10% bare islands, 90% sooted area.
                }
                mPixels.resize(patches.size());
                for (int y = 0; y < sPatternSize; ++y)
                    for (int x = 0; x < sPatternSize; ++x)
                    {
                        const auto i = y * sPatternSize + x;
                        const float p = patches[i], grain = noise(x, y);
                        auto& color = mPixels[i];
                        if (element == Ragdoll::DeathElement::Fire)
                        {
                            const float ash = std::pow(std::max(0.f, (p - .54f) * 3), 2.f);
                            color.set(.018f + ash * .15f + grain * .018f, .014f + ash * .13f + grain * .015f,
                                .011f + ash * .11f + grain * .012f,
                                p <= threshold ? 0.f : .72f + .14f * std::clamp((p - threshold) / .12f, 0.f, 1.f));
                        }
                        else if (element == Ragdoll::DeathElement::Poison)
                        {
                            // Continuous wet pools with small rounded droplets;
                            // highlights follow smooth pool edges, never sine stripes.
                            const float fine = smoothNoise(x / 4.f + 31, y / 4.f + 13);
                            const float ridge = std::exp(-std::pow((p - .58f) * 65.f, 2.f));
                            const float glint = std::pow(std::max(0.f, (fine - .65f) / .35f), 5.f);
                            color.set(.035f + p * .13f + ridge * .18f + glint * .32f,
                                .16f + p * .42f + ridge * .26f + glint * .35f,
                                .012f + p * .035f + ridge * .07f + glint * .15f, 1.f);
                        }
                        else
                        {
                            // Irregular granular ice: nearest jittered crystal
                            // sites create edges without repeating diagonal bands.
                            const float gx = x / 12.f, gy = y / 12.f;
                            const int cx = static_cast<int>(gx), cy = static_cast<int>(gy);
                            float first = 100, second = 100;
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx)
                                {
                                    const float px = cx + dx + noise(cx + dx, cy + dy);
                                    const float py = cy + dy + noise(cx + dx + 91, cy + dy - 53);
                                    const float d = (gx - px) * (gx - px) + (gy - py) * (gy - py);
                                    if (d < first)
                                    {
                                        second = first;
                                        first = d;
                                    }
                                    else
                                        second = std::min(second, d);
                                }
                            const float edge
                                = std::clamp(1.f - (std::sqrt(second) - std::sqrt(first)) * 12.f, 0.f, 1.f);
                            const float frost = .70f + p * .19f + grain * .035f + edge * .07f;
                            color.set(
                                frost, std::min(1.f, frost + .025f), std::min(1.f, frost + .055f), .72f + p * .08f);
                        }
                    }
            }
        };

        const SurfacePattern& surfacePattern(Ragdoll::DeathElement element)
        {
            if (element == Ragdoll::DeathElement::Fire)
            {
                static const SurfacePattern value(element);
                return value;
            }
            if (element == Ragdoll::DeathElement::Poison)
            {
                static const SurfacePattern value(element);
                return value;
            }
            static const SurfacePattern value(Ragdoll::DeathElement::Frost);
            return value;
        }

        struct Paint
        {
            osg::ref_ptr<osg::Texture2D> mSource;
            std::vector<unsigned char> mOriginal;
            std::vector<unsigned char> mTarget;
            osg::ref_ptr<osg::Texture2D> mCurrent;
            int mWidth = 0;
            int mHeight = 0;
            int mStep = -1;

            Paint(osg::Texture2D& source, Ragdoll::DeathElement element)
                : mSource(&source)
            {
                const osg::Image& image = *source.getImage();
                mWidth = std::min(sTextureSize, image.s());
                mHeight = std::min(sTextureSize, image.t());
                mOriginal.reserve(mWidth * mHeight * 4);
                mTarget.reserve(mWidth * mHeight * 4);
                const auto& pattern = surfacePattern(element);
                for (int y = 0; y < mHeight; ++y)
                    for (int x = 0; x < mWidth; ++x)
                    {
                        // OSG 3.6 getColor also decodes stock DDS/DXT textures.
                        const auto pixel = image.getColor(x * image.s() / mWidth, y * image.t() / mHeight);
                        const auto layer
                            = pattern.mPixels[(y * sPatternSize / mHeight) * sPatternSize + x * sPatternSize / mWidth];
                        for (int channel = 0; channel < 4; ++channel)
                        {
                            const float original = pixel[channel];
                            const float target
                                = channel == 3 ? original : original * (1 - layer.a()) + layer[channel] * layer.a();
                            mOriginal.push_back(
                                static_cast<unsigned char>(std::clamp(original, 0.f, 1.f) * 255.f + .5f));
                            mTarget.push_back(static_cast<unsigned char>(std::clamp(target, 0.f, 1.f) * 255.f + .5f));
                        }
                    }
            }

            void update(int step)
            {
                if (mStep == step)
                    return;
                mStep = step;
                if (step == 0)
                {
                    mCurrent = mSource;
                    return;
                }
                osg::ref_ptr<osg::Image> image = new osg::Image;
                image->setOrigin(mSource->getImage()->getOrigin());
                image->allocateImage(mWidth, mHeight, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                auto* bytes = image->data();
                for (std::size_t i = 0; i < mOriginal.size(); ++i)
                    bytes[i] = static_cast<unsigned char>(
                        (mOriginal[i] * (sBlendSteps - step) + mTarget[i] * step) / sBlendSteps);
                // Publish a new immutable image/texture; drawing the preceding
                // frame can never race with writes to its pixels or StateSet.
                mCurrent = new osg::Texture2D(image);
                mCurrent->setWrap(osg::Texture::WRAP_S, mSource->getWrap(osg::Texture::WRAP_S));
                mCurrent->setWrap(osg::Texture::WRAP_T, mSource->getWrap(osg::Texture::WRAP_T));
                mCurrent->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
                mCurrent->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
                mCurrent->setMaxAnisotropy(mSource->getMaxAnisotropy());
                if (step == sBlendSteps)
                {
                    std::vector<unsigned char>().swap(mOriginal);
                    std::vector<unsigned char>().swap(mTarget);
                }
            }
        };

        struct Binding
        {
            osg::ref_ptr<osg::Group> mParent;
            osg::ref_ptr<osg::Node> mOriginal;
            osg::ref_ptr<osg::Node> mPartRoot;
            osg::ref_ptr<osg::Node> mOverlay;
            osg::ref_ptr<osg::StateSet> mOriginalStates;
            bool mDrawableClone = false;
            osg::ref_ptr<osg::Material> mWetMaterial;
            std::shared_ptr<Paint> mPaint;
        };

        class FindTextures : public osg::NodeVisitor
        {
        public:
            std::vector<std::tuple<osg::ref_ptr<osg::Node>, osg::ref_ptr<osg::Texture2D>, osg::ref_ptr<osg::Group>>>
                mFound;
            FindTextures()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
                setNodeMaskOverride(~0u);
            }
            void apply(osg::Node& node) override
            {
                bool preserve = false;
                node.getUserValue("RealCombatPreserveMaterial", preserve);
                if (preserve || node.getNodeMask() == Mask_Effect)
                    return;
                if (auto* states = node.getStateSet())
                    if (auto* texture
                        = dynamic_cast<osg::Texture2D*>(states->getTextureAttribute(0, osg::StateAttribute::TEXTURE)))
                        if (texture->getImage() && texture->getImage()->data() && texture->getImage()->s() > 0
                            && texture->getImage()->t() > 0)
                        {
                            osg::Group* parent = node.getNumParents() == 1 ? node.getParent(0) : nullptr;
                            // Static hair/armour Drawables may be shared between
                            // instances. Clone only this traversal's Geode edge.
                            if (node.asDrawable() && getNodePath().size() > 1)
                                parent = getNodePath()[getNodePath().size() - 2]->asGroup();
                            if (parent)
                                mFound.emplace_back(&node, texture, parent);
                        }
                traverse(node);
            }
        };

        osg::ref_ptr<osg::Texture2D> particleTexture(bool smoke, Ragdoll::DeathElement element)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(32, 32, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                {
                    const float px = (x - 15.5f) / 16.f;
                    const float py = (y - 15.5f) / 16.f;
                    const float radial = std::max(0.f, 1.f - px * px - py * py);
                    const float alpha = radial * radial * (smoke ? 0.5f : 1.f);
                    const auto color = element == Ragdoll::DeathElement::Poison ? osg::Vec4f(0.1f, 0.75f, 0.16f, alpha)
                        : element == Ragdoll::DeathElement::Shock               ? osg::Vec4f(0.35f, 0.65f, 1.f, alpha)
                        : smoke                                                 ? osg::Vec4f(0.17f, 0.15f, 0.14f, alpha)
                                : osg::Vec4f(1.f, 0.3f + 0.65f * radial, 0.03f + 0.22f * radial, alpha);
                    image->setColor(color, x, y);
                }
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            return texture;
        }

        osg::ref_ptr<osg::StateSet> particleState(bool smoke, Ragdoll::DeathElement element)
        {
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setTextureAttributeAndModes(0, particleTexture(smoke, element), sTextureOverride);
            state->setAttributeAndModes(
                new osg::BlendFunc(GL_SRC_ALPHA, smoke ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE), sTextureOverride);
            state->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::LEQUAL, 0, 1, false), sTextureOverride);
            state->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
            state->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
            osg::ref_ptr<osg::Program> program = new osg::Program;
            program->addShader(new osg::Shader(osg::Shader::VERTEX,
                "#version 120\nuniform mat4 projectionMatrix; varying vec2 uv; varying vec4 tint; void main(){"
                // OpenMW retains an unreversed camera projection for culling;
                // its actual render projection is a per-view uniform. The
                // built-in matrix is only a fallback for standalone OSG views.
                "mat4 p=projectionMatrix; if(p[0][0]==0.0) p=gl_ProjectionMatrix;"
                "gl_Position=p*gl_ModelViewMatrix*gl_Vertex; uv=gl_MultiTexCoord0.xy; tint=gl_Color;}"));
            program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
                "#version 120\nuniform sampler2D diffuseMap; varying vec2 uv; varying vec4 tint;"
                "void main(){gl_FragColor=texture2D(diffuseMap,uv)*tint;}"));
            state->setAttributeAndModes(program, sTextureOverride);
            state->addUniform(new osg::Uniform("diffuseMap", 0));
            return state;
        }

        osg::ref_ptr<osg::Geometry> quad(float width, float height, float alpha)
        {
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            const std::array<osg::Vec3f, 4> vertices{ osg::Vec3f(-width, -height, 0), osg::Vec3f(width, -height, 0),
                osg::Vec3f(width, height, 0), osg::Vec3f(-width, height, 0) };
            const std::array<osg::Vec2f, 4> coordinates{ osg::Vec2f(0, 0), osg::Vec2f(1, 0), osg::Vec2f(1, 1),
                osg::Vec2f(0, 1) };
            geometry->setVertexArray(new osg::Vec3Array(vertices.size(), vertices.data()));
            geometry->setTexCoordArray(0, new osg::Vec2Array(coordinates.size(), coordinates.data()));
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->push_back(osg::Vec4f(1, 1, 1, alpha));
            geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
            geometry->addPrimitiveSet(new osg::DrawArrays(GL_QUADS, 0, 4));
            geometry->setUseDisplayList(false);
            return geometry;
        }
    }

    struct RagdollAppearance::Impl
    {
        osg::ref_ptr<osg::Group> mRoot;
        Ragdoll::DeathElement mElement;
        Settings mSettings;
        SkinParts mParts;
        std::vector<Binding> mBindings;
        std::map<osg::Texture2D*, std::shared_ptr<Paint>> mPaints;
        osg::ref_ptr<osg::MatrixTransform> mFire;
        osg::ref_ptr<osg::StateSet> mFlameState;
        osg::ref_ptr<osg::StateSet> mSmokeState;
        float mBlend = 0;
        int mStep = -1;
        float mLastFireFrame = -1;

        ~Impl()
        {
            clearSkin();
            if (mFire)
                mRoot->removeChild(mFire);
        }

        void clearSkin(const SkinParts* replacementParts = nullptr)
        {
            // Undo inside-out so nested material roots recover their original path.
            for (auto it = mBindings.rbegin(); it != mBindings.rend(); ++it)
            {
                if (it->mDrawableClone)
                {
                    auto* geode = static_cast<osg::Geode*>(it->mParent.get());
                    const bool removedRoot = replacementParts && it->mPartRoot == it->mOriginal
                        && std::ranges::find(*replacementParts, it->mPartRoot) == replacementParts->end();
                    if (removedRoot)
                        geode->removeDrawable(it->mOverlay->asDrawable());
                    else
                        geode->replaceDrawable(it->mOverlay->asDrawable(), it->mOriginal->asDrawable());
                }
                else if (it->mOverlay->asGroup()->containsNode(it->mOriginal))
                {
                    it->mParent->replaceChild(it->mOverlay, it->mOriginal);
                    it->mOverlay->asGroup()->removeChild(it->mOriginal);
                }
                else
                    it->mParent->removeChild(it->mOverlay); // The equipment system removed this part.
            }
            mBindings.clear();
            mPaints.clear();
            mParts.clear();
        }

        void setParts(const SkinParts& parts)
        {
            clearSkin(&parts);
            mParts = parts;
            for (const auto& part : parts)
            {
                if (!part)
                    continue;
                FindTextures visitor;
                part->accept(visitor);
                for (const auto& [node, texture, parent] : visitor.mFound)
                {
                    auto& paint = mPaints[texture];
                    if (!paint)
                        paint = std::make_shared<Paint>(*texture, mElement);
                    Binding binding;
                    binding.mParent = parent;
                    binding.mOriginal = node;
                    binding.mPartRoot = part;
                    binding.mPaint = paint;
                    if (mElement == Ragdoll::DeathElement::Poison)
                    {
                        const auto* states = node->getStateSet();
                        const auto* original = states
                            ? dynamic_cast<const osg::Material*>(states->getAttribute(osg::StateAttribute::MATERIAL))
                            : nullptr;
                        binding.mWetMaterial = original ? new osg::Material(*original) : new osg::Material;
                        if (!original)
                            binding.mWetMaterial->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
                        binding.mWetMaterial->setSpecular(
                            osg::Material::FRONT_AND_BACK, osg::Vec4f(.55f, .75f, .4f, 1));
                        binding.mWetMaterial->setShininess(osg::Material::FRONT_AND_BACK, 80.f);
                    }
                    if (auto* geode = dynamic_cast<osg::Geode*>(binding.mParent.get()))
                    {
                        auto* original = node->asDrawable();
                        if (!original)
                            continue;
                        // Geode children must stay Drawables. Clone just this
                        // instance, retaining immutable geometry arrays through
                        // mOriginal while replacing its private StateSet.
                        osg::ref_ptr<osg::Drawable> clone
                            = static_cast<osg::Drawable*>(original->clone(osg::CopyOp::DEEP_COPY_CALLBACKS));
                        binding.mOverlay = clone;
                        binding.mOriginalStates = original->getStateSet();
                        binding.mDrawableClone = true;
                        geode->replaceDrawable(original, clone);
                    }
                    else
                    {
                        osg::ref_ptr<osg::Group> wrapper = new osg::Group;
                        wrapper->setName("Ragdoll skin surface");
                        binding.mOverlay = wrapper;
                        binding.mParent->replaceChild(node, wrapper);
                        wrapper->addChild(node);
                    }
                    mBindings.push_back(std::move(binding));
                }
            }
            mStep = -1;
        }

        void updateSkin(float elapsed, const SkinParts& parts)
        {
            if (mElement != Ragdoll::DeathElement::Poison && mElement != Ragdoll::DeathElement::Fire
                && mElement != Ragdoll::DeathElement::Frost)
                return;
            if (mParts != parts)
                setParts(parts);
            const float duration = mElement == Ragdoll::DeathElement::Poison ? mSettings.mPoisonTransitionTime
                : mElement == Ragdoll::DeathElement::Fire                    ? mSettings.mFireTransitionTime
                                                                             : mSettings.mFrostTransitionTime;
            mBlend = duration > 0 ? std::clamp(elapsed / duration, 0.f, 1.f) : 1.f;
            const int step = std::min(sBlendSteps, static_cast<int>(mBlend * sBlendSteps));
            if (step == mStep)
                return;
            mStep = step;
            for (const auto& [source, paint] : mPaints)
                paint->update(step);
            for (const auto& binding : mBindings)
            {
                osg::ref_ptr<osg::StateSet> state = binding.mOriginalStates
                    ? new osg::StateSet(*binding.mOriginalStates, osg::CopyOp::SHALLOW_COPY)
                    : new osg::StateSet;
                state->setTextureAttributeAndModes(0, binding.mPaint->mCurrent, sTextureOverride);
                if (binding.mWetMaterial)
                    state->setAttributeAndModes(binding.mWetMaterial, sTextureOverride);
                binding.mOverlay->setStateSet(state);
            }
        }

        void updateFire(float elapsed, std::span<const osg::Vec3f> positions)
        {
            if ((mElement != Ragdoll::DeathElement::Fire && mElement != Ragdoll::DeathElement::Poison
                    && mElement != Ragdoll::DeathElement::Shock)
                || positions.empty())
                return;
            const float duration = mElement == Ragdoll::DeathElement::Fire ? mSettings.mFireEffectDuration
                : mElement == Ragdoll::DeathElement::Poison                ? mSettings.mPoisonEffectDuration
                                                                           : mSettings.mShockEffectDuration;
            if (elapsed >= duration)
            {
                if (mFire)
                    mRoot->removeChild(mFire);
                mFire = nullptr;
                mFlameState = nullptr;
                mSmokeState = nullptr;
                return;
            }
            if (!mFire)
            {
                mFire = new osg::MatrixTransform;
                mFire->setName(mElement == Ragdoll::DeathElement::Fire ? "Ragdoll body fire and smoke"
                        : mElement == Ragdoll::DeathElement::Poison    ? "Ragdoll poison smoke"
                                                                       : "Ragdoll electric discharges");
                mFire->setNodeMask(Mask_Effect);
                mFlameState = particleState(false, mElement);
                mSmokeState = particleState(true, mElement);
                if (mElement == Ragdoll::DeathElement::Shock)
                    mFlameState->setAttributeAndModes(new osg::LineWidth(3.f), sTextureOverride);
                mRoot->addChild(mFire);
            }
            mFire->setMatrix(osg::Matrixf::inverse(rootWorldMatrix(*mRoot)));
            if (elapsed - mLastFireFrame < 1.f / 30.f)
                return;
            mLastFireFrame = elapsed;
            osg::ref_ptr<osg::Group> particles = new osg::Group;
            const float ending = std::clamp((duration - elapsed) / 0.5f, 0.f, 1.f);
            if (mElement == Ragdoll::DeathElement::Shock)
            {
                osg::ref_ptr<osg::Geode> arcs = new osg::Geode;
                arcs->setStateSet(mFlameState);
                for (unsigned arc = 0; arc < 16; ++arc)
                {
                    const auto start = positions[arc % positions.size()] + osg::Vec3f(-8, -6, 4);
                    const auto end = positions[(arc * 3 + 1) % positions.size()] + osg::Vec3f(8, -6, 10);
                    osg::ref_ptr<osg::Vec3Array> points = new osg::Vec3Array;
                    osg::ref_ptr<osg::Vec2Array> uv = new osg::Vec2Array;
                    for (int joint = 0; joint <= 8; ++joint)
                    {
                        const float t = joint / 8.f;
                        const float phase = std::floor(elapsed * 24.f) * 1.7f + arc * 3.1f + joint * 2.3f;
                        points->push_back(start * (1 - t) + end * t
                            + osg::Vec3f(std::sin(phase) * 4, std::cos(phase * 1.3f) * 4, std::sin(phase * 0.8f) * 4));
                        uv->push_back(osg::Vec2f(0.5f, 0.5f));
                    }
                    osg::ref_ptr<osg::Geometry> line = new osg::Geometry;
                    line->setVertexArray(points);
                    line->setTexCoordArray(0, uv);
                    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
                    colors->push_back(osg::Vec4f(1, 1, 1, ending));
                    line->setColorArray(colors, osg::Array::BIND_OVERALL);
                    line->addPrimitiveSet(new osg::DrawArrays(GL_LINE_STRIP, 0, points->size()));
                    line->setUseDisplayList(false);
                    arcs->addDrawable(line);
                }
                particles->addChild(arcs);
                mFire->removeChildren(0, mFire->getNumChildren());
                mFire->addChild(particles);
                return;
            }
            for (const bool smoke : { false, true })
            {
                if (mElement == Ragdoll::DeathElement::Poison && !smoke)
                    continue;
                osg::ref_ptr<osg::Billboard> billboard = new osg::Billboard;
                billboard->setMode(osg::Billboard::POINT_ROT_EYE);
                billboard->setNormal(osg::Vec3f(0, 0, 1));
                billboard->setStateSet(smoke ? mSmokeState : mFlameState);
                for (unsigned i = 0; i < (smoke ? 12u : 18u); ++i)
                {
                    const float cycle = elapsed * (smoke ? 0.6f : 1.6f) + i * 0.6180339f;
                    const float age = cycle - std::floor(cycle);
                    osg::Vec3f point = positions[(i * 7u) % positions.size()];
                    point += osg::Vec3f(std::sin(i * 2.1f + age * 4.f) * (smoke ? 9.f : 4.f),
                        std::cos(i * 1.7f + age * 3.f) * 5.f, age * (smoke ? 55.f : 22.f));
                    const float size = smoke ? 8.f + age * 15.f : 4.f + age * 4.f;
                    billboard->addDrawable(
                        quad(size, smoke ? size : size * 1.8f, std::sin(age * 3.14159265f) * ending), point);
                }
                particles->addChild(billboard);
            }
            mFire->removeChildren(0, mFire->getNumChildren());
            mFire->addChild(particles);
        }
    };

    RagdollAppearance::RagdollAppearance(osg::Group& root, Ragdoll::DeathElement element, const Settings& settings)
        : mImpl(std::make_unique<Impl>())
    {
        mImpl->mRoot = &root;
        mImpl->mElement = element;
        mImpl->mSettings = settings;
    }

    RagdollAppearance::~RagdollAppearance() = default;
    void RagdollAppearance::update(float elapsed, const SkinParts& skinParts, std::span<const osg::Vec3f> bodyPositions)
    {
        mImpl->updateSkin(elapsed, skinParts);
        mImpl->updateFire(elapsed, bodyPositions);
    }
    float RagdollAppearance::getBlend() const
    {
        return mImpl->mBlend;
    }
    void RagdollAppearance::setEffectDurations(float fire, float poison, float shock)
    {
        mImpl->mSettings.mFireEffectDuration = fire;
        mImpl->mSettings.mPoisonEffectDuration = poison;
        mImpl->mSettings.mShockEffectDuration = shock;
    }
    bool RagdollAppearance::hasFire() const
    {
        return mImpl->mElement == Ragdoll::DeathElement::Fire && bool(mImpl->mFire);
    }
    bool RagdollAppearance::hasParticles() const
    {
        return bool(mImpl->mFire);
    }
}
