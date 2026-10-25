#include "bloodsurfacevisitor.hpp"
#include "vismask.hpp"

#include <osg/Geometry>
#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/LOD>
#include <osg/PagedLOD>
#include <osg/Matrix>
#include <osg/Polytope>
#include <osg/TriangleIndexFunctor>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/PolytopeIntersector>

#include <algorithm>
#include <array>
#include <cmath>

namespace MWRender
{
    namespace
    {
        constexpr std::size_t MaxNodes = 16384;
        constexpr std::size_t MaxScannedTriangles = 131072;
        constexpr std::size_t MaxStoredTriangles = 32768;
        constexpr std::size_t MaxTrianglesPerMesh = 32768;
        struct SnapshotFull {};
        struct MeshScanFull {};

        float distanceToBounds(const osg::BoundingBoxf& bounds, const osg::Vec3f& point)
        {
            float distance = 0;
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                const float closest = std::clamp(point[axis], bounds._min[axis], bounds._max[axis]);
                distance += (point[axis] - closest) * (point[axis] - closest);
            }
            return distance;
        }

        class Collector;
        struct Triangles
        {
            Collector* mCollector = nullptr;
            const osg::Vec3Array* mPositions = nullptr;
            const osg::Vec3dArray* mDoublePositions = nullptr;
            osg::Matrixf mWorld;
            void operator()(unsigned a, unsigned b, unsigned c);
        };

        class Collector final : public osgUtil::IntersectionVisitor
        {
        public:
            Collector(const osg::Vec3f& center, float radius,
                const std::function<bool(const osg::Node&)>& filter)
                : mCenter(center), mRadius(radius), mFilter(filter)
            {
                osg::Polytope bounds;
                bounds.setToBoundingBox(osg::BoundingBox(center - osg::Vec3f(radius, radius, radius),
                    center + osg::Vec3f(radius, radius, radius)));
                setIntersector(new osgUtil::PolytopeIntersector(bounds));
                // Effect-masked NIF children can be real visible carpets. They
                // are distinguished below from standalone particle/effect roots.
                setTraversalMask(~(Mask_UpdateVisitor | Mask_Actor | Mask_Player | Mask_FirstPerson
                    | Mask_Sky | Mask_Water | Mask_SimpleWater | Mask_Debug | Mask_RenderToTexture
                    | Mask_PreCompile | Mask_ParticleSystem | Mask_Groundcover));
                setReferenceEyePoint(center);
                setReferenceEyePointCoordinateFrame(osgUtil::Intersector::MODEL);
                setLODSelectionMode(USE_HIGHEST_LEVEL_OF_DETAIL);
                setUserValue("brutalBloodReceiverRadius", radius);
                mRetained.reserve(4096);
            }

            bool allowed(const osg::Node& node)
            {
                if (++mNodes > MaxNodes) throw SnapshotFull{};
                if (mFilter && !mFilter(node)) return false;
                const auto mask = node.getNodeMask();
                if (mask != ~0u && (mask & Mask_Effect)
                    && !(mask & (Mask_Object | Mask_Static | Mask_Scene | Mask_Terrain)))
                {
                    bool object = false;
                    for (const auto* parent : getNodePath())
                    {
                        const auto parentMask = parent->getNodeMask();
                        if (parentMask != ~0u && (parentMask & (Mask_Object | Mask_Static)))
                        {
                            object = true;
                            break;
                        }
                    }
                    if (!object) return false;
                }
                return true;
            }

            void apply(osg::Node& node) override
            {
                if (allowed(node)) osgUtil::IntersectionVisitor::apply(node);
            }
            void apply(osg::Group& node) override
            {
                if (allowed(node)) osgUtil::IntersectionVisitor::apply(node);
            }
            void apply(osg::Geode& node) override
            {
                // IntersectionVisitor's Geode overload sends drawables straight
                // to its Intersector, bypassing apply(Drawable). Traverse this
                // group explicitly so we capture complete rendered triangles.
                if (allowed(node)) osgUtil::IntersectionVisitor::apply(static_cast<osg::Group&>(node));
            }
            void apply(osg::LOD& node) override
            {
                if (allowed(node)) osgUtil::IntersectionVisitor::apply(node);
            }
            void apply(osg::PagedLOD& node) override
            {
                if (allowed(node)) osgUtil::IntersectionVisitor::apply(node);
            }
            void apply(osg::Transform& node) override
            {
                if (allowed(node)) osgUtil::IntersectionVisitor::apply(node);
            }
            void apply(osg::Drawable& drawable) override
            {
                if (!allowed(drawable)) return;
                const auto* mesh = drawable.asGeometry();
                if (!mesh) return;
                const auto* positions = dynamic_cast<const osg::Vec3Array*>(mesh->getVertexArray());
                const auto* doublePositions = dynamic_cast<const osg::Vec3dArray*>(mesh->getVertexArray());
                if (!positions && !doublePositions) return;
                const auto world = osg::Matrixf(osg::computeLocalToWorld(getNodePath()));
                osg::BoundingBoxf bounds;
                for (unsigned corner = 0; corner < 8; ++corner)
                    bounds.expandBy(drawable.getBoundingBox().corner(corner) * world);
                if (!bounds.valid() || !intersects(bounds)) return;
                // Native TerrainDrawable carries this exact mask. Paged
                // decoration can have a huge bounding box containing the
                // corpse even though all of its first triangles are remote.
                // Reserve the early scan for actual land receivers.
                const bool terrain = drawable.getNodeMask() == Mask_Terrain;
                mMeshes.push_back({ mesh, world, distanceToBounds(bounds, mCenter), terrain });
            }

            void triangle(const std::array<osg::Vec3f, 3>& triangle)
            {
                if (++mMeshScanned > mMeshScanLimit) throw MeshScanFull{};
                if (++mScanned > MaxScannedTriangles) throw SnapshotFull{};
                osg::BoundingBoxf bounds;
                for (const auto& vertex : triangle)
                {
                    if (!std::isfinite(vertex.length2())) return;
                    bounds.expandBy(vertex);
                }
                if (!intersects(bounds)) return;
                if (((triangle[1] - triangle[0]) ^ (triangle[2] - triangle[0])).length2() < .00000001f)
                    return;
                const RetainedTriangle item{ triangle, distanceToBounds(bounds, mCenter) };
                if (mRetained.size() < MaxStoredTriangles)
                {
                    mRetained.push_back(item);
                    std::push_heap(mRetained.begin(), mRetained.end());
                }
                else if (item.mDistance < mRetained.front().mDistance)
                {
                    std::pop_heap(mRetained.begin(), mRetained.end());
                    mRetained.back() = item;
                    std::push_heap(mRetained.begin(), mRetained.end());
                }
            }

            std::vector<osg::Vec3f> finish()
            {
                // Visit nearby drawables before distant decoration. Within the
                // scan budget, retain the nearest triangles rather than whichever
                // geometry happened to appear first in a cell's scene graph.
                std::sort(mMeshes.begin(), mMeshes.end(), [](const auto& a, const auto& b) {
                    if (a.mTerrain != b.mTerrain) return a.mTerrain;
                    return a.mDistance < b.mDistance;
                });
                try
                {
                    for (std::size_t index = 0; index < mMeshes.size(); ++index)
                    {
                        const auto& mesh = mMeshes[index];
                        const auto remaining = MaxScannedTriangles - mScanned;
                        if (!remaining) break;
                        // Keep the native land mesh intact, then share the
                        // remaining work fairly between static drawables. A
                        // single merged city mesh cannot spend every later
                        // receiver's scan budget on remote triangles.
                        const auto share = remaining / (mMeshes.size() - index);
                        mMeshScanLimit = std::min(MaxTrianglesPerMesh,
                            mesh.mTerrain ? remaining : std::max<std::size_t>(1, share));
                        osg::TriangleIndexFunctor<Triangles> triangles;
                        triangles.mCollector = this;
                        triangles.mPositions = dynamic_cast<const osg::Vec3Array*>(mesh.mMesh->getVertexArray());
                        triangles.mDoublePositions = dynamic_cast<const osg::Vec3dArray*>(mesh.mMesh->getVertexArray());
                        triangles.mWorld = mesh.mWorld;
                        mMeshScanned = 0;
                        try { mesh.mMesh->accept(triangles); }
                        catch (const MeshScanFull&) { /* Share the global scan budget with other receivers. */ }
                    }
                }
                catch (const SnapshotFull&) {}
                std::sort_heap(mRetained.begin(), mRetained.end());
                std::vector<osg::Vec3f> result;
                result.reserve(mRetained.size() * 3);
                for (const auto& triangle : mRetained)
                    result.insert(result.end(), triangle.mVertices.begin(), triangle.mVertices.end());
                return result;
            }

        private:
            struct RetainedTriangle
            {
                std::array<osg::Vec3f, 3> mVertices;
                float mDistance;
                bool operator<(const RetainedTriangle& other) const { return mDistance < other.mDistance; }
            };
            struct Mesh
            {
                osg::ref_ptr<const osg::Geometry> mMesh;
                osg::Matrixf mWorld;
                float mDistance;
                bool mTerrain;
            };
            bool intersects(const osg::BoundingBoxf& bounds) const
            {
                return !(bounds.xMax() < mCenter.x() - mRadius || bounds.xMin() > mCenter.x() + mRadius
                    || bounds.yMax() < mCenter.y() - mRadius || bounds.yMin() > mCenter.y() + mRadius
                    || bounds.zMax() < mCenter.z() - mRadius || bounds.zMin() > mCenter.z() + mRadius);
            }
            std::vector<RetainedTriangle> mRetained;
            std::vector<Mesh> mMeshes;
            osg::Vec3f mCenter;
            float mRadius;
            const std::function<bool(const osg::Node&)>& mFilter;
            std::size_t mNodes = 0, mScanned = 0, mMeshScanned = 0, mMeshScanLimit = MaxTrianglesPerMesh;
        };

        void Triangles::operator()(unsigned a, unsigned b, unsigned c)
        {
            const auto size = mPositions ? mPositions->size() : mDoublePositions->size();
            if (a >= size || b >= size || c >= size) return;
            const auto read = [&](unsigned index) {
                return (mPositions ? (*mPositions)[index] : osg::Vec3f((*mDoublePositions)[index])) * mWorld;
            };
            mCollector->triangle({ read(a), read(b), read(c) });
        }
    }

    std::vector<osg::Vec3f> collectBloodSurfaceTriangles(osg::Node& root,
        const osg::Vec3f& center, float radius,
        const std::function<bool(const osg::Node&)>& filter,
        const osg::FrameStamp* frameStamp)
    {
        if (!std::isfinite(center.length2()) || !std::isfinite(radius) || radius <= 0) return {};
        Collector collector(center, std::min(radius, 2048.f), filter);
        if (frameStamp)
        {
            collector.setFrameStamp(new osg::FrameStamp(*frameStamp));
            collector.setTraversalNumber(frameStamp->getFrameNumber());
        }
        try { root.accept(collector); }
        catch (const SnapshotFull&) { /* Preserve the bounded prefix, never retry an unbounded traversal. */ }
        return collector.finish();
    }
}
