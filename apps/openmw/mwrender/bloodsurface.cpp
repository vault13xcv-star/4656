#include "brutalblood.hpp"

#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace MWRender
{
    namespace
    {
        constexpr float SurfaceOffset = .03f;
        constexpr std::size_t MaxCandidates = 256;
        constexpr unsigned MaxVisibilityChecks = 64;
        constexpr std::size_t MaxInputVertices = 3 * 131072;

        btVector3 toBullet(const osg::Vec3f& value) { return { value.x(), value.y(), value.z() }; }
        osg::Vec3f toOsg(const btVector3& value)
        {
            return { float(value.x()), float(value.y()), float(value.z()) };
        }
        bool normalize(osg::Vec3f& value)
        {
            return std::isfinite(value.length2()) && value.normalize() > .0001f;
        }

        struct Polygon
        {
            // A triangle clipped by six planes has at most nine corners.
            std::array<osg::Vec3f, 12> mPoints;
            std::size_t mCount = 0;
        };

        void clip(Polygon& polygon, const osg::Vec3f& origin, const osg::Vec3f& axis, float limit)
        {
            if (!polygon.mCount) return;
            Polygon result;
            auto previous = polygon.mPoints[polygon.mCount - 1];
            float previousDistance = (previous - origin) * axis - limit;
            for (std::size_t i = 0; i < polygon.mCount; ++i)
            {
                const auto current = polygon.mPoints[i];
                const float distance = (current - origin) * axis - limit;
                const bool previousInside = previousDistance <= 0;
                const bool inside = distance <= 0;
                if (inside != previousInside && result.mCount < result.mPoints.size())
                {
                    const float fraction = previousDistance / (previousDistance - distance);
                    result.mPoints[result.mCount++] = previous + (current - previous) * fraction;
                }
                if (inside && result.mCount < result.mPoints.size())
                    result.mPoints[result.mCount++] = current;
                previous = current;
                previousDistance = distance;
            }
            polygon = result;
        }

        struct Candidate
        {
            std::array<osg::Vec3f, 3> mVertices;
            float mDistance = 0;
            int mTriangle = -1;
        };

        float distanceToTriangle(const osg::Vec3f& point, const std::array<osg::Vec3f, 3>& vertices)
        {
            const auto a = vertices[0], b = vertices[1], c = vertices[2];
            const auto normal = (b - a) ^ (c - a);
            const float area = normal.length2();
            if (area < .00000001f) return (point - a).length2();
            const auto projected = point - normal * (((point - a) * normal) / area);
            if ((((b - a) ^ (projected - a)) * normal) >= 0
                && (((c - b) ^ (projected - b)) * normal) >= 0
                && (((a - c) ^ (projected - c)) * normal) >= 0)
                return (point - projected).length2();
            float distance = (point - a).length2();
            for (std::size_t i = 0; i < 3; ++i)
            {
                const auto edge = vertices[(i + 1) % 3] - vertices[i];
                const float length = edge.length2();
                const float fraction = length > .00000001f
                    ? std::clamp(((point - vertices[i]) * edge) / length, 0.f, 1.f) : 0.f;
                distance = std::min(distance, (point - vertices[i] - edge * fraction).length2());
            }
            return distance;
        }

        struct CandidateQuery final : btTriangleCallback
        {
            const osg::Vec3f mCenter;
            std::vector<Candidate> mCandidates;

            explicit CandidateQuery(const osg::Vec3f& center) : mCenter(center)
            {
                mCandidates.reserve(MaxCandidates);
            }
            void processTriangle(btVector3* triangle, int, int triangleIndex) override
            {
                Candidate candidate{ { toOsg(triangle[0]), toOsg(triangle[1]), toOsg(triangle[2]) }, 0, triangleIndex };
                // A large floor triangle may have a very distant centroid.
                // Prioritize its nearest point, so nearby dense furniture
                // cannot push the actual floor out of the bounded query.
                candidate.mDistance = distanceToTriangle(mCenter, candidate.mVertices);
                if (mCandidates.size() < MaxCandidates)
                {
                    mCandidates.push_back(candidate);
                    std::push_heap(mCandidates.begin(), mCandidates.end(), nearer);
                }
                else if (candidate.mDistance < mCandidates.front().mDistance)
                {
                    std::pop_heap(mCandidates.begin(), mCandidates.end(), nearer);
                    mCandidates.back() = candidate;
                    std::push_heap(mCandidates.begin(), mCandidates.end(), nearer);
                }
            }
            static bool nearer(const Candidate& a, const Candidate& b) { return a.mDistance < b.mDistance; }
        };

        class SurfaceMesh
        {
        public:
            explicit SurfaceMesh(const std::vector<osg::Vec3f>& triangles)
            {
                for (std::size_t i = 0; i < triangles.size(); i += 3)
                {
                    if (((triangles[i + 1] - triangles[i]) ^ (triangles[i + 2] - triangles[i])).length2() < .00000001f)
                        continue;
                    mTriangles.addTriangle(toBullet(triangles[i]), toBullet(triangles[i + 1]), toBullet(triangles[i + 2]));
                }
                if (mTriangles.getNumTriangles())
                    mShape = std::make_unique<btBvhTriangleMeshShape>(&mTriangles, true);
            }

            bool valid() const { return bool(mShape); }

            bool raycast(const osg::Vec3f& a, const osg::Vec3f& b, osg::Vec3f& point, osg::Vec3f& normal,
                int* triangleIndex = nullptr) const
            {
                if (!mShape || !std::isfinite(a.length2()) || !std::isfinite(b.length2()) || (b - a).length2() < .00000001f)
                    return false;
                btCollisionObject object;
                object.setCollisionShape(mShape.get());
                object.setWorldTransform(btTransform::getIdentity());
                auto from = btTransform::getIdentity(), to = from;
                from.setOrigin(toBullet(a)); to.setOrigin(toBullet(b));
                struct ClosestSurfaceRay final : btCollisionWorld::ClosestRayResultCallback
                {
                    int mTriangle = -1;
                    using btCollisionWorld::ClosestRayResultCallback::ClosestRayResultCallback;
                    btScalar addSingleResult(btCollisionWorld::LocalRayResult& result, bool worldNormal) override
                    {
                        if (result.m_hitFraction <= m_closestHitFraction)
                            mTriangle = result.m_localShapeInfo ? result.m_localShapeInfo->m_triangleIndex : -1;
                        return btCollisionWorld::ClosestRayResultCallback::addSingleResult(result, worldNormal);
                    }
                } hit(toBullet(a), toBullet(b));
                btCollisionWorld::rayTestSingle(from, to, &object, mShape.get(), object.getWorldTransform(), hit);
                if (!hit.hasHit()) return false;
                point = toOsg(hit.m_hitPointWorld);
                normal = toOsg(hit.m_hitNormalWorld);
                if (triangleIndex) *triangleIndex = hit.mTriangle;
                return std::isfinite(point.length2()) && normalize(normal);
            }

            std::vector<osg::Vec3f> project(const osg::Vec3f& center, osg::Vec3f normal, float radius) const
            {
                std::vector<osg::Vec3f> result;
                if (!mShape || !std::isfinite(center.length2()) || !normalize(normal)
                    || !std::isfinite(radius) || radius <= 0 || radius > 256)
                    return result;
                const osg::Vec3f up = normal.z() >= .7f ? osg::Vec3f(0, 1, 0) : osg::Vec3f(0, 0, 1);
                auto vertical = up - normal * (up * normal);
                if (!normalize(vertical)) return result;
                auto horizontal = vertical ^ normal;
                if (!normalize(horizontal)) return result;
                // Project inward from the impact plane. A floor hit under a
                // table must not jump upward onto its top, or be rejected by
                // a low ceiling. The back depth admits lower stair treads and
                // a column's curvature, while first-surface visibility excludes
                // hidden parallel faces behind the actual receiver.
                const float depth = std::clamp(radius * 1.5f, 4.f, 64.f);
                constexpr float frontDepth = 2.f;
                const float largestDepth = std::max(depth, frontDepth);
                const osg::Vec3f extent(
                    (std::abs(horizontal.x()) + std::abs(vertical.x())) * radius + std::abs(normal.x()) * largestDepth,
                    (std::abs(horizontal.y()) + std::abs(vertical.y())) * radius + std::abs(normal.y()) * largestDepth,
                    (std::abs(horizontal.z()) + std::abs(vertical.z())) * radius + std::abs(normal.z()) * largestDepth);
                CandidateQuery query(center);
                mShape->processAllTriangles(&query, toBullet(center - extent), toBullet(center + extent));
                std::sort(query.mCandidates.begin(), query.mCandidates.end(), CandidateQuery::nearer);
                result.reserve(ESM::BrutalBloodState::MaxVerticesPerStain);
                unsigned visibilityChecks = 0;
                for (const auto& triangle : query.mCandidates)
                {
                    auto faceNormal = (triangle.mVertices[1] - triangle.mVertices[0])
                        ^ (triangle.mVertices[2] - triangle.mVertices[0]);
                    if (!normalize(faceNormal)) continue;
                    const float facing = faceNormal * normal;
                    if (std::abs(facing) < .15f) continue;
                    if (facing < 0) faceNormal = -faceNormal;
                    Polygon polygon;
                    polygon.mCount = 3;
                    std::copy(triangle.mVertices.begin(), triangle.mVertices.end(), polygon.mPoints.begin());
                    clip(polygon, center, horizontal, radius);
                    clip(polygon, center, -horizontal, radius);
                    clip(polygon, center, vertical, radius);
                    clip(polygon, center, -vertical, radius);
                    clip(polygon, center, normal, frontDepth);
                    clip(polygon, center, -normal, depth);
                    if (polygon.mCount < 3) continue;
                    if (visibilityChecks++ >= MaxVisibilityChecks) break;
                    osg::Vec3f centroid;
                    for (std::size_t i = 0; i < polygon.mCount; ++i) centroid += polygon.mPoints[i];
                    centroid /= float(polygon.mCount);
                    osg::Vec3f visible, visibleNormal;
                    int visibleTriangle = -1;
                    const auto rayStart = centroid + normal * (frontDepth + .1f - ((centroid - center) * normal));
                    if (!raycast(rayStart, centroid - normal * .1f, visible, visibleNormal, &visibleTriangle)
                        || visibleTriangle != triangle.mTriangle || (visible - centroid).length2() > .04f)
                        continue;
                    for (std::size_t i = 1; i + 1 < polygon.mCount; ++i)
                    {
                        if (result.size() + 3 > ESM::BrutalBloodState::MaxVerticesPerStain) return result;
                        const auto a = polygon.mPoints[0], b = polygon.mPoints[i], c = polygon.mPoints[i + 1];
                        if (((b - a) ^ (c - a)).length2() < .00000001f) continue;
                        result.push_back(a + faceNormal * SurfaceOffset);
                        result.push_back(b + faceNormal * SurfaceOffset);
                        result.push_back(c + faceNormal * SurfaceOffset);
                    }
                }
                return result;
            }

        private:
            btTriangleMesh mTriangles;
            std::unique_ptr<btBvhTriangleMeshShape> mShape;
        };
    }

    BloodTrail::Surface BloodTrail::visualSurface(const std::vector<osg::Vec3f>& triangles)
    {
        if (triangles.empty() || triangles.size() % 3 || triangles.size() > MaxInputVertices
            || std::any_of(triangles.begin(), triangles.end(), [](const auto& v) { return !std::isfinite(v.length2()); }))
            return {};
        auto mesh = std::make_shared<SurfaceMesh>(triangles);
        if (!mesh->valid()) return {};
        return {
            [mesh](const auto& a, const auto& b, auto& p, auto& n) { return mesh->raycast(a, b, p, n); },
            [mesh](const auto& p, const auto& n, float r) { return mesh->project(p, n, r); }
        };
    }
}
