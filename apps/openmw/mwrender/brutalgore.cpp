#include "brutalgore.hpp"
#include "brutalblood.hpp"
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <osg/Geode>
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
                    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                    geode->setStateSet(createWetGoreState());
                    geode->addDrawable(makeGeometry(cap));
                    result[side]->addChild(geode);
                }
            }
        };
    }

    std::array<osg::ref_ptr<osg::Group>, 2> splitGoreMesh(osg::Node& part,
        const osg::Vec3f& planePoint, const osg::Vec3f& planeNormal)
    {
        Slice visitor;
        visitor.point = planePoint; visitor.normal = unit(planeNormal);
        const auto paths = part.getParentalNodePaths();
        if (!paths.empty()) { visitor.prefix = paths.front(); visitor.prefix.pop_back(); }
        part.accept(visitor);
        return visitor.result;
    }

    osg::ref_ptr<osg::Geometry> makeIntestineTube(const std::vector<osg::Vec3f>& centers, float radius)
    {
        std::vector<Vertex> vertices;
        if (centers.size() < 2) return makeGeometry(vertices);
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
        curve.push_back(centers.back());
        std::vector<std::array<Vertex, 9>> rings;
        osg::Vec3f side(1, 0, 0);
        float length = 0;
        for (std::size_t i = 0; i < curve.size(); ++i)
        {
            const auto tangent = unit(curve[std::min(i + 1, curve.size() - 1)] - curve[i ? i - 1 : i]);
            side = unit(side - tangent * (side * tangent), unit(tangent ^ osg::Vec3f(0, 1, 0), { 1, 0, 0 }));
            const auto up = unit(tangent ^ side);
            if (i) length += (curve[i] - curve[i - 1]).length();
            std::array<Vertex, 9> ring;
            for (unsigned j = 0; j < ring.size(); ++j)
            {
                const float angle = j * 6.2831853f / 8.f;
                const auto n = side * std::cos(angle) + up * std::sin(angle);
                const float r = radius * (1.f + .055f * std::sin(length * 2.8f));
                ring[j].p = curve[i] + n * r; ring[j].n = n;
                ring[j].color = { .62f, .24f, .27f, 1 };
                ring[j].uv[0] = { length * .025f, j / 8.f, 0, 0 };
            }
            rings.push_back(ring);
        }
        for (std::size_t i = 0; i + 1 < rings.size(); ++i)
            for (unsigned j = 0; j < 8; ++j)
                vertices.insert(vertices.end(), { rings[i][j], rings[i + 1][j], rings[i + 1][j + 1],
                    rings[i][j], rings[i + 1][j + 1], rings[i][j + 1] });
        return makeGeometry(vertices);
    }
}
