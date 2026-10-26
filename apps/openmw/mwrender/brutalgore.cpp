#include "brutalgore.hpp"
#include "brutalblood.hpp"
#include "brutaltextures.hpp"
#include "brutalbloodlightingbase.hpp"
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <osg/Geode>
#include <osg/Shape>
#include <osg/TriangleIndexFunctor>
#include <algorithm>
#include <cmath>

namespace MWRender
{
    namespace
    {
        osg::Vec3f unit(osg::Vec3f v, osg::Vec3f fallback = { 0, 0, 1 })
        { return v.normalize() > .0001f ? v : fallback; }
        struct Vertex
        {
            osg::Vec3f p, n;
            osg::Vec4f color{ 1, 1, 1, 1 };
            std::array<osg::Vec4f, 8> uv{};
            Vertex mix(const Vertex& v, float t) const
            {
                Vertex out;
                out.p = p + (v.p - p) * t; out.n = n + (v.n - n) * t;
                out.color = color + (v.color - color) * t;
                for (unsigned i = 0; i < uv.size(); ++i) out.uv[i] = uv[i] + (v.uv[i] - uv[i]) * t;
                return out;
            }
        };
        osg::Vec4f value(const osg::Array* array, unsigned i, osg::Vec4f fallback = {})
        {
            if (!array || !array->getNumElements()) return fallback;
            if (i >= array->getNumElements()) i = 0;
            if (auto* a = dynamic_cast<const osg::Vec2Array*>(array)) return { (*a)[i].x(), (*a)[i].y(), 0, 0 };
            if (auto* a = dynamic_cast<const osg::Vec3Array*>(array)) return { (*a)[i].x(), (*a)[i].y(), (*a)[i].z(), 0 };
            if (auto* a = dynamic_cast<const osg::Vec4Array*>(array)) return (*a)[i];
            return fallback;
        }
        osg::ref_ptr<osg::Geometry> makeGeometry(const std::vector<Vertex>& vertices,
            const osg::Geometry* original = nullptr)
        {
            osg::ref_ptr<osg::Geometry> g = new osg::Geometry;
            if (original) g->setName(original->getName());
            osg::ref_ptr<osg::Vec3Array> p = new osg::Vec3Array, n = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            for (const auto& v : vertices) { p->push_back(v.p); n->push_back(unit(v.n)); colors->push_back(v.color); }
            g->setVertexArray(p); g->setNormalArray(n, osg::Array::BIND_PER_VERTEX); g->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            for (unsigned channel = 0; channel < 8; ++channel)
            {
                const auto* source = original ? original->getTexCoordArray(channel) : nullptr;
                if (!original || dynamic_cast<const osg::Vec2Array*>(source))
                {
                    osg::ref_ptr<osg::Vec2Array> a = new osg::Vec2Array;
                    for (const auto& v : vertices) a->push_back({ v.uv[channel].x(), v.uv[channel].y() });
                    g->setTexCoordArray(channel, a);
                    if (!original) break;
                }
                else if (dynamic_cast<const osg::Vec3Array*>(source))
                {
                    osg::ref_ptr<osg::Vec3Array> a = new osg::Vec3Array;
                    for (const auto& v : vertices) a->push_back({ v.uv[channel].x(), v.uv[channel].y(), v.uv[channel].z() });
                    g->setTexCoordArray(channel, a);
                }
                else if (dynamic_cast<const osg::Vec4Array*>(source))
                {
                    osg::ref_ptr<osg::Vec4Array> a = new osg::Vec4Array;
                    for (const auto& v : vertices) a->push_back(v.uv[channel]);
                    g->setTexCoordArray(channel, a);
                }
            }
            g->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, p->size()));
            g->setUseDisplayList(false); g->setUseVertexBufferObjects(true);
            return g;
        }

        struct Triangles
        {
            std::vector<std::array<unsigned, 3>> indices;
            void operator()(unsigned a, unsigned b, unsigned c) { indices.push_back({ a, b, c }); }
        };

        class Slice : public osg::NodeVisitor
        {
        public:
            osg::Vec3f point, normal;
            osg::NodePath prefix;
            const GoreSliceMapping* mapping=nullptr;
            std::array<osg::ref_ptr<osg::Group>, 2> result{ new osg::Group, new osg::Group };
            Slice() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) { setNodeMaskOverride(~0u); }
            void apply(osg::Drawable& drawable) override
            {
                auto path = prefix;
                path.insert(path.end(), getNodePath().begin(), getNodePath().end());
                osg::ref_ptr<osg::Geometry> posed;
                if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable)) posed = rig->snapshotPose(path);
                else if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                {
                    if (!morph->getSourceGeometry()) return;
                    posed = new osg::Geometry(*morph->getSourceGeometry(), osg::CopyOp::DEEP_COPY_ARRAYS);
                    auto* p = dynamic_cast<osg::Vec3Array*>(posed->getVertexArray());
                    const auto& targets = morph->getMorphTargetList();
                    if (p && !targets.empty() && targets.front().getOffsets()->size() == p->size())
                    {
                        for (unsigned i = 0; i < p->size(); ++i) (*p)[i] = (*targets.front().getOffsets())[i];
                        for (unsigned j = 1; j < targets.size(); ++j)
                            if (targets[j].getOffsets()->size() == p->size())
                                for (unsigned i = 0; i < p->size(); ++i) (*p)[i] += (*targets[j].getOffsets())[i] * targets[j].getWeight();
                    }
                }
                else if (auto* g = drawable.asGeometry()) posed = g;
                if (!posed) return;
                const auto* positions = dynamic_cast<const osg::Vec3Array*>(posed->getVertexArray());
                if (!positions) return;
                const auto world = osg::Matrixf(osg::computeLocalToWorld(path));
                osg::Matrixf normalMatrix;
                normalMatrix.transpose(osg::Matrixf::inverse(world));
                const auto read = [&](unsigned i) {
                    Vertex v;
                    v.p = (*positions)[i] * world;
                    const auto n = value(posed->getNormalArray(), i, { 0, 0, 1, 0 });
                    v.n = unit(osg::Matrixf::transform3x3(osg::Vec3f(n.x(), n.y(), n.z()), normalMatrix));
                    v.color = value(posed->getColorArray(), i, { 1, 1, 1, 1 });
                    for (unsigned j = 0; j < 8; ++j) v.uv[j] = value(posed->getTexCoordArray(j), i);
                    return v;
                };
                osg::TriangleIndexFunctor<Triangles> triangles;
                posed->accept(triangles);
                std::array<std::vector<Vertex>, 2> halves;
                std::vector<osg::Vec3f> boundary;
                for (const auto& tri : triangles.indices)
                {
                    if (tri[0] >= positions->size() || tri[1] >= positions->size() || tri[2] >= positions->size()) continue;
                    const std::array<Vertex, 3> input{ read(tri[0]), read(tri[1]), read(tri[2]) };
                    for (int side = 0; side < 2; ++side)
                    {
                        const float sign = side ? 1.f : -1.f;
                        std::vector<Vertex> polygon;
                        for (unsigned i = 0; i < 3; ++i)
                        {
                            const auto& a = input[i]; const auto& b = input[(i + 1) % 3];
                            const float da = (a.p - point) * normal * sign, db = (b.p - point) * normal * sign;
                            if (da >= 0) polygon.push_back(a);
                            if ((da > 0 && db < 0) || (da < 0 && db > 0))
                            {
                                const auto edge = a.mix(b, da / (da - db));
                                polygon.push_back(edge);
                                if (!side) boundary.push_back(edge.p);
                            }
                        }
                        for (unsigned i = 1; i + 1 < polygon.size(); ++i)
                            halves[side].insert(halves[side].end(), { polygon[0], polygon[i], polygon[i + 1] });
                    }
                }
                osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
                for (auto* node : path) if (node->getStateSet()) state->merge(*node->getStateSet());
                if (posed->getStateSet()) state->merge(*posed->getStateSet());
                for (int side = 0; side < 2; ++side)
                {
                    if (halves[side].empty()) continue;
                    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                    auto mesh = makeGeometry(halves[side], posed);
                    mesh->setStateSet(state);
                    geode->addDrawable(mesh);
                    result[side]->addChild(geode);
                }
                if (boundary.size() < 3) return;
                osg::Vec3f center;
                for (const auto& p : boundary) center += p;
                center /= static_cast<float>(boundary.size());
                const auto u = unit(normal ^ osg::Vec3f(0, 0, 1), { 0, 1, 0 }), v = unit(normal ^ u);
                std::sort(boundary.begin(), boundary.end(), [&](const auto& a, const auto& b) {
                    return std::atan2((a - center) * v, (a - center) * u) < std::atan2((b - center) * v, (b - center) * u);
                });
                boundary.erase(std::unique(boundary.begin(), boundary.end(), [](const auto& a, const auto& b) {
                    return (a - b).length2() < .0001f;
                }), boundary.end());
                const auto mapped=mapping && mapping->mTextures && mapping->mTextures->mTextures[BrutalTextures::Slice]
                    && !mapping->mTextures->mSliceRows[0].empty() && !mapping->mTextures->mSliceRows[1].empty();
                for (int side = 0; side < 2; ++side)
                {
                    std::vector<Vertex> cap;
                    const auto n = normal * (side ? -1.f : 1.f);
                    const auto flesh = [&](const osg::Vec3f& p) { Vertex q; q.p = p; q.n = n; q.color = { .42f, .07f, .09f, 1 }; q.uv[0] = { (p - center) * u * .08f, (p - center) * v * .08f, 0, 0 }; return q; };
                    for (unsigned i = 0; i < boundary.size(); ++i)
                    {
                        const auto next = (i + 1) % boundary.size();
                        cap.insert(cap.end(), { flesh(center), flesh(boundary[side ? next : i]), flesh(boundary[side ? i : next]) });
                    }
                    if(mapped)
                    {
                        // Clip the real cross-section into short vertical
                        // strips. Each strip fits the corresponding atlas row,
                        // so UVs follow the silhouette instead of exposing its
                        // opaque rectangular background or stretching one fan.
                        cap.clear();
                        const auto height=[&](const auto& p) {return (p-mapping->mOrigin)*mapping->mUp;};
                        const auto depth=[&](const auto& p) {return (p-mapping->mOrigin)*mapping->mForward;};
                        float low=height(boundary.front()),high=low;
                        for(const auto& p:boundary){low=std::min(low,height(p));high=std::max(high,height(p));}
                        // Thin decorative plates can intersect in a line.
                        // They have no exposed area to cap or normalize.
                        if(high-low<.001f)continue;
                        const int strips=std::clamp(int(std::ceil((high-low)/std::max(.1f,mapping->mHeight)*64)),1,48);
                        const auto spanAt=[&](float h) {
                            float a=1e20f,b=-1e20f;
                            for(unsigned i=0;i<boundary.size();++i)
                            {
                                const auto& p=boundary[i];const auto& q=boundary[(i+1)%boundary.size()];
                                const float ph=height(p),qh=height(q);
                                if(std::abs(ph-h)<.0001f){a=std::min(a,depth(p));b=std::max(b,depth(p));}
                                if((ph<h && qh>h)||(qh<h && ph>h))
                                {
                                    const float d=depth(p)+(depth(q)-depth(p))*(h-ph)/(qh-ph);
                                    a=std::min(a,d);b=std::max(b,d);
                                }
                            }
                            return osg::Vec2f(a,b);
                        };
                        const auto fleshMapped=[&](const osg::Vec3f& p) {
                            Vertex out;out.p=p;out.n=n;
                            const float h=height(p),vv=std::clamp(h/std::max(.1f,mapping->mHeight),.005f,.995f);
                            const auto span=spanAt(std::clamp(h,low+.0001f,high-.0001f));
                            const float front=std::clamp((depth(p)-span.x())/std::max(.0001f,span.y()-span.x()),0.f,1.f);
                            const auto& rows=mapping->mTextures->mSliceRows[side];
                            const float row=vv*(rows.size()-1);const auto index=std::min(std::size_t(row),rows.size()-1);
                            const auto limits=rows[index]+(rows[std::min(index+1,rows.size()-1)]-rows[index])*(row-index);
                            // Left atlas: face points left, back/spine right.
                            // Right atlas: face points right, back/spine left.
                            const float uu=limits.x()+(limits.y()-limits.x())*(side?front:1-front);
                            out.uv[0]={uu,vv,0,0};return out;
                        };
                        for(int strip=0;strip<strips;++strip)
                        {
                            const float from=low+(high-low)*strip/strips,to=low+(high-low)*(strip+1)/strips;
                            auto polygon=boundary;
                            for(unsigned edge=0;edge<2;++edge)
                            {
                                std::vector<osg::Vec3f> clipped;
                                const float plane=edge?to:from,sign=edge?-1.f:1.f;
                                for(unsigned i=0;i<polygon.size();++i)
                                {
                                    const auto& a=polygon[i];const auto& b=polygon[(i+1)%polygon.size()];
                                    const float da=(height(a)-plane)*sign,db=(height(b)-plane)*sign;
                                    if(da>=0)clipped.push_back(a);
                                    if((da>0&&db<0)||(da<0&&db>0))clipped.push_back(a+(b-a)*(da/(da-db)));
                                }
                                polygon=std::move(clipped);
                            }
                            for(unsigned i=1;i+1<polygon.size();++i)
                            {
                                auto a=fleshMapped(polygon[0]),b=fleshMapped(polygon[i]),c=fleshMapped(polygon[i+1]);
                                if(((b.p-a.p)^(c.p-a.p))*n<0)std::swap(b,c);
                                cap.insert(cap.end(),{a,b,c});
                            }
                        }
                    }
                    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                    geode->setName(mapped?"Brutal Morrowind textured longitudinal cut":"Brutal Morrowind exposed tissue");
                    geode->setUserValue("RealCombatPreserveMaterial",true);
                    geode->setStateSet(mapped?createTexturedGoreState(*mapping->mTextures->mTextures[BrutalTextures::Slice],
                        mapping->mTextures->mMasks[BrutalTextures::Slice]):createWetGoreState());
                    if(mapped && mapping->mLightingRoot)attachBloodLighting(*geode,*mapping->mLightingRoot);
                    geode->addDrawable(makeGeometry(cap));
                    result[side]->addChild(geode);
                }
            }
        };
    }

    std::array<osg::ref_ptr<osg::Group>, 2> splitGoreMesh(osg::Node& part,
        const osg::Vec3f& planePoint, const osg::Vec3f& planeNormal, const GoreSliceMapping* mapping)
    {
        Slice visitor;
        visitor.point = planePoint; visitor.normal = unit(planeNormal);
        visitor.mapping=mapping;
        const auto paths = part.getParentalNodePaths();
        if (!paths.empty()) { visitor.prefix = paths.front(); visitor.prefix.pop_back(); }
        part.accept(visitor);
        return visitor.result;
    }

    osg::ref_ptr<osg::Geometry> makeWoundGeometry(const osg::Shape& shape,bool torso,const BrutalTextures* textures)
    {
        std::vector<Vertex> vertices;
        const auto vertex=[](const osg::Vec3f& p,const osg::Vec3f& n,const osg::Vec2f& uv) {
            Vertex out;out.p=p;out.n=n;out.uv[0]={uv.x(),uv.y(),0,0};return out;
        };
        if(const auto* cylinder=dynamic_cast<const osg::Cylinder*>(&shape))
        {
            constexpr unsigned count=64;
            const auto transform=osg::Matrixf::rotate(cylinder->getRotation())*osg::Matrixf::translate(cylinder->getCenter());
            const auto point=[&](float angle,float z) {return osg::Vec3f(std::cos(angle)*cylinder->getRadius(),
                std::sin(angle)*cylinder->getRadius()*(torso?.7f:1.f),z)*transform;};
            const auto uv=[&](unsigned i) {return !torso && textures?textures->mLimbRim[i%count]
                :osg::Vec2f(.5f+std::cos(i*6.283185307f/count)*(torso?.5f:.47f),.5f+std::sin(i*6.283185307f/count)*(torso?.5f:.47f));};
            for(unsigned i=0;i<count;++i)
            {
                const float a=i*6.283185307f/count,b=(i+1)*6.283185307f/count;
                for(int side:{-1,1})
                {
                    const float z=cylinder->getHeight()*.5f*side;
                    const auto normal=osg::Matrixf::transform3x3(osg::Vec3f(0,0,float(side)),transform);
                    auto center=vertex(osg::Vec3f(0,0,z)*transform,normal,{.5f,.5f});
                    auto p=vertex(point(a,z),normal,uv(i)),q=vertex(point(b,z),normal,uv(i+1));
                    if(side<0)std::swap(p,q);
                    vertices.insert(vertices.end(),{center,p,q});
                }
                const auto normal=unit(osg::Matrixf::transform3x3(osg::Vec3f(std::cos(a),std::sin(a),0),transform));
                const auto p=vertex(point(a,-cylinder->getHeight()*.5f),normal,uv(i));
                const auto q=vertex(point(b,-cylinder->getHeight()*.5f),normal,uv(i+1));
                const auto r=vertex(point(b,cylinder->getHeight()*.5f),normal,uv(i+1));
                const auto s=vertex(point(a,cylinder->getHeight()*.5f),normal,uv(i));
                vertices.insert(vertices.end(),{p,q,r,p,r,s});
            }
        }
        else if(const auto* box=dynamic_cast<const osg::Box*>(&shape))
        {
            const auto extent=box->getHalfLengths();
            const auto transform=osg::Matrixf::rotate(box->getRotation())*osg::Matrixf::translate(box->getCenter());
            for(unsigned axis=0;axis<3;++axis)for(int sign:{-1,1})
            {
                std::array<Vertex,4> corners;
                for(unsigned i=0;i<4;++i)
                {
                    const float u=(i==1||i==2)?1.f:-1.f,v=i>=2?1.f:-1.f;
                    osg::Vec3f p,n;p[axis]=extent[axis]*sign;n[axis]=float(sign);
                    p[(axis+1)%3]=extent[(axis+1)%3]*u;p[(axis+2)%3]=extent[(axis+2)%3]*v;
                    corners[i]=vertex(p*transform,osg::Matrixf::transform3x3(n,transform),{.5f+u*.28f,.5f+v*.28f});
                }
                vertices.insert(vertices.end(),{corners[0],corners[1],corners[2],corners[0],corners[2],corners[3]});
            }
        }
        auto mesh=makeGeometry(vertices);mesh->setName(torso?"Brutal Morrowind waist cut":"Brutal Morrowind limb cut");
        return mesh;
    }

    osg::ref_ptr<osg::Geometry> makeIntestineTube(const std::vector<osg::Vec3f>& centers, float radius)
    {
        osg::ref_ptr<osg::Geometry> mesh = new osg::Geometry;
        mesh->setUseDisplayList(false); mesh->setUseVertexBufferObjects(true);
        mesh->setDataVariance(osg::Object::DYNAMIC);
        mesh->setVertexArray(new osg::Vec3Array);
        mesh->setNormalArray(new osg::Vec3Array, osg::Array::BIND_PER_VERTEX);
        osg::ref_ptr<osg::Vec4Array> color = new osg::Vec4Array;
        color->push_back({ .62f, .24f, .27f, 1 });
        mesh->setColorArray(color, osg::Array::BIND_OVERALL);
        mesh->setTexCoordArray(0, new osg::Vec2Array);
        mesh->addPrimitiveSet(new osg::DrawElementsUInt(GL_TRIANGLES));
        updateIntestineTube(*mesh, centers, radius);
        return mesh;
    }

    void updateIntestineTube(osg::Geometry& mesh, const std::vector<osg::Vec3f>& centers, float radius)
    {
        auto* positions = static_cast<osg::Vec3Array*>(mesh.getVertexArray());
        auto* normals = static_cast<osg::Vec3Array*>(mesh.getNormalArray());
        auto* uv = static_cast<osg::Vec2Array*>(mesh.getTexCoordArray(0));
        auto* indices = static_cast<osg::DrawElementsUInt*>(mesh.getPrimitiveSet(0));
        std::vector<osg::Vec3f> curve;
        // Catmull-Rom centreline rounds the capsule joints instead of drawing a
        // necklace of cylinders. A transported frame avoids twisting at bends.
        for (std::size_t i = 0; i + 1 < centers.size(); ++i)
            for (int j = 0; j < 3; ++j)
            {
                const float t = j / 3.f, t2 = t * t, t3 = t2 * t;
                const auto& a = centers[i ? i - 1 : i]; const auto& b = centers[i];
                const auto& c = centers[i + 1]; const auto& d = centers[std::min(i + 2, centers.size() - 1)];
                curve.push_back((b * 2.f + (c - a) * t + (a * 2.f - b * 5.f + c * 4.f - d) * t2
                    + (-a + b * 3.f - c * 3.f + d) * t3) * .5f);
            }
        if (!centers.empty()) curve.push_back(centers.back());
        const bool topologyChanged = positions->size() != curve.size() * 9;
        positions->clear(); normals->clear(); uv->clear();
        osg::Vec3f side(1, 0, 0);
        float length = 0;
        for (std::size_t i = 0; i < curve.size(); ++i)
        {
            const auto tangent = unit(curve[std::min(i + 1, curve.size() - 1)] - curve[i ? i - 1 : i]);
            side = unit(side - tangent * (side * tangent), unit(tangent ^ osg::Vec3f(0, 1, 0), { 1, 0, 0 }));
            const auto up = unit(tangent ^ side);
            if (i) length += (curve[i] - curve[i - 1]).length();
            for (unsigned j = 0; j < 9; ++j)
            {
                const float angle = j * 6.2831853f / 8.f;
                const auto n = side * std::cos(angle) + up * std::sin(angle);
                const float r = radius * (1.f + .055f * std::sin(length * 2.8f));
                positions->push_back(curve[i] + n * r); normals->push_back(n);
                uv->push_back({ length * .025f, j / 8.f });
            }
        }
        if (topologyChanged)
        {
            indices->clear();
            for (unsigned i = 0; i + 1 < curve.size(); ++i)
                for (unsigned j = 0; j < 8; ++j)
                {
                    const unsigned quad[] = { i * 9 + j, (i + 1) * 9 + j, (i + 1) * 9 + j + 1,
                        i * 9 + j, (i + 1) * 9 + j + 1, i * 9 + j + 1 };
                    indices->insert(indices->end(), std::begin(quad), std::end(quad));
                }
            indices->dirty();
        }
        positions->dirty(); normals->dirty(); uv->dirty();
        mesh.dirtyBound();
    }
}
