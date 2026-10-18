#include "ragdollappearance.hpp"

#include "vismask.hpp"

#include <osg/Billboard>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Group>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

namespace MWRender
{
    namespace
    {
        constexpr int sTextureSize = 256;
        constexpr int sBlendSteps = 24;
        constexpr unsigned sTextureOverride = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE
            | osg::StateAttribute::PROTECTED;

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

        osg::Vec4f surfacePixel(Ragdoll::DeathElement element, int x, int y, const osg::Vec4f& source)
        {
            const float grain = noise(x, y);
            const float waves = std::sin(x * 0.13f + std::sin(y * 0.09f) * 4.f);
            const float brightness = 0.65f + (source.r() + source.g() + source.b()) * (0.35f / 3.f);
            osg::Vec4f color;
            if (element == Ragdoll::DeathElement::Poison)
            {
                const float vein = std::pow(std::max(0.f, waves), 12.f);
                color.set(0.04f + grain * 0.08f + vein * 0.12f,
                    0.24f + grain * 0.25f + vein * 0.35f, 0.015f + grain * 0.035f, source.a());
            }
            else if (element == Ragdoll::DeathElement::Fire)
            {
                const float crack = std::abs(std::sin(x * 0.19f + std::sin(y * 0.17f) * 2.f)) < 0.08f ? 1.f : 0.f;
                color.set(0.018f + grain * 0.04f + crack * 0.055f,
                    0.014f + grain * 0.03f + crack * 0.025f, 0.012f + grain * 0.022f, source.a());
            }
            else
            {
                const float crystal = std::pow(std::abs(std::sin((x + y) * 0.17f)), 18.f);
                color.set(0.40f + grain * 0.22f + crystal * 0.3f,
                    0.65f + grain * 0.15f + crystal * 0.16f, 0.85f + grain * 0.1f, source.a());
            }
            color.r() *= brightness;
            color.g() *= brightness;
            color.b() *= brightness;
            return color;
        }

        struct Paint
        {
            osg::ref_ptr<osg::Texture2D> mSource;
            std::vector<osg::Vec4f> mOriginal;
            std::vector<osg::Vec4f> mTarget;
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
                mOriginal.reserve(mWidth * mHeight);
                mTarget.reserve(mWidth * mHeight);
                for (int y = 0; y < mHeight; ++y)
                    for (int x = 0; x < mWidth; ++x)
                    {
                        // OSG 3.6 getColor also decodes stock DDS/DXT textures.
                        const auto pixel = image.getColor(x * image.s() / mWidth, y * image.t() / mHeight);
                        mOriginal.push_back(pixel);
                        mTarget.push_back(surfacePixel(element, x, y, pixel));
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
                const float blend = static_cast<float>(step) / sBlendSteps;
                osg::ref_ptr<osg::Image> image = new osg::Image;
                image->setOrigin(mSource->getImage()->getOrigin());
                image->allocateImage(mWidth, mHeight, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                for (int y = 0; y < mHeight; ++y)
                    for (int x = 0; x < mWidth; ++x)
                    {
                        const auto index = y * mWidth + x;
                        image->setColor(mOriginal[index] * (1.f - blend) + mTarget[index] * blend, x, y);
                    }
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
                    std::vector<osg::Vec4f>().swap(mOriginal);
                    std::vector<osg::Vec4f>().swap(mTarget);
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
            std::shared_ptr<Paint> mPaint;
        };

        class FindTextures : public osg::NodeVisitor
        {
        public:
            std::vector<std::pair<osg::ref_ptr<osg::Node>, osg::ref_ptr<osg::Texture2D>>> mFound;
            FindTextures() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
            void apply(osg::Node& node) override
            {
                if (auto* states = node.getStateSet())
                    if (auto* texture = dynamic_cast<osg::Texture2D*>(states->getTextureAttribute(0, osg::StateAttribute::TEXTURE)))
                        if (texture->getImage() && texture->getImage()->data()
                            && texture->getImage()->s() > 0 && texture->getImage()->t() > 0)
                            mFound.emplace_back(&node, texture);
                traverse(node);
            }
        };

        osg::ref_ptr<osg::Texture2D> particleTexture(bool smoke)
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
                    image->setColor(smoke ? osg::Vec4f(0.17f, 0.15f, 0.14f, alpha)
                        : osg::Vec4f(1.f, 0.3f + 0.65f * radial, 0.03f + 0.22f * radial, alpha), x, y);
                }
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            return texture;
        }

        osg::ref_ptr<osg::StateSet> particleState(bool smoke)
        {
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setTextureAttributeAndModes(0, particleTexture(smoke), sTextureOverride);
            state->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA,
                smoke ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE), osg::StateAttribute::ON);
            state->setAttributeAndModes(new osg::Depth(osg::Depth::LEQUAL, 0, 1, false), osg::StateAttribute::ON);
            state->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            state->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
            osg::ref_ptr<osg::Program> program = new osg::Program;
            program->addShader(new osg::Shader(osg::Shader::VERTEX,
                "#version 120\nvarying vec2 uv; varying vec4 tint; void main(){"
                "gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex; uv=gl_MultiTexCoord0.xy; tint=gl_Color;}"));
            program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
                "#version 120\nuniform sampler2D diffuseMap; varying vec2 uv; varying vec4 tint;"
                "void main(){gl_FragColor=texture2D(diffuseMap,uv)*tint;}"));
            state->setAttributeAndModes(program, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            state->addUniform(new osg::Uniform("diffuseMap", 0));
            return state;
        }

        osg::ref_ptr<osg::Geometry> quad(float width, float height, float alpha)
        {
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            const std::array<osg::Vec3f, 4> vertices{ osg::Vec3f(-width, -height, 0),
                osg::Vec3f(width, -height, 0), osg::Vec3f(width, height, 0), osg::Vec3f(-width, height, 0) };
            const std::array<osg::Vec2f, 4> coordinates{ osg::Vec2f(0, 0), osg::Vec2f(1, 0),
                osg::Vec2f(1, 1), osg::Vec2f(0, 1) };
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
                for (const auto& [node, texture] : visitor.mFound)
                {
                    if (node->getNumParents() != 1)
                        continue; // Do not mutate a multiply attached custom mesh.
                    auto& paint = mPaints[texture];
                    if (!paint)
                        paint = std::make_shared<Paint>(*texture, mElement);
                    Binding binding;
                    binding.mParent = node->getParent(0);
                    binding.mOriginal = node;
                    binding.mPartRoot = part;
                    binding.mPaint = paint;
                    if (auto* geode = dynamic_cast<osg::Geode*>(binding.mParent.get()))
                    {
                        auto* original = node->asDrawable();
                        if (!original)
                            continue;
                        // Geode children must stay Drawables. Clone just this
                        // instance, retaining immutable geometry arrays through
                        // mOriginal while replacing its private StateSet.
                        osg::ref_ptr<osg::Drawable> clone = static_cast<osg::Drawable*>(
                            original->clone(osg::CopyOp::DEEP_COPY_CALLBACKS));
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
                : mElement == Ragdoll::DeathElement::Fire ? mSettings.mFireTransitionTime
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
                    ? new osg::StateSet(*binding.mOriginalStates, osg::CopyOp::SHALLOW_COPY) : new osg::StateSet;
                state->setTextureAttributeAndModes(0, binding.mPaint->mCurrent, sTextureOverride);
                binding.mOverlay->setStateSet(state);
            }
        }

        void updateFire(float elapsed, std::span<const osg::Vec3f> positions)
        {
            if (mElement != Ragdoll::DeathElement::Fire || positions.empty())
                return;
            if (elapsed >= mSettings.mFireEffectDuration)
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
                mFire->setName("Ragdoll body fire and smoke");
                mFire->setNodeMask(Mask_Effect);
                mFlameState = particleState(false);
                mSmokeState = particleState(true);
                mRoot->addChild(mFire);
            }
            mFire->setMatrix(osg::Matrixf::inverse(rootWorldMatrix(*mRoot)));
            if (elapsed - mLastFireFrame < 1.f / 30.f)
                return;
            mLastFireFrame = elapsed;
            osg::ref_ptr<osg::Group> particles = new osg::Group;
            const float ending = std::clamp((mSettings.mFireEffectDuration - elapsed) / 0.5f, 0.f, 1.f);
            for (const bool smoke : { false, true })
            {
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
                    billboard->addDrawable(quad(size, smoke ? size : size * 1.8f,
                        std::sin(age * 3.14159265f) * ending), point);
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
    float RagdollAppearance::getBlend() const { return mImpl->mBlend; }
    bool RagdollAppearance::hasFire() const { return bool(mImpl->mFire); }
}
