#ifndef OPENMW_MWRENDER_BRUTALBLOODLIGHTINGBASE_HPP
#define OPENMW_MWRENDER_BRUTALBLOODLIGHTINGBASE_HPP

#include <algorithm>
#include <array>
#include <functional>
#include <map>

#include <osg/Camera>
#include <osg/Group>
#include <osg/Light>
#include <osg/NodeCallback>
#include <osg/Program>
#include <osg/Uniform>
#include <osg/UserDataContainer>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>


namespace MWRender
{
    inline constexpr int BloodMaxPointLights = 8;

    // Blood keeps its own small lighting palette instead of assuming one of
    // OpenMW's uniform/UBO lighting layouts. The inputs still come from the
    // actual scene LightManager, including animated and negative lights.
    inline osg::ref_ptr<osg::StateSet> createBloodLightingState(
        bool classic = false, bool clamp = true, int fogMode = 0, bool radial = false)
    {
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        constexpr auto mode = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        state->addUniform(new osg::Uniform("brutalBloodSunDirection", osg::Vec3f()), mode);
        state->addUniform(new osg::Uniform("brutalBloodSunDiffuse", osg::Vec3f()), mode);
        state->addUniform(new osg::Uniform("brutalBloodPointCount", 0), mode);
        for (const char* name : { "brutalBloodPointPosition", "brutalBloodPointDiffuse", "brutalBloodPointAmbient" })
            state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_VEC3, name, BloodMaxPointLights), mode);
        state->addUniform(new osg::Uniform(osg::Uniform::FLOAT_VEC4, "brutalBloodPointAttenuation", BloodMaxPointLights), mode);
        state->addUniform(new osg::Uniform("brutalBloodClassicFalloff", classic), mode);
        state->addUniform(new osg::Uniform("brutalBloodClamp", clamp), mode);
        state->addUniform(new osg::Uniform("brutalBloodFogMode", fogMode), mode);
        state->addUniform(new osg::Uniform("brutalBloodFogRadial", radial), mode);
        return state;
    }

    inline osg::Vec3f bloodLightRGB(const osg::Vec4f& color)
    {
        return { color.r(), color.g(), color.b() };
    }

    inline void setBloodSunLight(osg::StateSet& state, const osg::Light& light, const osg::Matrixf& view)
    {
        const auto direction = light.getPosition() * view;
        state.getUniform("brutalBloodSunDirection")->set(osg::Vec3f(direction.x(), direction.y(), direction.z()));
        state.getUniform("brutalBloodSunDiffuse")->set(bloodLightRGB(light.getDiffuse()));
    }

    inline void setBloodPointLight(osg::StateSet& state, int index, const osg::Light& light, float radius,
        const osg::Matrixf& view)
    {
        if (index < 0 || index >= BloodMaxPointLights) return;
        const auto position = light.getPosition() * view;
        state.getUniform("brutalBloodPointPosition")->setElement(index, osg::Vec3f(position.x(), position.y(), position.z()));
        state.getUniform("brutalBloodPointDiffuse")->setElement(index, bloodLightRGB(light.getDiffuse()));
        state.getUniform("brutalBloodPointAmbient")->setElement(index, bloodLightRGB(light.getAmbient()));
        state.getUniform("brutalBloodPointAttenuation")->setElement(index,
            osg::Vec4f(light.getConstantAttenuation(), light.getLinearAttenuation(), light.getQuadraticAttenuation(), radius));
        state.getUniform("brutalBloodPointCount")->set(index + 1);
    }

    using BloodLightingFactory = std::function<osg::Callback*()>;

    class BloodLightingRoot : public osg::Object
    {
    public:
        BloodLightingRoot() = default;
        explicit BloodLightingRoot(BloodLightingFactory factory) : mFactory(std::move(factory)) {}
        BloodLightingRoot(const BloodLightingRoot& rhs, const osg::CopyOp& copy)
            : osg::Object(rhs, copy), mFactory(rhs.mFactory), mShadowVertex(rhs.mShadowVertex)
            , mShadowFragment(rhs.mShadowFragment), mPrograms(rhs.mPrograms) {}
        META_Object(MWRender, BloodLightingRoot)
        BloodLightingFactory mFactory;
        osg::ref_ptr<osg::Shader> mShadowVertex, mShadowFragment;
        std::map<const osg::Program*, osg::ref_ptr<osg::Program>> mPrograms;
    };

    inline void setBloodLightingRoot(osg::Group& root, BloodLightingFactory factory,
        osg::Shader* shadowVertex = nullptr, osg::Shader* shadowFragment = nullptr)
    {
        osg::ref_ptr<BloodLightingRoot> context = new BloodLightingRoot(std::move(factory));
        context->mShadowVertex = shadowVertex;
        context->mShadowFragment = shadowFragment;
        root.getOrCreateUserDataContainer()->addUserObject(context);
    }

    // A corpse's blood root can be several transforms below the scene root.
    // Find the installed adapter once when the effect is created, never by
    // searching the world graph for every drop or for every rendered frame.
    inline void attachBloodLighting(osg::Node& receiver, osg::Node& root)
    {
        auto paths = root.getParentalNodePaths();
        if (paths.empty()) paths.push_back({ &root });
        for (const auto& path : paths)
            for (auto it = path.rbegin(); it != path.rend(); ++it)
                if (auto* data = (*it)->getUserDataContainer())
                    for (unsigned i = 0; i < data->getNumUserObjects(); ++i)
                        if (auto* context = dynamic_cast<BloodLightingRoot*>(data->getUserObject(i)))
                        {
                            if (context->mFactory)
                                if (auto* callback = context->mFactory()) receiver.addCullCallback(callback);
                            if (context->mShadowVertex && context->mShadowFragment)
                            {
                                auto* state = receiver.getOrCreateStateSet();
                                auto* original = dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM));
                                if (original)
                                {
                                    auto& program = context->mPrograms[original];
                                    if (!program)
                                    {
                                        program = new osg::Program(*original, osg::CopyOp::SHALLOW_COPY);
                                        for (unsigned shader = 0; shader < program->getNumShaders();)
                                        {
                                            auto* library = program->getShader(shader);
                                            if (library->getName().starts_with("Brutal blood fallback shadows"))
                                                program->removeShader(library);
                                            else ++shader;
                                        }
                                        // These ShaderManager-owned objects are shared and regenerated
                                        // when shadow settings/global defines change during gameplay.
                                        program->addShader(context->mShadowVertex);
                                        program->addShader(context->mShadowFragment);
                                    }
                                    state->setAttributeAndModes(program, osg::StateAttribute::ON
                                        | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
                                }
                            }
                            return;
                        }
    }

}

#endif
