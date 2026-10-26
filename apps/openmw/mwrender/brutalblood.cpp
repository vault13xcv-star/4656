#include "brutalblood.hpp"
#include "bloodcoverage.hpp"
#include "brutalbloodlightingbase.hpp"
#include "vismask.hpp"
#include <components/ragdoll/blood.hpp>
#include <components/esm/serializerefid.hpp>
#include <components/sceneutil/depth.hpp>
#include <osg/BlendFunc>
#include <osg/Geode>
#include <osg/Depth>
#include <osg/Program>
#include <osg/PrimitiveSet>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osg/Stencil>
#include <osgUtil/RenderBin>
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <limits>
#include <unordered_map>
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <cmath>

namespace MWRender
{
    namespace
    {
        // Count visible blood fragments, not transparent decal rectangles.
        // A dedicated bin clears only its two counter bits for each camera;
        // the scene's depth buffer and the other stencil bits remain intact.
        class BloodRenderBin final : public osgUtil::RenderBin
        {
        public:
            BloodRenderBin() = default;
            BloodRenderBin(const BloodRenderBin& rhs, const osg::CopyOp& copy)
                : osgUtil::RenderBin(rhs, copy) {}
            META_Object(MWRender, BloodRenderBin)
            void drawImplementation(osg::RenderInfo& info, osgUtil::RenderLeaf*& previous) override
            {
                GLint oldMask = 0, oldClear = 0;
                glGetIntegerv(GL_STENCIL_WRITEMASK, &oldMask);
                glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &oldClear);
                const auto reset = [] {
                    glStencilMask(3); glClearStencil(0); glClear(GL_STENCIL_BUFFER_BIT);
                };
                reset(); glStencilMask(oldMask); glClearStencil(oldClear);
                osgUtil::RenderBin::drawImplementation(info, previous);
                reset();
                // OSG may have changed the stencil mask during this bin.
                // Restore the final draw state's mask, then invalidate the
                // attribute cache so subsequent bins reapply their own state.
                glStencilMask(oldMask); glClearStencil(oldClear);
                info.getState()->haveAppliedAttribute(osg::StateAttribute::STENCIL);
            }
        };

        std::uint64_t bloodChunk(std::uint64_t sequence, bool pool)
        {
            return sequence / BloodDecals::ChunkStains * 2 + (pool ? 0 : 1);
        }

        bool repeatedBloodStamp(const BloodStamp& a, const BloodStamp& b)
        {
            return a.mCenter == b.mCenter && a.mNormal == b.mNormal && a.mRadius == b.mRadius
                && a.mPool == b.mPool && (!a.mVisualSeed || a.mVisualSeed == b.mVisualSeed)
                && a.mTriangles == b.mTriangles;
        }

        bool sameSplashPatch(const BloodStamp& a, const BloodStamp& b)
        {
            if (a.mPool || b.mPool || a.mNormal * b.mNormal < .98f
                || std::abs((a.mCenter - b.mCenter) * a.mNormal) > 4.f) return false;
            unsigned depth = 0;
            for (unsigned axis = 1; axis < 3; ++axis)
                if (std::abs(a.mNormal[axis]) > std::abs(a.mNormal[depth])) depth = axis;
            for (unsigned axis = 0; axis < 3; ++axis)
                if (axis != depth && std::floor(a.mCenter[axis] / 32.f) != std::floor(b.mCenter[axis] / 32.f))
                    return false;
            return true;
        }

        osg::Vec3f unit(osg::Vec3f v, osg::Vec3f fallback = { 0, 0, 1 })
        {
            return std::isfinite(v.length2()) && v.normalize() > .0001f ? v : fallback;
        }
        osg::Matrixd worldMatrix(osg::Node& root)
        {
            const auto paths = root.getParentalNodePaths();
            return paths.empty() ? osg::Matrixd::identity() : osg::computeLocalToWorld(paths.front());
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
        bool validStampMetadata(const BloodStamp& stamp)
        {
            return std::isfinite(stamp.mRadius) && stamp.mRadius > 0 && stamp.mRadius <= 256
                && std::isfinite(stamp.mCenter.length2()) && std::isfinite(stamp.mNormal.length2())
                && std::abs(stamp.mNormal.length2() - 1.f) <= .05f;
        }
        bool validStamp(const BloodStamp& stamp)
        {
            if (stamp.mTriangles.empty() || stamp.mTriangles.size() % 3
                || stamp.mTriangles.size() > ESM::BrutalBloodState::MaxVerticesPerStain
                || !validStampMetadata(stamp)) return false;
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

        osg::Texture2D* bloodSpriteTexture()
        {
            // One small immutable RGBA mask is shared by every airborne spray.
            // The colour remains in the particle tint, so the same native light
            // and shadow response applies to the whole droplet texture.
            static const osg::ref_ptr<osg::Texture2D> texture = [] {
                constexpr int width = 64, height = 128;
                osg::ref_ptr<osg::Image> image = new osg::Image;
                image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                image->setOrigin(osg::Image::BOTTOM_LEFT);
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x)
                    {
                        const float u = (x + .5f) * 2.f / width - 1.f;
                        const float v = (y + .5f) * 2.f / height - 1.f;
                        // A narrow leading tip and a rounded trailing body.
                        const float radius = std::sqrt(std::max(0.f, 1.f - v * v)) * (.65f - .25f * v);
                        const float alpha = std::clamp((radius - std::abs(u)) * width * .25f, 0.f, 1.f);
                        image->setColor({ 1, 1, 1, alpha }, x, y);
                    }
                osg::ref_ptr<osg::Texture2D> result = new osg::Texture2D(image);
                result->setResizeNonPowerOfTwoHint(false);
                result->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
                result->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
                result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
                result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
                result->setUseHardwareMipMapGeneration(false);
                return result;
            }();
            return texture;
        }
    }

    static osg::ref_ptr<osg::StateSet> makeWetGoreState(bool decal, bool sprite = false)
    {
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        constexpr unsigned on = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        constexpr unsigned off = osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
        const bool transparent = decal || sprite;
        state->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::LEQUAL, 0, 1, !transparent), on);
        state->setMode(GL_LIGHTING, off);
        state->setMode(GL_CULL_FACE, off);
        state->setMode(GL_ALPHA_TEST, off);
        state->setMode(GL_BLEND, transparent ? on : off);
        if (transparent)
            state->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), on);
        state->setRenderingHint(transparent ? osg::StateSet::TRANSPARENT_BIN : osg::StateSet::OPAQUE_BIN);
        if (decal)
        {
            static std::once_flag registered;
            std::call_once(registered, [] {
                osgUtil::RenderBin::addRenderBinPrototype("BrutalBloodBin", new BloodRenderBin);
            });
            state->setRenderBinDetails(9, "BrutalBloodBin");
            osg::ref_ptr<osg::Stencil> stencil = new osg::Stencil;
            stencil->setFunction(osg::Stencil::GREATER, 2, 3);
            stencil->setWriteMask(3);
            stencil->setOperation(osg::Stencil::KEEP, osg::Stencil::KEEP, osg::Stencil::INCR);
            state->setAttributeAndModes(stencil, on);
        }
        osg::ref_ptr<osg::Program> program = new osg::Program;
        const std::string vertex =
            "#version 120\nuniform mat4 projectionMatrix; varying vec4 tint; varying vec3 n; varying vec3 p; varying vec4 uv;"
            "void setupShadowCoords(vec4 viewPos,vec3 viewNormal);"
            "void main(){mat4 proj=projectionMatrix; if(proj[0][0]==0.0)proj=gl_ProjectionMatrix;"
            "vec4 eye=gl_ModelViewMatrix*gl_Vertex;gl_Position=proj*eye;n=gl_NormalMatrix*gl_Normal;"
            "p=eye.xyz;tint=" + std::string(decal ? "vec4(gl_Color.r,gl_Color.b,gl_MultiTexCoord1.xy)" : "gl_Color") + ";"
            "uv=vec4(gl_MultiTexCoord0.xy,gl_MultiTexCoord2.xy);"
            "setupShadowCoords(eye,normalize(n));}";
        const std::string spriteVertex =
            "#version 120\nuniform mat4 projectionMatrix;varying vec4 tint;varying vec3 n;varying vec3 p;varying vec2 uv;"
            "void setupShadowCoords(vec4 viewPos,vec3 viewNormal);"
            "void main(){mat4 proj=projectionMatrix;if(proj[0][0]==0.0)proj=gl_ProjectionMatrix;"
            "vec4 eye=gl_ModelViewMatrix*gl_Vertex;"
            "vec3 travel=gl_NormalMatrix*gl_Normal;vec2 axis=travel.xy;float visibleTravel=length(axis);"
            "if(visibleTravel<.001)axis=vec2(0.0,1.0);else axis/=visibleTravel;"
            "vec2 across=vec2(axis.y,-axis.x);uv=gl_MultiTexCoord0.xy;"
            "vec2 corner=uv*2.0-1.0;vec2 size=gl_MultiTexCoord1.xy;"
            "size.y=mix(size.x,size.y,clamp(visibleTravel,0.0,1.0));"
            "eye.xy+=across*corner.x*size.x+axis*corner.y*size.y;"
            "gl_Position=proj*eye;p=eye.xyz;tint=gl_Color;"
            "n=normalize(gl_NormalMatrix*vec3(0.0,0.0,1.0));"
            "setupShadowCoords(eye,n);}";
        program->addShader(new osg::Shader(osg::Shader::VERTEX, sprite ? spriteVertex : vertex));
        // A particle card represents a volume, so side lighting must not vanish
        // with a horizontal light or change as its billboard faces the camera.
        // Its diffuse response still scales only actual scene illumination.
        const std::string lighting =
            "uniform vec3 brutalBloodSunDirection;uniform vec3 brutalBloodSunDiffuse;"
            "uniform vec3 brutalBloodPointPosition[8];uniform vec3 brutalBloodPointDiffuse[8];"
            "uniform vec3 brutalBloodPointAmbient[8];uniform vec4 brutalBloodPointAttenuation[8];"
            "uniform int brutalBloodPointCount;uniform bool brutalBloodClassicFalloff;uniform bool brutalBloodClamp;"
            "uniform int brutalBloodFogMode;uniform bool brutalBloodFogRadial;"
            "float unshadowedLightRatio(float distance);"
            "float bloodSpec(vec3 N,vec3 L,vec3 V){if(dot(N,L)<=0.0)return 0.0;"
            "vec3 H=L+V;H/=max(.0001,length(H));return pow(max(0.0,dot(N,H)),24.0);}"
            "float bloodDiffuse(vec3 N,vec3 L){return "
            + std::string(sprite ? ".5+.5*abs(dot(N,L));}" : "max(0.0,dot(N,L));}") +
            "vec3 bloodLighting(vec3 N,vec3 pos,out vec3 spec){"
            "vec3 V=-pos/max(.0001,length(pos));vec3 L=brutalBloodSunDirection/max(.0001,length(brutalBloodSunDirection));"
            "float shadow=unshadowedLightRatio(length(pos));"
            "vec3 light=gl_LightModel.ambient.rgb+brutalBloodSunDiffuse*bloodDiffuse(N,L)*shadow;"
            "spec=brutalBloodSunDiffuse*bloodSpec(N,L,V)*shadow;"
            "for(int i=0;i<8;++i){if(i>=brutalBloodPointCount)break;"
            "vec3 delta=brutalBloodPointPosition[i]-pos;float dist=length(delta);"
            "vec4 att=brutalBloodPointAttenuation[i];"
            "float power=1.0/max(.0001,att.x+att.y*dist+att.z*dist*dist);"
            "if(!brutalBloodClassicFalloff){float fade=clamp(dist/max(.0001,att.w)-1.0,0.0,1.0);"
            "fade=1.0-fade*fade;power*=fade*fade;}"
            "L=delta/max(.0001,dist);light+=(brutalBloodPointAmbient[i]+brutalBloodPointDiffuse[i]*bloodDiffuse(N,L))*power;"
            "spec+=brutalBloodPointDiffuse[i]*bloodSpec(N,L,V)*power;}"
            "light=max(vec3(0.0),light);spec=max(vec3(0.0),spec);"
            "if(brutalBloodClamp)light=min(vec3(1.0),light);return light;}"
            "vec3 bloodFog(vec3 color,vec3 pos){if(brutalBloodFogMode==0)return color;"
            "float dist=brutalBloodFogRadial?length(pos):abs(pos.z);float f=0.0;"
            "if(brutalBloodFogMode==2){float start=gl_Fog.start*.5;"
            "if(gl_Fog.end>start)f=1.0-exp(-2.0*max(0.0,dist-start)/(gl_Fog.end-start));}"
            "else if(gl_Fog.end>gl_Fog.start)f=clamp((dist-gl_Fog.start)/(gl_Fog.end-gl_Fog.start),0.0,1.0);"
            "return mix(color,gl_Fog.color.rgb,clamp(f,0.0,1.0));}";
        const std::string decalFragment =
            "uniform float brutalBloodTime;uniform float brutalBloodLifetime;"
            "uniform sampler2D brutalBloodAtlas;"
            "varying vec4 tint;varying vec3 n;varying vec3 p;varying vec4 uv;"
            "void main(){vec3 N=normalize(n);"
            "float age=max(0.0,brutalBloodTime-tint.z);float dry=min(1.0,age/90.0);"
            "vec3 shade=vec3(tint.x,tint.x*(tint.w>.5?.72413793:.036363636),tint.y);vec4 c=vec4(shade,.94);"
            "if(tint.w>.5){vec4 texel=texture2D(brutalBloodAtlas,uv.xy);"
            "vec3 pigment=vec3(texel.r,texel.g,max(texel.b,texel.r*.11));"
            "c=vec4(pigment*shade,texel.a*.94);"
            "if(tint.w>1.5){vec2 local=uv.zw*2.0-1.0;"
            "float edge=.79+.045*sin(local.x*11.0+sin(local.y*9.0));"
            "float fill=1.0-smoothstep(edge-.18,edge,length(local*vec2(.95,1.08)));"
            "c.rgb=mix(vec3(.34,.009,.036),c.rgb,texel.a);c.a=max(c.a,fill*.96);}}"
            "c.rgb*=1.0-.5*dry;c.a*=clamp((brutalBloodLifetime-age)/10.0,0.0,1.0);"
            "if(c.a<.003)discard;"
            "vec3 spec;vec3 light=bloodLighting(N,p,spec);"
            "gl_FragColor=vec4(bloodFog(c.rgb*light+spec*.035*c.a,p),c.a);}";
        const std::string solidFragment =
            "uniform bool brutalBloodWorldLighting;"
            "varying vec4 tint;varying vec3 n;varying vec3 p;varying vec4 uv;"
            "void main(){vec3 N=normalize(n);vec3 L=normalize(vec3(.3,.4,.8));"
            "float fold=.92+.08*sin(uv.x*110.0);float vein=pow(max(0.0,sin(uv.x*47.0+sin(uv.y*17.0)*2.0)),24.0);"
            "vec3 flesh=mix(tint.rgb,vec3(.23,.025,.045),vein*.5);"
            "float spec=pow(max(0.0,dot(reflect(-L,N),normalize(-p))),28.0);"
            "vec3 color=flesh*fold*(.5+.5*abs(dot(N,L)))+vec3(.32)*spec;"
            "if(brutalBloodWorldLighting){vec3 wetSpec;vec3 light=bloodLighting(N,p,wetSpec);"
            "color=bloodFog(tint.rgb*light+wetSpec*.035,p);}gl_FragColor=vec4(color,1.0);}";
        const std::string spriteFragment =
            "uniform sampler2D brutalBloodSprite;"
            "varying vec4 tint;varying vec3 n;varying vec3 p;varying vec2 uv;"
            "void main(){float alpha=texture2D(brutalBloodSprite,uv).a*tint.a;"
            "if(alpha<.015)discard;"
            "vec3 spec;vec3 light=bloodLighting(normalize(n),p,spec);"
            "gl_FragColor=vec4(bloodFog(tint.rgb*light+spec*.02,p),alpha);}";
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\n" + lighting + (sprite ? spriteFragment : decal ? decalFragment : solidFragment)));
        osg::ref_ptr<osg::Shader> vertexFallback = new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nvoid setupShadowCoords(vec4 viewPos,vec3 viewNormal){}");
        vertexFallback->setName("Brutal blood fallback shadows vertex");
        program->addShader(vertexFallback);
        osg::ref_ptr<osg::Shader> fragmentFallback = new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nfloat unshadowedLightRatio(float distance){return 1.0;}");
        fragmentFallback->setName("Brutal blood fallback shadows fragment");
        program->addShader(fragmentFallback);
        state->setAttributeAndModes(program, on);
        if (decal)
        {
            state->addUniform(new osg::Uniform("brutalBloodTime", 0.f));
            state->addUniform(new osg::Uniform("brutalBloodLifetime", 600.f));
            state->addUniform(new osg::Uniform("brutalBloodAtlas", 0));
        }
        if (sprite)
        {
            state->addUniform(new osg::Uniform("brutalBloodSprite", 0));
            state->setTextureAttributeAndModes(0, bloodSpriteTexture(), on);
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

    static osg::ref_ptr<osg::StateSet> createBloodSpriteState()
    {
        static const auto state = makeWetGoreState(false, true);
        return new osg::StateSet(*state, osg::CopyOp::SHALLOW_COPY);
    }

    struct BloodDecals::CoverageIndex
    {
        static constexpr double BinSize = 32;
        static constexpr std::size_t MaxBinsPerStamp = 4096;
        static constexpr std::size_t MaxNearby = 128;
        static constexpr std::size_t MaxQueryVisits = 16384;
        static constexpr std::size_t RuntimeWork = 8192;
        struct Key
        {
            std::string mCell;
            int mX, mY, mZ;
            bool operator==(const Key&) const = default;
        };
        struct Hash
        {
            std::size_t operator()(const Key& key) const
            {
                std::size_t value = std::hash<std::string>{}(key.mCell);
                for (int coordinate : { key.mX, key.mY, key.mZ })
                    value ^= std::hash<int>{}(coordinate) + 0x9e3779b9u + (value << 6) + (value >> 2);
                return value;
            }
        };
        struct Query
        {
            std::vector<Stain*> mStains;
            bool mComplete = true;
        };
        std::unordered_map<Key, std::vector<Stain*>, Hash> mBins;
        std::unordered_map<const Stain*, osg::BoundingBoxf> mBounds;
        mutable std::uint64_t mVisit = 0;

        static std::string domain(const std::string& cell)
        {
            // TES3 exterior cells belong to one continuous world. A stain can
            // cross a cell border, so its overlap budget must cross it too.
            // Interior StringRefIds are separate even at identical coordinates.
            constexpr auto prefix = ESM::esm3ExteriorCellRefIdPrefix;
            if (cell.starts_with(prefix))
            {
                const auto* begin = cell.data() + prefix.size();
                const auto* end = cell.data() + cell.size();
                std::int32_t x = 0, y = 0;
                const auto first = std::from_chars(begin, end, x);
                if (first.ec == std::errc() && first.ptr != end && *first.ptr == ':')
                {
                    const auto second = std::from_chars(first.ptr + 1, end, y);
                    if (second.ec == std::errc() && second.ptr == end) return std::string(prefix);
                }
            }
            return cell;
        }

        static osg::BoundingBoxf bounds(const BloodStamp& stamp)
        {
            osg::BoundingBoxf result;
            for (const auto& point : stamp.mTriangles) result.expandBy(point);
            return result;
        }
        static osg::BoundingBoxf footprint(const BloodStamp& stamp)
        {
            // Conservative bounds before a deferred stamp is projected. Actual
            // triangles, not this proxy, decide overlap and are stored/indexed.
            const osg::Vec3f up = stamp.mNormal.z() >= .7f ? osg::Vec3f(0, 1, 0) : osg::Vec3f(0, 0, 1);
            const auto vertical = unit(up - stamp.mNormal * (up * stamp.mNormal), { 0, 1, 0 });
            const auto horizontal = unit(vertical ^ stamp.mNormal, { 1, 0, 0 });
            osg::Vec3f extent;
            for (unsigned axis = 0; axis < 3; ++axis)
                extent[axis] = (std::abs(horizontal[axis]) + std::abs(vertical[axis])) * stamp.mRadius
                    + std::abs(stamp.mNormal[axis]) * .2f;
            return { stamp.mCenter - extent, stamp.mCenter + extent };
        }
        template <class Function>
        static bool bins(const std::string& cell, const osg::BoundingBoxf& bounds, Function function)
        {
            if (!bounds.valid()) return false;
            const auto coverageDomain = domain(cell);
            std::array<int, 3> low{}, high{};
            std::size_t count = 1;
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                // Padding also catches the 0.03/0.06 offsets used by older blood
                // builds when a receiver lies exactly on a hash-bin boundary.
                const double a = std::floor((double(bounds._min[axis]) - .1) / BinSize);
                const double b = std::floor((double(bounds._max[axis]) + .1) / BinSize);
                if (!std::isfinite(a) || !std::isfinite(b) || a < std::numeric_limits<int>::min()
                    || b > std::numeric_limits<int>::max() || b < a || b - a + 1 > MaxBinsPerStamp)
                    return false;
                low[axis] = int(a); high[axis] = int(b);
                count *= std::size_t(b - a + 1);
                if (count > MaxBinsPerStamp) return false;
            }
            for (std::int64_t z = low[2]; z <= high[2]; ++z)
                for (std::int64_t y = low[1]; y <= high[1]; ++y)
                    for (std::int64_t x = low[0]; x <= high[0]; ++x)
                        if (!function(Key{ coverageDomain, int(x), int(y), int(z) })) return false;
            return true;
        }
        static bool intersects(const osg::BoundingBoxf& a, const osg::BoundingBoxf& b)
        {
            for (unsigned axis = 0; axis < 3; ++axis)
                if (a._max[axis] + .1f < b._min[axis] || b._max[axis] + .1f < a._min[axis]) return false;
            return true;
        }
        static bool containsBounds(const osg::BoundingBoxf& outer, const osg::BoundingBoxf& inner)
        {
            for (unsigned axis = 0; axis < 3; ++axis)
                if (inner._min[axis] < outer._min[axis] - .08f
                    || inner._max[axis] > outer._max[axis] + .08f) return false;
            return true;
        }
        static bool coversPoint(const BloodStamp& stamp, const osg::Vec3f& point)
        {
            const osg::Vec3d target(point);
            const double tolerance = std::max(.12, std::max({ std::abs(double(point.x())),
                std::abs(double(point.y())), std::abs(double(point.z())) }) * std::numeric_limits<float>::epsilon() * 4);
            for (std::size_t i = 0; i + 2 < stamp.mTriangles.size(); i += 3)
            {
                const osg::Vec3d origin(stamp.mTriangles[i]);
                const auto a = osg::Vec3d(stamp.mTriangles[i + 1]) - origin;
                const auto b = osg::Vec3d(stamp.mTriangles[i + 2]) - origin;
                const auto relative = target - origin;
                const auto normal = a ^ b;
                const double length2 = normal.length2();
                if (length2 <= 1e-16) continue;
                const double height = relative * normal;
                if (height * height > tolerance * tolerance * length2) continue;
                const double u = ((relative ^ b) * normal) / length2;
                const double v = ((a ^ relative) * normal) / length2;
                if (u >= -1e-7 && v >= -1e-7 && u + v <= 1.0000001) return true;
            }
            return false;
        }
        Query query(const std::string& cell, const osg::BoundingBoxf& bounds, const Stain* skip = nullptr) const
        {
            Query result;
            if (++mVisit == 0)
            {
                for (const auto& [stain, box] : mBounds) stain->mCoverageVisit = 0;
                mVisit = 1;
            }
            std::size_t visits = 0;
            result.mComplete = bins(cell, bounds, [&](const Key& key) {
                const auto bin = mBins.find(key);
                if (bin == mBins.end()) return true;
                for (Stain* stain : bin->second)
                {
                    if (++visits > MaxQueryVisits) return false;
                    if (stain == skip || stain->mCoverageVisit == mVisit) continue;
                    stain->mCoverageVisit = mVisit;
                    const auto found = mBounds.find(stain);
                    if (found == mBounds.end() || !intersects(found->second, bounds)) continue;
                    if (result.mStains.size() >= MaxNearby) return false;
                    result.mStains.push_back(stain);
                }
                return true;
            });
            return result;
        }
        bool insert(Stain& stain)
        {
            const auto box = bounds(stain.mStamp);
            if (!bins(stain.mCell, box, [](const Key&) { return true; })) return false;
            mBounds.emplace(&stain, box);
            bins(stain.mCell, box, [&](const Key& key) { mBins[key].push_back(&stain); return true; });
            return true;
        }
        void erase(const Stain& stain)
        {
            const auto found = mBounds.find(&stain);
            if (found == mBounds.end()) return;
            bins(stain.mCell, found->second, [&](const Key& key) {
                const auto bin = mBins.find(key);
                if (bin == mBins.end()) return true;
                std::erase(bin->second, &stain);
                if (bin->second.empty()) mBins.erase(bin);
                return true;
            });
            mBounds.erase(found);
        }
        void clear() { mBins.clear(); mBounds.clear(); mVisit = 0; }
    };

    BloodDecals::BloodDecals(osg::Group& parent)
        : mParent(&parent), mCoverage(std::make_unique<CoverageIndex>()), mTimeUniform(new osg::Uniform("brutalBloodTime", 0.f))
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
        {
            if (chunk.mNode) chunk.mNode->setNodeMask(active ? unsigned(Mask_Effect) : 0u);
            if (chunk.mTransform) chunk.mTransform->setNodeMask(active ? unsigned(Mask_Effect) : 0u);
        }
    }

    void BloodDecals::dirty(const Stain& stain)
    {
        mBatches[stain.mCell].mChunks[bloodChunk(stain.mSequence, stain.mStamp.mPool)].mDirty = true;
    }

    std::list<BloodDecals::Stain>::iterator BloodDecals::eraseStain(std::list<Stain>::iterator stain)
    {
        if (stain->mNeedsRepair) --mPendingPoolRepairs;
        dirty(*stain);
        mVertexCount -= stain->mStamp.mTriangles.size();
        mCoverage->erase(*stain);
        return mStains.erase(stain);
    }

    bool BloodDecals::feedPool(const std::string& cell, const BloodStamp& stamp)
    {
        if (!stamp.mProjector || stamp.mNormal.z() < .7f) return false;
        const auto nearby = mCoverage->query(cell, CoverageIndex::footprint(stamp));
        Stain* pool = nullptr;
        float nearest = std::numeric_limits<float>::max();
        for (Stain* candidate : nearby.mStains)
        {
            const auto& existing = candidate->mStamp;
            if (!existing.mPool || existing.mNormal.z() < .7f || existing.mNormal * stamp.mNormal < .98f) continue;
            const auto offset = stamp.mCenter - existing.mCenter;
            const auto tangent = offset - existing.mNormal * (offset * existing.mNormal);
            const float reach = std::max(6.f, existing.mRadius * .8f + stamp.mRadius * .3f);
            const float distance = tangent.length2();
            if (distance > reach * reach) continue;
            // On curved ground, the impact can be on this pool's actual
            // receiver while lying outside its anchor's tangent plane.
            const bool covered = CoverageIndex::coversPoint(existing, stamp.mCenter);
            if (!covered && std::abs(offset * existing.mNormal) > 2.f) continue;
            // Keep feeding the established puddle rather than promoting each
            // nearby airborne impact into a separate, competing pool anchor.
            const float score = distance / (reach * reach) - (existing.mPool ? 2.f : 0.f) - (covered ? 1.f : 0.f);
            if (score < nearest) { nearest = score; pool = candidate; }
        }
        if (!pool) return false;

        // Nominal radius alone is not coverage: earlier clipping can leave
        // holes or disconnected receiver pieces. Never swallow an impact in
        // such a hole merely because this anchor is nearby.
        const auto denseCoverage = [&](const BloodStamp& target) {
            const auto offset = stamp.mCenter - target.mCenter;
            const auto tangent = offset - target.mNormal * (offset * target.mNormal);
            return tangent.length2() < target.mRadius * target.mRadius * .2f
                && CoverageIndex::coversPoint(target, stamp.mCenter);
        };
        const bool coveredBefore = denseCoverage(pool->mStamp);
        // Ballistic detail is merged only where opaque liquid already exists.
        // Outside that core it must leave its own visible splash.
        if (!stamp.mPool && !coveredBefore) return false;
        const auto touch = [&]() {
            if (mTime >= pool->mNextWetRefresh)
            {
                pool->mAge = 0;
                pool->mNextWetRefresh = mTime + PoolGrowthInterval;
                dirty(*pool);
            }
        };
        if (coveredBefore) touch();
        const float volume = stamp.mRadius * stamp.mRadius * .12f;
        const float limit = std::clamp(stamp.mPoolLimit, 1.f, MaxPoolRadius);
        pool->mAccumulatedArea = std::min(limit * limit, pool->mAccumulatedArea + volume);
        if (mTime < pool->mNextGrowth || mPoolGrowths >= MaxPoolGrowthsPerFrame) return coveredBefore;
        const float oldRadius = pool->mStamp.mRadius;
        const float desired = std::min({ std::max(oldRadius, limit), oldRadius + 8.f,
            std::max(stamp.mPool ? stamp.mRadius : oldRadius,
                std::sqrt(oldRadius * oldRadius + pool->mAccumulatedArea)) });
        if (desired < oldRadius + .5f)
        {
            pool->mNextGrowth = mTime + PoolGrowthInterval;
            return coveredBefore;
        }
        ++mPoolGrowths;
        pool->mNextGrowth = mTime + PoolGrowthInterval;

        BloodStamp grown = pool->mStamp;
        grown.mProjector = {};
        grown.mPool = true;
        grown.mRadius = desired;
        grown.mTriangles = stamp.mProjector(grown.mCenter, grown.mNormal, grown.mRadius);
        if (!validStamp(grown)) return coveredBefore;
        auto neighbors = mCoverage->query(cell, CoverageIndex::bounds(grown), pool);

        // Absorb small decals that the actual pool geometry already contains,
        // or that an accepted growth will contain. This reduces both fragments
        // and draw overdraw instead of endlessly cutting a bigger pool around
        // dozens of little stains. A work-limited proof must finish before an
        // old stain can be removed; an empty budget result is not containment.
        std::vector<Stain*> absorbed;
        const auto oldBounds = CoverageIndex::bounds(pool->mStamp);
        const auto grownBounds = CoverageIndex::bounds(grown);
        for (Stain* other : neighbors.mStains)
        {
            if (mPoolAbsorptionChecks >= MaxPoolAbsorptionChecksPerFrame) break;
            if (other->mStamp.mNormal * grown.mNormal < .98f || other->mStamp.mNormal.z() < .7f
                || other->mStamp.mRadius > grown.mRadius) continue;
            const auto otherBounds = CoverageIndex::bounds(other->mStamp);
            const bool insideOldBox = CoverageIndex::containsBounds(oldBounds, otherBounds);
            if (!insideOldBox && !CoverageIndex::containsBounds(grownBounds, otherBounds)) continue;
            ++mPoolAbsorptionChecks;
            const auto& covering = insideOldBox ? pool->mStamp : grown;
            // The decal square includes transparent margins. Only absorb
            // detail fully inside the opaque fluid, never its dry perimeter.
            if (!other->mStamp.mPool && !std::all_of(other->mStamp.mTriangles.begin(), other->mStamp.mTriangles.end(),
                [&](const osg::Vec3f& vertex) {
                    const auto offset = vertex - covering.mCenter;
                    const auto tangent = offset - covering.mNormal * (offset * covering.mNormal);
                    return tangent.length2() < covering.mRadius * covering.mRadius * .2f;
                })) continue;
            if (!bloodStampContains(covering, other->mStamp)) continue;
            if (insideOldBox) eraseStain(other->mSelf);
            else absorbed.push_back(other);
        }
        // Erasing covered old details can make progress even if the new growth
        // cannot yet be admitted this frame.
        grown.mTriangles = clipBloodCoverage(grown, {}, ESM::BrutalBloodState::MaxVerticesPerStain,
            CoverageIndex::RuntimeWork);
        std::size_t removedVertices = 0;
        for (const Stain* other : absorbed) removedVertices += other->mStamp.mTriangles.size();
        if (!validStamp(grown) || mVertexCount - removedVertices - pool->mStamp.mTriangles.size() + grown.mTriangles.size() > MaxVertices)
            return coveredBefore;
        // An indexed replacement must remain bounded even for corrupt callers.
        if (!CoverageIndex::bins(cell, CoverageIndex::bounds(grown), [](const CoverageIndex::Key&) { return true; }))
            return coveredBefore;
        for (Stain* other : absorbed) eraseStain(other->mSelf);
        mCoverage->erase(*pool);
        mVertexCount -= pool->mStamp.mTriangles.size();
        pool->mStamp = std::move(grown);
        mVertexCount += pool->mStamp.mTriangles.size();
        mCoverage->insert(*pool);
        pool->mAccumulatedArea = std::max(0.f, pool->mAccumulatedArea - (desired * desired - oldRadius * oldRadius));
        dirty(*pool);
        const bool coveredAfter = denseCoverage(pool->mStamp);
        if (coveredAfter) touch();
        return coveredAfter;
    }

    bool BloodDecals::feedSplash(const std::string& cell, const BloodStamp& stamp)
    {
        if (stamp.mPool || !stamp.mProjector) return false;
        const osg::Vec3f extent(64, 64, 64);
        const auto nearby = mCoverage->query(cell, { stamp.mCenter - extent, stamp.mCenter + extent });
        Stain* selected = nullptr;
        unsigned count = 0;
        float nearest = std::numeric_limits<float>::max();
        for (Stain* other : nearby.mStains)
            if (sameSplashPatch(stamp, other->mStamp))
            {
                ++count;
                const float distance = (stamp.mCenter - other->mStamp.mCenter).length2();
                if (distance < nearest) { nearest = distance; selected = other; }
            }
        if (count < 2) return false;
        // Keep two distinct pieces of artwork per small surface patch. Further
        // blood enlarges one of them instead of adding hundreds of new quads.
        const float limit = std::clamp(stamp.mPoolLimit * .48f, 24.f, 96.f);
        selected->mAccumulatedArea = std::min(limit * limit,
            selected->mAccumulatedArea + stamp.mRadius * stamp.mRadius * .12f);
        if (mTime >= selected->mNextWetRefresh)
        {
            selected->mAge = 0;
            selected->mNextWetRefresh = mTime + PoolGrowthInterval;
            dirty(*selected);
        }
        if (mTime < selected->mNextGrowth || mSplashGrowths >= MaxSplashGrowthsPerFrame) return true;
        const float oldRadius = selected->mStamp.mRadius;
        const float radius = std::min({ std::max(oldRadius, limit), oldRadius + 4,
            std::sqrt(oldRadius * oldRadius + selected->mAccumulatedArea) });
        selected->mNextGrowth = mTime + PoolGrowthInterval;
        if (radius < oldRadius + .5f) return true;
        ++mSplashGrowths;
        auto grown = selected->mStamp;
        grown.mRadius = radius;
        grown.mTriangles = stamp.mProjector(grown.mCenter, grown.mNormal, radius);
        if (!validStamp(grown)) return true;
        grown.mTriangles = clipBloodCoverage(grown, {}, ESM::BrutalBloodState::MaxVerticesPerStain,
            CoverageIndex::RuntimeWork);
        if (!validStamp(grown) || mVertexCount - selected->mStamp.mTriangles.size() + grown.mTriangles.size() > MaxVertices
            || !CoverageIndex::bins(cell, CoverageIndex::bounds(grown), [](const CoverageIndex::Key&) { return true; }))
            return true;
        mCoverage->erase(*selected);
        mVertexCount -= selected->mStamp.mTriangles.size();
        selected->mStamp = std::move(grown);
        mVertexCount += selected->mStamp.mTriangles.size();
        selected->mAccumulatedArea = std::max(0.f,
            selected->mAccumulatedArea - (radius * radius - oldRadius * oldRadius));
        mCoverage->insert(*selected);
        dirty(*selected);
        return true;
    }

    void BloodDecals::add(const std::string& cell, const BloodStamp& stamp)
    {
        if (!validStampMetadata(stamp)) return;
        // Deferred deposits are accepted only with a live, ephemeral projector;
        // empty saved geometry cannot enter this path during restoration.
        if (stamp.mTriangles.empty() && !stamp.mProjector) return;
        EmitterBudget* emitter = nullptr;
        if (stamp.mProjector && stamp.mTriangles.empty())
        {
            if (mEmitters.size() >= 128 && !mEmitters.contains(stamp.mEmitter)) return;
            emitter = &mEmitters[stamp.mEmitter];
            emitter->mLastSeen = mTime;
        }
        if (feedPool(cell, stamp)) return;
        if (feedSplash(cell, stamp)) return;
        if (stamp.mProjector && stamp.mTriangles.empty())
        {
            if (mNewDeposits >= MaxNewDepositsPerFrame || mProjectionAttempts >= MaxProjectionAttemptsPerFrame
                || emitter->mCredit < 1) return;
            emitter->mCredit -= 1;
            ++mProjectionAttempts;
        }
        BloodStamp incoming = stamp;
        incoming.mProjector = {};
        if (incoming.mTriangles.empty()) incoming.mTriangles = stamp.mProjector(stamp.mCenter, stamp.mNormal, stamp.mRadius);
        if (!validStamp(incoming)) return;
        const auto nearby = mCoverage->query(cell, CoverageIndex::bounds(incoming));
        unsigned repeats = 0;
        for (const Stain* other : nearby.mStains)
            if (repeatedBloodStamp(incoming, other->mStamp) && ++repeats == MaxOverlappingLayers) return;
        incoming.mTriangles = clipBloodCoverage(incoming, {}, ESM::BrutalBloodState::MaxVerticesPerStain,
            stamp.mProjector ? CoverageIndex::RuntimeWork : 32768);
        if (!validStamp(incoming)
            || !CoverageIndex::bins(cell, CoverageIndex::bounds(incoming), [](const CoverageIndex::Key&) { return true; })) return;
        while (!mStains.empty() && (mStains.size() >= MaxStains
            || mVertexCount + incoming.mTriangles.size() > MaxVertices)) eraseStain(mStains.begin());
        mStains.push_back({ { cell, std::move(incoming), 0 }, mNextSequence++ });
        auto& added = mStains.back();
        added.mSelf = std::prev(mStains.end());
        if (stamp.mProjector) ++mNewDeposits;
        if (!added.mStamp.mVisualSeed) added.mStamp.mVisualSeed = nextSeed(mRandom);
        added.mNextGrowth = added.mNextWetRefresh = mTime + PoolGrowthInterval;
        mVertexCount += added.mStamp.mTriangles.size();
        mCoverage->insert(added);
        dirty(added);
    }

    void BloodDecals::update(float duration, float lifetime)
    {
        if (!std::isfinite(lifetime)) lifetime = 600;
        mLifetime = std::clamp(lifetime, 300.f, 1800.f);
        mLifetimeUniform->set(mLifetime);
        if (!std::isfinite(duration) || duration <= 0) return;
        mNewDeposits = mProjectionAttempts = mPoolGrowths = mSplashGrowths = mPoolAbsorptionChecks = 0;
        mTime += duration;
        std::erase_if(mEmitters, [&](const auto& item) { return mTime - item.second.mLastSeen > 1; });
        const float share = float(MaxProjectionAttemptsPerFrame) / std::max<std::size_t>(1, mEmitters.size());
        for (auto& [id, emitter] : mEmitters) emitter.mCredit = std::min(4.f, emitter.mCredit + share);
        mTimeUniform->set(static_cast<float>(mTime));
        mSinceBatch += duration;
        for (auto& stain : mStains)
        {
            stain.mAge += duration;
        }
        for (auto it = mStains.begin(); it != mStains.end();)
            if (it->mAge >= mLifetime) it = eraseStain(it); else ++it;
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
                    chunk.mGeometry->setTexCoordArray(2, new osg::Vec2Array);
                }
                static_cast<osg::Vec3Array*>(chunk.mGeometry->getVertexArray())->clear();
                static_cast<osg::Vec3Array*>(chunk.mGeometry->getNormalArray())->clear();
                static_cast<osg::Vec4Array*>(chunk.mGeometry->getColorArray())->clear();
                static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(0))->clear();
                static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(1))->clear();
                static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(2))->clear();
                dirtyChunks.emplace_back(&chunk, batch.mActive);
            }
        if (dirtyChunks.empty()) return;
        for (const auto& stain : mStains)
        {
            auto& chunk = mBatches.find(stain.mCell)->second.mChunks.find(bloodChunk(stain.mSequence, stain.mStamp.mPool))->second;
            if (!chunk.mDirty) continue;
            auto* vertices = static_cast<osg::Vec3Array*>(chunk.mGeometry->getVertexArray());
            auto* normals = static_cast<osg::Vec3Array*>(chunk.mGeometry->getNormalArray());
            auto* colors = static_cast<osg::Vec4Array*>(chunk.mGeometry->getColorArray());
            auto* coords = static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(0));
            auto* births = static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(1));
            auto* localCoords = static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(2));
            const auto& stamp = stain.mStamp;
            // Pool liquid goes below all splash detail, including when those
            // stains belong to different cells/chunks. Empty PNG margins never
            // reserve a layer or cut geometry out of the underlying pool.
            chunk.mGeometry->getOrCreateStateSet()->setRenderBinDetails(stamp.mPool ? 0 : 1, "RenderBin");
            if (!chunk.mTransform)
            {
                // Keep GPU positions close to zero. Multiplying large exterior
                // world coordinates by a float model-view matrix loses the tiny
                // receiver offset and makes blood fail the ground depth test.
                // OSG combines this translation with the view in double
                // precision; saved geometry and texture coordinates stay world-space.
                chunk.mOrigin = stamp.mCenter;
                chunk.mTransform = new osg::MatrixTransform;
                chunk.mTransform->setName("Brutal Morrowind blood chunk");
                chunk.mTransform->setNodeMask(Mask_Effect);
                chunk.mTransform->setMatrix(osg::Matrixd::translate(osg::Vec3d(chunk.mOrigin)));
            }
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
            osg::Vec3f surfaceNormal = stamp.mNormal;
            for (std::size_t corner = 0; corner < stamp.mTriangles.size(); ++corner)
            {
                const auto& vertex = stamp.mTriangles[corner];
                if (corner % 3 == 0)
                {
                    surfaceNormal = unit((stamp.mTriangles[corner + 1] - vertex)
                        ^ (stamp.mTriangles[corner + 2] - vertex), stamp.mNormal);
                    if (surfaceNormal * stamp.mNormal < 0) surfaceNormal = -surfaceNormal;
                }
                const auto relative = vertex - stamp.mCenter;
                float u = std::clamp(.5f + (relative * horizontal) / (stamp.mRadius * 2), 0.f, 1.f);
                float v = std::clamp(.5f + (relative * vertical) / (stamp.mRadius * 2), 0.f, 1.f);
                if (floor && (seed & 2)) { u = 1 - u; v = 1 - v; }
                if (seed & 1) u = 1 - u;
                vertices->push_back(vertex - chunk.mOrigin);
                normals->push_back(surfaceNormal);
                colors->push_back(tint);
                coords->push_back({ tile.x() + u * (tile.z() - tile.x()), tile.y() + v * (tile.w() - tile.y()) });
                births->push_back({ static_cast<float>(mTime - stain.mAge), textured ? (floor && stamp.mPool ? 2.f : 1.f) : 0.f });
                localCoords->push_back({ u, v });
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
                attachBloodLighting(*chunk->mNode, *mParent);
                chunk->mNode->addDrawable(chunk->mGeometry);
                chunk->mTransform->addChild(chunk->mNode);
                mParent->addChild(chunk->mTransform);
            }
            vertices->dirty();
            chunk->mGeometry->getNormalArray()->dirty();
            chunk->mGeometry->getColorArray()->dirty();
            chunk->mGeometry->getTexCoordArray(0)->dirty();
            chunk->mGeometry->getTexCoordArray(1)->dirty();
            chunk->mGeometry->getTexCoordArray(2)->dirty();
            static_cast<osg::DrawArrays*>(chunk->mGeometry->getPrimitiveSet(0))->setCount(vertices->getNumElements());
            chunk->mGeometry->dirtyBound();
            chunk->mNode->setNodeMask(active ? unsigned(Mask_Effect) : 0u);
            chunk->mTransform->setNodeMask(active ? unsigned(Mask_Effect) : 0u);
            chunk->mDirty = false;
        }
        for (auto& [cell, batch] : mBatches)
            for (auto it = batch.mChunks.begin(); it != batch.mChunks.end();)
            {
                if (it->second.mDirty && !it->second.mGeometry->getVertexArray()->getNumElements())
                {
                    if (it->second.mTransform) mParent->removeChild(it->second.mTransform);
                    it = batch.mChunks.erase(it);
                }
                else ++it;
            }
    }
    void BloodDecals::clear()
    {
        for (auto& [cell, batch] : mBatches)
            for (auto& [id, chunk] : batch.mChunks)
                if (chunk.mTransform) mParent->removeChild(chunk.mTransform);
        mBatches.clear();
        mCoverage->clear();
        mEmitters.clear();
        mStains.clear();
        mVertexCount = 0;
        mPendingPoolRepairs = 0;
        mNextSequence = 0; mTime = 0; mSinceBatch = 1;
        mNewDeposits = mProjectionAttempts = mPoolGrowths = mSplashGrowths = mPoolAbsorptionChecks = 0;
        mTimeUniform->set(0.f);
    }
    std::vector<BloodStainState> BloodDecals::saveState() const { return { mStains.begin(), mStains.end() }; }
    void BloodDecals::restoreState(const std::vector<BloodStainState>& state)
    {
        // Preserve visibility flags established while loading active cells.
        for (auto& [cell, batch] : mBatches)
        {
            for (auto& [id, chunk] : batch.mChunks)
                if (chunk.mTransform) mParent->removeChild(chunk.mTransform);
            batch.mChunks.clear();
        }
        mCoverage->clear();
        mEmitters.clear();
        mStains.clear();
        mVertexCount = 0;
        mPendingPoolRepairs = 0;
        mNextSequence = 0; mTime = 0; mSinceBatch = 1;
        mNewDeposits = mProjectionAttempts = mPoolGrowths = mSplashGrowths = mPoolAbsorptionChecks = 0;
        mTimeUniform->set(0.f);
        // Prefer the newest visible blood when compacting an old save that
        // stacked dozens of deposits at one location. Restore chronological
        // ordering afterwards so global oldest-first eviction remains correct.
        for (auto source = state.rbegin(); source != state.rend(); ++source)
        {
            BloodStainState stain = *source;
            stain.mStamp.mProjector = {};
            if (!std::isfinite(stain.mAge) || stain.mAge < 0 || stain.mAge >= 1800) continue;
            if (mStains.size() >= MaxStains) break;
            if (!validStamp(stain.mStamp) || mVertexCount + stain.mStamp.mTriangles.size() > MaxVertices) continue;
            const auto neighbors = mCoverage->query(stain.mCell, CoverageIndex::bounds(stain.mStamp));
            unsigned repeats = 0;
            for (const Stain* other : neighbors.mStains)
                if (repeatedBloodStamp(stain.mStamp, other->mStamp)) ++repeats;
            if (repeats >= MaxOverlappingLayers) continue;
            stain.mStamp.mTriangles = clipBloodCoverage(stain.mStamp, {});
            if (!validStamp(stain.mStamp)
                || !CoverageIndex::bins(stain.mCell, CoverageIndex::bounds(stain.mStamp),
                    [](const CoverageIndex::Key&) { return true; })) continue;
            // Do not rejuvenate saved blood or run a saved callback.
            mStains.push_front({ std::move(stain), 0 });
            auto& restored = mStains.front();
            restored.mSelf = mStains.begin();
            restored.mNeedsRepair = restored.mStamp.mPool;
            if (restored.mNeedsRepair) ++mPendingPoolRepairs;
            if (!restored.mStamp.mVisualSeed) restored.mStamp.mVisualSeed = nextSeed(mRandom);
            restored.mNextGrowth = restored.mNextWetRefresh = PoolGrowthInterval;
            mVertexCount += restored.mStamp.mTriangles.size();
            mCoverage->insert(restored);
        }
        for (auto& stain : mStains) { stain.mSequence = mNextSequence++; dirty(stain); }
        rebuild();
    }

    void BloodDecals::repairPools(const std::function<BloodStamp::Projector(
        const std::string&, const osg::Vec3f&, float)>& surface)
    {
        if (!mPendingPoolRepairs || !surface) return;
        unsigned attempts = 0;
        for (auto& stain : mStains)
        {
            if (!stain.mNeedsRepair || !mBatches[stain.mCell].mActive) continue;
            if (++attempts > 2) break;
            auto projector = surface(stain.mCell, stain.mStamp.mCenter, stain.mStamp.mRadius);
            if (!projector) continue; // The frame's terrain-query budget may already be spent.
            stain.mNeedsRepair = false; --mPendingPoolRepairs;
            auto repaired = stain.mStamp;
            repaired.mTriangles = projector(repaired.mCenter, repaired.mNormal, repaired.mRadius);
            if (!validStamp(repaired)) continue;
            repaired.mTriangles = clipBloodCoverage(repaired, {});
            if (!validStamp(repaired)
                || mVertexCount - stain.mStamp.mTriangles.size() + repaired.mTriangles.size() > MaxVertices
                || !CoverageIndex::bins(stain.mCell, CoverageIndex::bounds(repaired), [](const CoverageIndex::Key&) { return true; }))
                continue;
            mCoverage->erase(stain);
            mVertexCount -= stain.mStamp.mTriangles.size();
            stain.mStamp = std::move(repaired);
            mVertexCount += stain.mStamp.mTriangles.size();
            mCoverage->insert(stain);
            dirty(stain); // Preserve age, texture identity and saved radius.
        }
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

    BloodTrail::BloodTrail(osg::Group& root, Raycast raycast, Deposit deposit, Projector projector, bool deferProjection)
        : mRoot(&root), mTransform(new osg::MatrixTransform)
        , mGeometry(geometry(new osg::Vec3Array, new osg::Vec3Array, new osg::Vec4Array))
        , mRaycast(std::move(raycast)), mProjector(std::move(projector)), mDeposit(std::move(deposit))
        , mDeferProjection(deferProjection)
    {
        mTransform->setName("Brutal Morrowind ballistic blood");
        mTransform->setNodeMask(Mask_Effect);
        mGeometry->setTexCoordArray(0, new osg::Vec2Array);
        mGeometry->setTexCoordArray(1, new osg::Vec2Array);
        mGeometry->setPrimitiveSet(0, new osg::DrawElementsUShort(GL_TRIANGLES));
        static_cast<osg::Vec3Array*>(mGeometry->getVertexArray())->reserve(MaxParticles * SpriteVertices);
        static_cast<osg::Vec3Array*>(mGeometry->getNormalArray())->reserve(MaxParticles * SpriteVertices);
        static_cast<osg::Vec4Array*>(mGeometry->getColorArray())->reserve(MaxParticles * SpriteVertices);
        static_cast<osg::Vec2Array*>(mGeometry->getTexCoordArray(0))->reserve(MaxParticles * SpriteVertices);
        static_cast<osg::Vec2Array*>(mGeometry->getTexCoordArray(1))->reserve(MaxParticles * SpriteVertices);
        static_cast<osg::DrawElementsUShort*>(mGeometry->getPrimitiveSet(0))->reserve(MaxParticles * SpriteIndices);
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->setStateSet(createBloodSpriteState());
        attachBloodLighting(*geode, root);
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
        stamp.mPoolLimit = mPoolLimit;
        stamp.mEmitter = reinterpret_cast<std::uintptr_t>(this);
        if (mProjector)
        {
            stamp.mProjector = mProjector;
            if (mDeferProjection) { mDeposit(stamp); return; }
            stamp.mTriangles = mProjector(position, stamp.mNormal, radius);
            if (validStamp(stamp)) mDeposit(stamp);
            return;
        }
        // Ray-only compatibility for isolated callers. The game runtime uses
        // actual visual triangles through the projector above.
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
        const float launchScale = Ragdoll::bloodLaunchScale(amount);
        const float spreadScale = Ragdoll::bloodSpreadScale(amount);
        mPoolLimit = 100.f * std::sqrt(Ragdoll::bloodAmountMultiplier(amount));
        const float sizeScale = std::sqrt(Ragdoll::bloodEmissionScale(amount));
        amount = Ragdoll::bloodEmissionScale(amount);
        const auto sourceCount = std::min(sources.size(), MaxSources);
        mRemainders.resize(sourceCount, 0);
        // Establish the common footprint on the first emitted drop. Waiting
        // 250 ms lets a multi-wound corpse first fill it with competing tiny
        // splats, blocking the later large pool's bounded clipping work.
        mPoolTimes.resize(sourceCount, BloodDecals::PoolGrowthInterval);
        mPendingDrops.assign(sourceCount, 0);
        unsigned pending = 0;
        for (std::size_t i = 0; i < sourceCount; ++i)
        {
            const auto& source = sources[i];
            if (!std::isfinite(source.mPosition.length2()) || !std::isfinite(source.mDirection.length2())
                || !std::isfinite(source.mPressure)) continue;
            const float rate = source.mNeck ? NeckEmissionRate : source.mMajorWound ? MajorEmissionRate : NormalEmissionRate;
            mRemainders[i] += dt * amount * rate;
            const unsigned count = static_cast<unsigned>(mRemainders[i]);
            mRemainders[i] -= count;
            mPendingDrops[i] = count;
            pending += count;
            // Slow dripping from a grounded wound grows a local pool. Each rim
            // point is projected independently, preventing stains across corners.
            mPoolTimes[i] += dt;
            if (amount > 0 && count > 0 && mPoolTimes[i] >= .25f)
            {
                mPoolTimes[i] = 0;
                osg::Vec3f point, normal;
                if (mRaycast && mRaycast(source.mPosition, source.mPosition - osg::Vec3f(0, 0, GroundPoolProbe), point, normal)
                    && normal.z() > .65f)
                    stamp(point, normal, std::min(36.f, 6.f + std::sqrt(std::max(0.f, age)) * 10.f) * std::sqrt(amount), true);
            }
        }
        // Share the finite particle budget between all open wounds. A neck or
        // torso split must not permanently starve later sources in the list.
        if (sourceCount) mNextSource %= sourceCount;
        while (pending && mDrops.size() < MaxParticles)
        {
            const auto i = mNextSource;
            mNextSource = (mNextSource + 1) % sourceCount;
            if (!mPendingDrops[i]) continue;
            --mPendingDrops[i]; --pending;
            const auto& source = sources[i];
            const float pulse = .55f + .45f * std::pow(std::max(0.f, std::sin(age * 11.f + i * .7f)), 2.f);
            const float cone = source.mNeck ? .28f : source.mMajorWound ? .65f : .95f;
            const osg::Vec3f jitter(random() - .5f, random() - .5f, random() - .5f);
            const auto direction = unit(source.mDirection + jitter * cone * spreadScale);
            const float speed = source.mNeck ? 240.f + 190.f * pulse
                : source.mMajorWound ? 140.f + 140.f * pulse + random() * 40.f : 80.f + random() * 130.f;
            const float width = (.65f + random() * .6f + (source.mNeck ? .55f : source.mMajorWound ? .35f : 0.f)) * sizeScale;
            mDrops.push_back({ source.mPosition,
                direction * speed * launchScale * std::clamp(source.mPressure, .15f, 1.f), 0, width });
            ++mEmitted;
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
                if (amount > 0 && splats++ < MaxImpactSplats)
                    stamp(point, normal, (4.f + random() * 7.f) * std::sqrt(amount), false);
                drop.mAge = 4;
            }
            else
            {
                drop.mPosition = next;
                drop.mVelocity.z() -= 627.2f * dt;
                drop.mAge += dt;
            }
        }
        mDrops.erase(std::remove_if(mDrops.begin(), mDrops.end(), [](const auto& p) { return p.mAge > MaxDropAge; }), mDrops.end());
        auto* vertices = static_cast<osg::Vec3Array*>(mGeometry->getVertexArray());
        auto* normals = static_cast<osg::Vec3Array*>(mGeometry->getNormalArray());
        auto* colors = static_cast<osg::Vec4Array*>(mGeometry->getColorArray());
        auto* uv = static_cast<osg::Vec2Array*>(mGeometry->getTexCoordArray(0));
        auto* sizes = static_cast<osg::Vec2Array*>(mGeometry->getTexCoordArray(1));
        auto* indices = static_cast<osg::DrawElementsUShort*>(mGeometry->getPrimitiveSet(0));
        if (vertices->empty() && mDrops.empty()) return;
        vertices->clear(); normals->clear(); colors->clear(); uv->clear(); sizes->clear(); indices->clear();
        osg::BoundingBox bounds;
        const osg::Vec3f origin = mDrops.empty() ? osg::Vec3f() : mDrops.front().mPosition;
        const std::array<osg::Vec2f, SpriteVertices> corners{
            osg::Vec2f(0, 0), osg::Vec2f(1, 0), osg::Vec2f(1, 1), osg::Vec2f(0, 1)
        };
        constexpr std::array<unsigned short, SpriteIndices> triangleIndices{ 0, 1, 2, 0, 2, 3 };
        for (const auto& drop : mDrops)
        {
            const auto axis = unit(drop.mVelocity);
            const osg::Vec2f size(drop.mSize, std::max(drop.mSize, std::min(8.f, drop.mVelocity.length() * .009f)));
            const auto base = static_cast<unsigned short>(vertices->size());
            // All four positions are the physical particle centre. The vertex
            // shader expands one textured quad in the current camera's plane,
            // stretching its long axis along the projected flight direction.
            for (const auto& corner : corners)
            {
                vertices->push_back(drop.mPosition - origin);
                normals->push_back(axis);
                colors->push_back({ .26f, .007f, .028f, .94f });
                uv->push_back(corner);
                sizes->push_back(size);
            }
            for (const auto offset : triangleIndices)
                indices->push_back(static_cast<unsigned short>(base + offset));
            // GPU-generated corners are absent from the CPU vertex array.
            // Include every possible billboard orientation in culling/light
            // selection bounds, including when viewed along its travel axis.
            const float extent = size.length();
            bounds.expandBy(drop.mPosition - origin - osg::Vec3f(extent, extent, extent));
            bounds.expandBy(drop.mPosition - origin + osg::Vec3f(extent, extent, extent));
        }
        vertices->dirty(); normals->dirty(); colors->dirty(); uv->dirty(); sizes->dirty(); indices->dirty();
        mGeometry->setInitialBound(bounds);
        mGeometry->dirtyBound();
        mTransform->setMatrix(osg::Matrixd::translate(osg::Vec3d(origin)) * osg::Matrixd::inverse(worldMatrix(*mRoot)));
    }
}
