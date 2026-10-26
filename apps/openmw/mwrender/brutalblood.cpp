#include "brutalblood.hpp"
#include "vismask.hpp"
#include "brutalbloodlightingbase.hpp"
#include "brutaltextures.hpp"
#include <components/ragdoll/blood.hpp>
#include <osg/Texture2D>
#include <osg/PrimitiveSet>
#include <components/sceneutil/depth.hpp>
#include <osg/BlendFunc>
#include <osg/Geode>
#include <osg/Depth>
#include <osg/Program>
#include <osg/Shader>
#include <algorithm>
#include <array>
#include <memory>
#include <tuple>
#include <chrono>
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <cmath>

namespace MWRender
{
    std::shared_ptr<BloodProjectionBudget::Client> BloodProjectionBudget::addClient()
    {
        auto client=std::make_shared<Client>();mClients.push_back(client);return client;
    }
    bool BloodProjectionBudget::take(const std::shared_ptr<Client>& client)
    {
        const auto frame=mFrame?mFrame():mManualFrame;
        if (frame!=mLastFrame)
        {
            mLastFrame=frame;mJobs=0;mSpent=0;
            std::erase_if(mClients,[](const auto& weak){return weak.expired();});
            if (!mClients.empty()) mCursor%=mClients.size();
        }
        if (mJobs>=MaxJobs || mSpent>=MaxMilliseconds || mClients.empty()) return false;
        for (std::size_t i=0;i<mClients.size();++i)
        {
            const auto next=mClients[mCursor].lock();
            if (next && next->mPending)
            {
                if (next!=client) return false;
                mCursor=(mCursor+1)%mClients.size();++mJobs;return true;
            }
            mCursor=(mCursor+1)%mClients.size();
        }
        return false;
    }

    namespace
    {
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
        bool validStamp(const BloodStamp& stamp)
        {
            if (stamp.mTriangles.empty() || stamp.mTriangles.size() % 3 || stamp.mTriangles.size() > 192
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
        std::array<int,3> spatialKey(const osg::Vec3f& point)
        {
            return { int(std::floor(point.x()/64)), int(std::floor(point.y()/64)), int(std::floor(point.z()/64)) };
        }
        auto bloodAxes(const BloodStamp& stamp)
        {
            const osg::Vec3f up = stamp.mNormal.z() >= .7f ? osg::Vec3f(0,1,0) : osg::Vec3f(0,0,1);
            const auto vertical = unit(up - stamp.mNormal * (up * stamp.mNormal), {0,1,0});
            return std::pair(unit(vertical ^ stamp.mNormal, {1,0,0}), vertical);
        }
        bool fullSquare(const BloodStamp& stamp)
        {
            float twiceArea=0;
            const float depth=(stamp.mTriangles.front()-stamp.mCenter)*stamp.mNormal;
            for (const auto& point : stamp.mTriangles)
                if (std::abs((point-stamp.mCenter)*stamp.mNormal-depth)>.005f) return false;
            for (std::size_t i=0;i<stamp.mTriangles.size();i+=3)
                twiceArea+=((stamp.mTriangles[i+1]-stamp.mTriangles[i])^(stamp.mTriangles[i+2]-stamp.mTriangles[i])).length();
            return twiceArea>=stamp.mRadius*stamp.mRadius*7.99f;
        }
        // Intersect along the decal projector axis, then test the actual
        // receiver triangles. Bounding squares alone would suppress visible
        // blood on an adjacent stair or across a transparent DDS hole.
        bool onStamp(const BloodStamp& stamp, const osg::Vec3f& point, osg::Vec3f& surface, float tolerance)
        {
            for (std::size_t i=0; i<stamp.mTriangles.size(); i+=3)
            {
                const auto a=stamp.mTriangles[i], ab=stamp.mTriangles[i+1]-a, ac=stamp.mTriangles[i+2]-a;
                const auto normal=ab^ac;
                const float denom=normal*stamp.mNormal;
                if (std::abs(denom)<.00000001f) continue;
                const float depth=((a-point)*normal)/denom;
                if (std::abs(depth)>tolerance) continue;
                const auto p=point+stamp.mNormal*depth;
                const auto offset=p-a;
                const float d00=ab*ab, d01=ab*ac, d11=ac*ac, d20=offset*ab, d21=offset*ac;
                const float determinant=d00*d11-d01*d01;
                if (determinant<.00000001f) continue;
                const float u=(d11*d20-d01*d21)/determinant, v=(d00*d21-d01*d20)/determinant;
                if (u>=-.0001f && v>=-.0001f && u+v<=1.0001f) {surface=p;return true;}
            }
            return false;
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

    static osg::ref_ptr<osg::StateSet> makeWetGoreState(bool decal, bool sprite = false, bool textured = false)
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
        osg::ref_ptr<osg::Program> program = new osg::Program;
        const std::string vertex =
            "#version 120\nuniform mat4 projectionMatrix; varying vec4 tint; varying vec3 n; varying vec3 p; varying vec2 uv;varying vec2 bloodInfo;"
            "void setupShadowCoords(vec4 viewPos,vec3 viewNormal);"
            "void main(){mat4 proj=projectionMatrix; if(proj[0][0]==0.0)proj=gl_ProjectionMatrix;"
            "vec4 eye=gl_ModelViewMatrix*gl_Vertex;gl_Position=proj*eye;n=gl_NormalMatrix*gl_Normal;"
            "p=eye.xyz;tint=gl_Color;uv=gl_MultiTexCoord0.xy;bloodInfo=gl_MultiTexCoord1.xy;"
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
            "float sunExposure=clamp(max(brutalBloodSunDiffuse.r,max(brutalBloodSunDiffuse.g,brutalBloodSunDiffuse.b))"
            "*bloodDiffuse(N,L)*shadow,0.0,1.0);"
            "spec=brutalBloodSunDiffuse*bloodSpec(N,L,V)*shadow*.15;"
            "for(int i=0;i<8;++i){if(i>=brutalBloodPointCount)break;"
            "vec3 delta=brutalBloodPointPosition[i]-pos;float dist=length(delta);"
            "vec4 att=brutalBloodPointAttenuation[i];"
            "float power=1.0/max(.0001,att.x+att.y*dist+att.z*dist*dist);"
            "if(!brutalBloodClassicFalloff){float fade=clamp(dist/max(.0001,att.w)-1.0,0.0,1.0);"
            "fade=1.0-fade*fade;power*=fade*fade;}"
            "L=delta/max(.0001,dist);light+=(brutalBloodPointAmbient[i]+brutalBloodPointDiffuse[i]*bloodDiffuse(N,L))*power;"
            "spec+=brutalBloodPointDiffuse[i]*bloodSpec(N,L,V)*power;}"
            "light=max(vec3(0.0),light);spec=max(vec3(0.0),spec);"
            // Blood absorbs more of direct daylight than a bright diffuse
            // texture. Apply after the native clamp, preserving room/torch
            // illumination when no sunlight reaches the surface.
            "if(brutalBloodClamp)light=min(vec3(1.0),light);return light*(1.0-.35*sunExposure);}"
            "vec3 bloodFog(vec3 color,vec3 pos){if(brutalBloodFogMode==0)return color;"
            "float dist=brutalBloodFogRadial?length(pos):abs(pos.z);float f=0.0;"
            "if(brutalBloodFogMode==2){float start=gl_Fog.start*.5;"
            "if(gl_Fog.end>start)f=1.0-exp(-2.0*max(0.0,dist-start)/(gl_Fog.end-start));}"
            "else if(gl_Fog.end>gl_Fog.start)f=clamp((dist-gl_Fog.start)/(gl_Fog.end-gl_Fog.start),0.0,1.0);"
            "return mix(color,gl_Fog.color.rgb,clamp(f,0.0,1.0));}";
        const std::string decalFragment =
            "uniform float brutalBloodTime;uniform float brutalBloodLifetime;"
            "uniform sampler2D brutalBloodAtlas;"
            "varying vec4 tint;varying vec3 n;varying vec3 p;varying vec2 uv;varying vec2 bloodInfo;"
            "void main(){vec3 N=normalize(n);"
            "float age=max(0.0,brutalBloodTime-bloodInfo.x);float dry=min(1.0,age/90.0);"
            "vec4 c=tint;"
            "if(bloodInfo.y>.5){vec4 texel=texture2D(brutalBloodAtlas,uv);"
            "vec3 pigment=texel.rgb;"
            "c=vec4(pigment*tint.rgb,texel.a*tint.a);}"
            "c.rgb*=1.0-.5*dry;c.a*=clamp((brutalBloodLifetime-age)/10.0,0.0,1.0);"
            "if(c.a<.003)discard;"
            "vec3 spec;vec3 light=bloodLighting(N,p,spec);"
            "gl_FragColor=vec4(bloodFog(c.rgb*light+spec*.035*c.a,p),c.a);}";
        const std::string solidFragment =
            "uniform bool brutalBloodWorldLighting;"
            "varying vec4 tint;varying vec3 n;varying vec3 p;varying vec2 uv;"
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
        const std::string texturedFragment =
            "uniform sampler2D brutalGoreTexture;uniform sampler2D brutalGoreMask;uniform bool brutalGoreHasMask;uniform bool brutalGoreFlipV;"
            "varying vec4 tint;varying vec3 n;varying vec3 p;varying vec2 uv;"
            "void main(){vec2 tc=uv;if(brutalGoreFlipV)tc.y=1.0-tc.y;"
            "if(brutalGoreHasMask && texture2D(brutalGoreMask,tc).a<.5)discard;"
            "vec4 texel=texture2D(brutalGoreTexture,tc);if(texel.a<.01)discard;"
            "vec3 N=normalize(n);if(!gl_FrontFacing)N=-N;"
            "vec3 spec;vec3 light=bloodLighting(N,p,spec);"
            "gl_FragColor=vec4(bloodFog(texel.rgb*tint.rgb*light+spec*.015,p),1.0);}";
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\n" + lighting + (textured ? texturedFragment : sprite ? spriteFragment : decal ? decalFragment : solidFragment)));
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

    osg::ref_ptr<osg::StateSet> createTexturedGoreState(osg::Texture2D& texture,osg::Texture2D* mask)
    {
        static const auto material=makeWetGoreState(false,false,true);
        osg::ref_ptr<osg::StateSet> state=new osg::StateSet(*material,osg::CopyOp::SHALLOW_COPY);
        constexpr auto on=osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE|osg::StateAttribute::PROTECTED;
        state->setTextureAttributeAndModes(0,&texture,on);
        state->addUniform(new osg::Uniform("brutalGoreTexture",0),on);
        state->addUniform(new osg::Uniform("brutalGoreMask",1),on);
        state->addUniform(new osg::Uniform("brutalGoreHasMask",mask!=nullptr),on);
        if(mask)state->setTextureAttributeAndModes(1,mask,on);
        state->addUniform(new osg::Uniform("brutalGoreFlipV",texture.getImage()
            && texture.getImage()->getOrigin()==osg::Image::TOP_LEFT),on);
        return state;
    }

    static osg::ref_ptr<osg::StateSet> createBloodSpriteState()
    {
        static const auto state = makeWetGoreState(false, true);
        return new osg::StateSet(*state, osg::CopyOp::SHALLOW_COPY);
    }

    osg::ref_ptr<osg::Node> BloodDecals::createPrecompileNode()
    {
        osg::ref_ptr<osg::Group> root=new osg::Group;
        // Compile the shared programs/atlas during loading instead of the
        // first severing frame. This graph is never attached to the world.
        for (unsigned kind=0;kind<4;++kind)
        {
            osg::ref_ptr<osg::Geode> node=new osg::Geode;
            auto state=kind==3 ? createBloodSpriteState() : createWetGoreState(kind==2);
            if (kind==2 && mAtlas)
                state->setTextureAttributeAndModes(0,mAtlas,osg::StateAttribute::ON);
            node->setStateSet(state);
            if (kind) attachBloodLighting(*node,*mParent);
            root->addChild(node);
        }
        if(auto* assets=findBrutalTextures(*mParent))
            for(unsigned kind=BrutalTextures::Limb;kind<BrutalTextures::Count;++kind)
                if(assets->mTextures[kind])
                {
                    osg::ref_ptr<osg::Geode> node=new osg::Geode;
                    node->setStateSet(createTexturedGoreState(*assets->mTextures[kind],assets->mMasks[kind]));
                    attachBloodLighting(*node,*mParent);
                    root->addChild(node);
                }
        return root;
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
                    || image->r() != 1) continue;
                tiles.push_back({ image, group == &floor });
                if (++accepted == 8) break;
            }
        }
        mFloorTiles.clear(); mWallTiles.clear(); mFloorInk.clear(); mWallInk.clear(); mAtlas = nullptr;
        if (!tiles.empty())
        {
            // A fixed tile size bounds GPU/CPU memory independently of uploaded
            // image dimensions. Letterboxing retains the artwork's aspect ratio.
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
                std::vector<osg::Vec2f> ink;
                for (int row=0;row<32;++row) for (int column=0;column<32;++column)
                {
                    const osg::Vec2f uv((column+.5f)/32,(row+.5f)/32);
                    if (atlas->getColor(x+int(uv.x()*(tileSize-1)),y+int(uv.y()*(tileSize-1))).a()>.15f)
                        ink.push_back(uv);
                }
                std::vector<osg::Vec2f> samples;
                for (std::size_t i=0;i<std::min<std::size_t>(24,ink.size());++i)
                    samples.push_back(ink[i*ink.size()/std::min<std::size_t>(24,ink.size())]);
                (tile.mFloor ? mFloorInk : mWallInk).push_back(std::move(samples));
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
        mBatches[stain.mCell].mChunks[stain.mSequence / 128].mDirty = true;
    }

    std::vector<BloodDecals::Stain*> BloodDecals::nearby(const std::string& cell, const BloodStamp& stamp) const
    {
        const auto batch=mBatches.find(cell);
        if (batch==mBatches.end()) return {};
        const auto& buckets=batch->second.mSpatial;
        const float reach=stamp.mRadius+batch->second.mMaxRadius;
        const osg::Vec3f extent(reach,reach,reach);
        const auto lo=spatialKey(stamp.mCenter-extent), hi=spatialKey(stamp.mCenter+extent);
        std::vector<Stain*> result;
        for (int x=lo[0];x<=hi[0];++x) for (int y=lo[1];y<=hi[1];++y) for (int z=lo[2];z<=hi[2];++z)
        {
            const auto bucket=buckets.find({x,y,z});
            if (bucket==buckets.end()) continue;
            for (auto* stain : bucket->second)
            {
                const auto& other=stain->mStamp;
                const float radius=(other.mRadius+stamp.mRadius)*1.5f;
                if (other.mNormal*stamp.mNormal>.95f && (other.mCenter-stamp.mCenter).length2()<=radius*radius)
                    result.push_back(stain);
            }
        }
        return result;
    }
    void BloodDecals::index(Stain& stain)
    {
        std::tie(stain.mHorizontal,stain.mVertical)=bloodAxes(stain.mStamp);
        stain.mPlaneDepth=(stain.mStamp.mTriangles.front()-stain.mStamp.mCenter)*stain.mStamp.mNormal;
        stain.mFullSquare=fullSquare(stain.mStamp);
        auto& batch=mBatches[stain.mCell];
        batch.mSpatial[spatialKey(stain.mStamp.mCenter)].push_back(&stain);
        batch.mMaxRadius=std::max(batch.mMaxRadius,stain.mStamp.mRadius);
    }
    void BloodDecals::unindex(const Stain& stain)
    {
        auto& buckets=mBatches.find(stain.mCell)->second.mSpatial;
        const auto it=buckets.find(spatialKey(stain.mStamp.mCenter));
        if (it==buckets.end()) return;
        std::erase(it->second,&stain);
        if (it->second.empty()) buckets.erase(it);
    }
    bool BloodDecals::redundant(const BloodStamp& stamp, const std::vector<Stain*>& neighbours) const
    {
        if (stamp.mPool || !mAtlas || neighbours.empty()) return false;
        const bool floor=stamp.mNormal.z()>=.7f;
        const auto& tiles=floor ? mFloorTiles : mWallTiles;
        const auto& masks=floor ? mFloorInk : mWallInk;
        if (tiles.empty()) return false;
        const auto& samples=masks[(stamp.mVisualSeed>>2)%tiles.size()];
        const auto [horizontal,vertical]=bloodAxes(stamp);
        const bool square=fullSquare(stamp);
        const float depth=(stamp.mTriangles.front()-stamp.mCenter)*stamp.mNormal;
        unsigned tested=0,covered=0;
        for (auto uv : samples)
        {
            if (stamp.mVisualSeed&1) uv.x()=1-uv.x();
            if (floor && (stamp.mVisualSeed&2)) uv.set(1-uv.x(),1-uv.y());
            const auto probe=stamp.mCenter+(horizontal*(uv.x()-.5f)+vertical*(uv.y()-.5f))*(stamp.mRadius*2);
            osg::Vec3f point=probe+stamp.mNormal*depth;
            if (!square && !onStamp(stamp,probe,point,stamp.mRadius)) continue;
            ++tested;
            unsigned layers=0;
            for (const auto* other : neighbours)
            {
                const auto& old=other->mStamp;
                const auto relative=point-old.mCenter;
                osg::Vec2f at(.5f+(relative*other->mHorizontal)/(old.mRadius*2),.5f+(relative*other->mVertical)/(old.mRadius*2));
                if (old.mNormal.z()>=.7f && (old.mVisualSeed&2)) at.set(1-at.x(),1-at.y());
                if (old.mVisualSeed&1) at.x()=1-at.x();
                if (at.x()<0 || at.x()>1 || at.y()<0 || at.y()>1 || other->mAge>=mLifetime-10) continue;
                if (!old.mPool)
                {
                    const auto& oldTiles=old.mNormal.z()>=.7f ? mFloorTiles : mWallTiles;
                    if (oldTiles.empty()) continue;
                    const auto tile=oldTiles[(old.mVisualSeed>>2)%oldTiles.size()];
                    const auto* image=mAtlas->getImage();
                    const int x=std::clamp(int((tile.x()+at.x()*(tile.z()-tile.x()))*image->s()),0,image->s()-1);
                    const int y=std::clamp(int((tile.y()+at.y()*(tile.w()-tile.y()))*image->t()),0,image->t()-1);
                    if (image->data(x,y)[3]<=38) continue;
                }
                if (other->mFullSquare)
                {
                    if (std::abs(relative*old.mNormal-other->mPlaneDepth)>.3f) continue;
                }
                else {osg::Vec3f p;if (!onStamp(old,point,p,.3f)) continue;}
                layers+=old.mPool ? 2 : 1;
                if (layers>=2) {++covered;break;}
            }
        }
        // Never carve squares out of artwork. Keep the complete new decal
        // unless almost all of its actual opaque ink is already saturated.
        return tested>=4 && covered*10>=tested*9;
    }

    void BloodDecals::add(const std::string& cell, const BloodStamp& stamp)
    {
        if (stamp.mPool || !validStamp(stamp)) return;
        BloodStamp chosen=stamp;
        if (!chosen.mVisualSeed) chosen.mVisualSeed=nextSeed(mRandom);
        const auto neighbours=mAtlas ? nearby(cell,chosen) : std::vector<Stain*>{};
        if (redundant(chosen,neighbours)) return;
        // Bounded memory even with hundreds of simultaneous gore emitters.
        while (!mStains.empty() && (mStains.size() >= MaxStains
            || mVertexCount + stamp.mTriangles.size() > MaxVertices))
        {
            dirty(mStains.front());
            unindex(mStains.front());
            mVertexCount -= mStains.front().mStamp.mTriangles.size();
            mStains.pop_front(); // O(1), including when a fountain fills the cap.
        }
        mStains.push_back({ { cell, std::move(chosen), 0 }, mNextSequence++ });
        mVertexCount += stamp.mTriangles.size();
        index(mStains.back());
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
        for (auto it=mStains.begin();it!=mStains.end();)
            if (it->mAge>=mLifetime)
            {
                dirty(*it); unindex(*it);
                mVertexCount-=it->mStamp.mTriangles.size();
                it=mStains.erase(it);
            }
            else ++it;
        if (mSinceBatch >= .1f)
        {
            rebuild();
            mSinceBatch = 0;
        }
    }

    void BloodDecals::rebuild()
    {
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
            const auto& stamp = stain.mStamp;
            if (!chunk.mTransform)
            {
                chunk.mOrigin = stamp.mCenter;
                chunk.mTransform = new osg::MatrixTransform;
                chunk.mTransform->setNodeMask(Mask_Effect);
                chunk.mTransform->setMatrix(osg::Matrixd::translate(osg::Vec3d(chunk.mOrigin)));
            }
            const bool floor = stamp.mNormal.z() >= .7f;
            const auto& tiles = floor ? mFloorTiles : mWallTiles;
            const bool textured = !stamp.mPool && mAtlas && !tiles.empty();
            const auto seed = stamp.mVisualSeed;
            const auto tile = textured ? tiles[(seed >> 2) % tiles.size()] : osg::Vec4f(0,0,1,1);
            const osg::Vec3f up = floor ? osg::Vec3f(0,1,0) : osg::Vec3f(0,0,1);
            const auto vertical = unit(up - stamp.mNormal * (up * stamp.mNormal), {0,1,0});
            const auto horizontal = unit(vertical ^ stamp.mNormal, {1,0,0});
            auto* vertices = static_cast<osg::Vec3Array*>(chunk.mGeometry->getVertexArray());
            auto* normals = static_cast<osg::Vec3Array*>(chunk.mGeometry->getNormalArray());
            auto* colors = static_cast<osg::Vec4Array*>(chunk.mGeometry->getColorArray());
            auto* coords = static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(0));
            auto* births = static_cast<osg::Vec2Array*>(chunk.mGeometry->getTexCoordArray(1));
            const float shade = .94f + .06f * ((seed >> 8) & 255) / 255.f;
            osg::Vec3f normal = stamp.mNormal;
            for (std::size_t corner = 0; corner < stamp.mTriangles.size(); ++corner)
            {
                const auto& vertex = stamp.mTriangles[corner];
                if (corner % 3 == 0)
                {
                    normal = unit((stamp.mTriangles[corner+1] - vertex) ^ (stamp.mTriangles[corner+2] - vertex), stamp.mNormal);
                    if (normal * stamp.mNormal < 0) normal = -normal;
                }
                const auto relative = vertex - stamp.mCenter;
                float u = std::clamp(.5f + (relative * horizontal) / (stamp.mRadius * 2), 0.f, 1.f);
                float v = std::clamp(.5f + (relative * vertical) / (stamp.mRadius * 2), 0.f, 1.f);
                if (floor && (seed & 2)) { u = 1-u; v = 1-v; }
                if (seed & 1) u = 1-u;
                vertices->push_back(vertex - chunk.mOrigin); normals->push_back(normal);
                colors->push_back(textured ? osg::Vec4f(shade,shade,shade,.94f) : osg::Vec4f(.29f,.009f,.014f,.94f));
                coords->push_back({ tile.x()+u*(tile.z()-tile.x()), tile.y()+v*(tile.w()-tile.y()) });
                births->push_back({ static_cast<float>(mTime-stain.mAge), textured ? 1.f : 0.f });
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
            vertices->dirty(); chunk->mGeometry->getNormalArray()->dirty(); chunk->mGeometry->getColorArray()->dirty();
            chunk->mGeometry->getTexCoordArray(0)->dirty(); chunk->mGeometry->getTexCoordArray(1)->dirty();
            static_cast<osg::DrawArrays*>(chunk->mGeometry->getPrimitiveSet(0))->setCount(vertices->getNumElements());
            chunk->mGeometry->dirtyBound(); chunk->mNode->setNodeMask(active ? unsigned(Mask_Effect) : 0u);
            chunk->mTransform->setNodeMask(active ? unsigned(Mask_Effect) : 0u); chunk->mDirty = false;
        }
        for (auto& [cell, batch] : mBatches)
            for (auto it = batch.mChunks.begin(); it != batch.mChunks.end();)
                if (it->second.mDirty && !it->second.mGeometry->getVertexArray()->getNumElements())
                {
                    if (it->second.mTransform) mParent->removeChild(it->second.mTransform);
                    it = batch.mChunks.erase(it);
                }
                else ++it;
    }
    void BloodDecals::clear()
    {
        for (auto& [cell, batch] : mBatches)
            for (auto& [id, chunk] : batch.mChunks)
                if (chunk.mTransform) mParent->removeChild(chunk.mTransform);
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
                if (chunk.mTransform) mParent->removeChild(chunk.mTransform);
            batch.mChunks.clear();
            batch.mSpatial.clear(); batch.mMaxRadius=0;
        }
        mStains.clear();
        mVertexCount = 0;
        mNextSequence = 0; mTime = 0; mSinceBatch = 1;
        mTimeUniform->set(0.f);
        for (auto stain : state)
        {
            if (stain.mStamp.mPool) continue; // Retire old procedural pools, including saved ones.
            if (!std::isfinite(stain.mAge) || stain.mAge < 0 || stain.mAge >= 1800) continue;
            if (mStains.size() >= MaxStains || !validStamp(stain.mStamp)
                || mVertexCount + stain.mStamp.mTriangles.size() > MaxVertices) continue;
            if (!stain.mStamp.mVisualSeed) stain.mStamp.mVisualSeed=nextSeed(mRandom);
            if (mAtlas && redundant(stain.mStamp,nearby(stain.mCell,stain.mStamp))) continue;
            // Loading is not a new deposit: never merge/rejuvenate saved pools.
            mStains.push_back({ stain, mNextSequence++ });
            mVertexCount += stain.mStamp.mTriangles.size();
            index(mStains.back());
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
            saved.mVisualSeed = stain.mStamp.mVisualSeed;
            saved.mCell = stain.mCell; saved.mAge = stain.mAge; saved.mRadius = stain.mStamp.mRadius; saved.mPool = stain.mStamp.mPool;
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
            stain.mStamp.mVisualSeed = saved.mVisualSeed;
            stain.mCell = saved.mCell; stain.mAge = saved.mAge; stain.mStamp.mRadius = saved.mRadius; stain.mStamp.mPool = saved.mPool;
            stain.mStamp.mCenter = { saved.mCenter[0], saved.mCenter[1], saved.mCenter[2] };
            stain.mStamp.mNormal = { saved.mNormal[0], saved.mNormal[1], saved.mNormal[2] };
            for (const auto& v : saved.mTriangles) stain.mStamp.mTriangles.push_back({ v[0], v[1], v[2] });
            state.push_back(std::move(stain));
        }
        restoreState(state);
    }

    BloodTrail::BloodTrail(osg::Group& root, Raycast raycast, Deposit deposit, Projector projector,
        std::function<bool()> ready, std::shared_ptr<BloodProjectionBudget> budget)
        : mRoot(&root), mTransform(new osg::MatrixTransform)
        , mGeometry(geometry(new osg::Vec3Array, new osg::Vec3Array, new osg::Vec4Array))
        , mRaycast(std::move(raycast)), mProjector(std::move(projector)), mReady(std::move(ready))
        , mDeposit(std::move(deposit))
        , mBudget(std::move(budget)), mBudgetClient(mBudget?mBudget->addClient():nullptr)
    {
        mTransform->setName("Brutal Morrowind ballistic blood");
        mTransform->setNodeMask(Mask_Effect);
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->setStateSet(createBloodSpriteState());
        attachBloodLighting(*geode, root);
        mGeometry->setTexCoordArray(0, new osg::Vec2Array);
        mGeometry->setTexCoordArray(1, new osg::Vec2Array);
        mGeometry->setPrimitiveSet(0, new osg::DrawElementsUShort(GL_TRIANGLES));
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
    namespace
    {
        std::vector<osg::Vec3f> collisionSplat(const BloodTrail::Raycast& ray,
            const osg::Vec3f& position, const osg::Vec3f& normal, float radius)
        {
            std::vector<osg::Vec3f> result;
            if (!ray || !std::isfinite(radius) || radius <= 0 || radius > 256) return result;
            const auto n = unit(normal);
            const osg::Vec3f up = n.z() >= .7f ? osg::Vec3f(0,1,0) : osg::Vec3f(0,0,1);
            const auto vertical = unit(up - n * (up * n), {0,1,0});
            const auto horizontal = unit(vertical ^ n, {1,0,0});
            constexpr std::array<std::array<float,2>,8> corners{{
                {-1,-1},{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0} }};
            std::array<osg::Vec3f,8> rim;
            std::array<bool,8> valid{};
            // The receiver is still the stable collision mesh outdoors. A
            // wider DDS footprint must also reach uneven heights at its rim.
            const float reach = std::max(3.f, radius * 1.5f);
            for (unsigned i=0;i<corners.size();++i)
            {
                const auto point = position + (horizontal * corners[i][0] + vertical * corners[i][1]) * radius;
                osg::Vec3f hit, hn;
                valid[i] = ray(point + n * reach, point - n * reach, hit, hn)
                    && unit(hn) * n > .35f;
                rim[i] = hit + unit(hn, n) * .06f;
            }
            for (unsigned i=0;i<corners.size();++i)
            {
                const auto next = (i+1)%corners.size();
                if (valid[i] && valid[next]) result.insert(result.end(), { position + n * .06f, rim[i], rim[next] });
            }
            return result;
        }
    }

    BloodTrail::Surface BloodTrail::collisionSurface(const std::vector<osg::Vec3f>& triangles)
    {
        const auto ray = collisionRaycast(triangles);
        if (!ray) return {};
        return { ray, [ray](const auto& p, const auto& n, float r) { return collisionSplat(ray,p,n,r); } };
    }

    void BloodTrail::stamp(const osg::Vec3f& position, const osg::Vec3f& normal, float radius)
    {
        if (!mDeposit || !mRaycast) return;
        BloodStamp stamp;
        stamp.mCenter=position;stamp.mNormal=unit(normal);
        stamp.mVisualSeed=nextSeed(mRandom);
        stamp.mRadius=radius*3.f*(.9f+random()*.2f);
        stamp.mTriangles=mProjector?mProjector(position,stamp.mNormal,stamp.mRadius)
            :collisionSplat(mRaycast,position,stamp.mNormal,stamp.mRadius);
        if (validStamp(stamp)) mDeposit(stamp);
    }
    void BloodTrail::update(float duration, const std::vector<Source>& sources, float amount, float age)
    {
        if (!std::isfinite(duration) || duration <= 0) return;
        const float dt = std::min(duration, .1f);
        const float emission = Ragdoll::bloodEmissionScale(amount);
        const float launchScale = Ragdoll::bloodLaunchScale(amount);
        const float spreadScale = Ragdoll::bloodSpreadScale(amount);
        const float sizeScale = std::sqrt(emission);
        const auto sourceCount = std::min(sources.size(), MaxSources);
        mRemainders.resize(sourceCount,0);
        mPendingDrops.assign(sourceCount, 0);
        unsigned pending = 0;
        for (std::size_t i=0;i<sourceCount;++i)
        {
            const auto& source = sources[i];
            if (!std::isfinite(source.mPosition.length2()) || !std::isfinite(source.mDirection.length2())
                || !std::isfinite(source.mPressure)) continue;
            const float rate = source.mNeck ? 60.f : source.mMajorWound ? 35.f : 14.f;
            mRemainders[i] += dt * emission * rate;
            const auto count = static_cast<unsigned>(mRemainders[i]);
            mRemainders[i] -= count; mPendingDrops[i] = count; pending += count;
        }
        if (sourceCount) mNextSource %= sourceCount;
        while (pending && mDrops.size() < MaxParticles)
        {
            const auto i=mNextSource; mNextSource=(mNextSource+1)%sourceCount;
            if (!mPendingDrops[i]) continue;
            --mPendingDrops[i]; --pending;
            const auto& source=sources[i];
            const float pulse=.55f+.45f*std::pow(std::max(0.f,std::sin(age*11.f+i*.7f)),2.f);
            const float cone=source.mNeck ? .28f : source.mMajorWound ? .65f : .95f;
            const osg::Vec3f jitter(random()-.5f,random()-.5f,random()-.5f);
            const auto direction=unit(source.mDirection+jitter*cone*spreadScale);
            const float speed=source.mNeck ? 240.f+190.f*pulse
                : source.mMajorWound ? 140.f+140.f*pulse+random()*40.f : 80.f+random()*130.f;
            const float width=(.65f+random()*.6f+(source.mNeck ? .55f : source.mMajorWound ? .35f : 0.f))*sizeScale;
            mDrops.emplace_back(source.mPosition,direction*speed*launchScale*std::clamp(source.mPressure,.15f,1.f),width);
            ++mEmitted;
        }
        if (emission<=0) {mDrops.clear();mImpacts.clear();}
        for (auto& impact:mImpacts) impact.mAge+=duration;
        std::erase_if(mImpacts,[](const auto& impact){return impact.mAge>MaxPendingAge;});
        unsigned splats=0;
        const bool ready=(!mDrops.empty() || !mImpacts.empty()) && (!mReady || mReady());
        for (auto& drop:mDrops)
        {
            drop.mPosition+=drop.mVelocity*dt+osg::Vec3f(0,0,-313.6f*dt*dt);
            drop.mVelocity.z()-=627.2f*dt;
            drop.mAge+=dt;
            drop.mUntracedTime+=dt;
            if (!ready || drop.mAge>MaxDropAge) continue;
            // One sweep per live drop, just as on the ready path. Longer
            // deferred flights catch up over subsequent updates in ballistic
            // steps no larger than the existing 100 ms simulation step.
            const float traceDt=std::min(drop.mUntracedTime,.1f);
            const auto next=drop.mTracePosition+drop.mTraceVelocity*traceDt+osg::Vec3f(0,0,-313.6f*traceDt*traceDt);
            osg::Vec3f point,normal;
            if (mRaycast && mRaycast(drop.mTracePosition,next,point,normal))
            {
                if (emission>0 && mDeposit && splats++<MaxImpactSplats)
                {
                    const float radius=(1.f+random()*3.2f)*std::sqrt(emission);
                    // Combine nearby pending impacts before any expensive
                    // surface projection. Other positions remain independent.
                    auto same=std::find_if(mImpacts.begin(),mImpacts.end(),[&](const auto& impact) {
                        return (impact.mPosition-point).length2()<9.f && impact.mNormal*normal>.98f;
                    });
                    if (same!=mImpacts.end()) same->mRadius=std::max(same->mRadius,radius);
                    else
                    {
                        if (mImpacts.size()>=MaxQueuedImpacts) mImpacts.pop_front();
                        mImpacts.push_back({point,normal,radius,0});
                    }
                }
                drop.mAge=MaxDropAge+1;
            }
            else
            {
                drop.mTracePosition=next;
                drop.mTraceVelocity.z()-=627.2f*traceDt;
                drop.mUntracedTime=std::max(0.f,drop.mUntracedTime-traceDt);
            }
        }
        std::erase_if(mDrops,[](const auto& drop){return drop.mAge>MaxDropAge;});
        if (mBudgetClient) mBudgetClient->mPending=ready && !mImpacts.empty();
        for (unsigned i=0;ready && i<MaxProjectionsPerUpdate && !mImpacts.empty();++i)
        {
            if (mBudget && !mBudget->take(mBudgetClient)) break;
            const auto impact=mImpacts.front();mImpacts.pop_front();
            const auto start=std::chrono::steady_clock::now();
            stamp(impact.mPosition,impact.mNormal,impact.mRadius);
            if (mBudget) mBudget->complete(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            if (mBudgetClient) mBudgetClient->mPending=!mImpacts.empty();
        }
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
