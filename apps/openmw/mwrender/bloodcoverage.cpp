#include "bloodcoverage.hpp"
#include "brutalblood.hpp"

#include <osg/Vec2d>
#include <osg/Vec3d>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace MWRender
{
    namespace
    {
        constexpr std::size_t MaxCandidates = 128;
        constexpr std::size_t MaxPieces = 128;
        constexpr double PlaneTolerance = .08;
        // Clipped float vertices far from the world origin can perturb the
        // normal of a narrow triangle. Distance to the receiver plane remains
        // the authoritative test; an almost exact normal match loses real
        // coverage on otherwise identical exterior faces.
        constexpr double ParallelNormals = .98;
        constexpr double AreaEpsilon = 1e-9;
        using Polygon = std::vector<osg::Vec2d>;

        struct BudgetExhausted {};
        struct Budget
        {
            std::size_t mRemaining;
            explicit Budget(std::size_t maxWork) : mRemaining(maxWork) {}
            void spend(std::size_t amount = 1)
            {
                if (amount > mRemaining) throw BudgetExhausted{};
                mRemaining -= amount;
            }
        };

        double cross(const osg::Vec2d& a, const osg::Vec2d& b)
        {
            return a.x() * b.y() - a.y() * b.x();
        }

        double area(const Polygon& polygon)
        {
            double result = 0;
            for (std::size_t i = 0; i < polygon.size(); ++i)
                result += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
            return result * .5;
        }

        void clean(Polygon& polygon)
        {
            Polygon result;
            result.reserve(polygon.size());
            for (const auto& point : polygon)
                if (result.empty() || (point - result.back()).length2() > 1e-20)
                    result.push_back(point);
            if (result.size() > 1 && (result.front() - result.back()).length2() <= 1e-20)
                result.pop_back();
            if (result.size() < 3 || std::abs(area(result)) <= AreaEpsilon)
                result.clear();
            else if (area(result) < 0)
                std::reverse(result.begin(), result.end());
            polygon = std::move(result);
        }

        Polygon halfPlane(const Polygon& polygon, const osg::Vec2d& a, const osg::Vec2d& b,
            double side, double threshold, Budget& budget)
        {
            Polygon result;
            if (polygon.empty()) return result;
            const auto edge = b - a;
            const double length = edge.length();
            if (length <= 1e-12) return polygon;
            result.reserve(polygon.size() + 1);
            auto previous = polygon.back();
            double previousDistance = side * cross(edge, previous - a) / length - threshold;
            for (const auto& current : polygon)
            {
                budget.spend();
                const double distance = side * cross(edge, current - a) / length - threshold;
                const bool previousInside = previousDistance >= 0;
                const bool inside = distance >= 0;
                if (inside != previousInside)
                {
                    const double fraction = previousDistance / (previousDistance - distance);
                    result.push_back(previous + (current - previous) * fraction);
                }
                if (inside) result.push_back(current);
                previous = current;
                previousDistance = distance;
            }
            clean(result);
            return result;
        }

        Polygon intersection(Polygon polygon, const Polygon& mask, Budget& budget)
        {
            for (std::size_t i = 0; i < mask.size() && !polygon.empty(); ++i)
                polygon = halfPlane(polygon, mask[i], mask[(i + 1) % mask.size()], 1, 0, budget);
            return polygon;
        }

        bool boundsOverlap(const Polygon& a, const Polygon& b)
        {
            if (a.empty() || b.empty()) return false;
            osg::Vec2d aMin = a.front(), aMax = a.front(), bMin = b.front(), bMax = b.front();
            for (const auto& point : a)
                for (unsigned axis = 0; axis < 2; ++axis)
                {
                    aMin[axis] = std::min(aMin[axis], point[axis]);
                    aMax[axis] = std::max(aMax[axis], point[axis]);
                }
            for (const auto& point : b)
                for (unsigned axis = 0; axis < 2; ++axis)
                {
                    bMin[axis] = std::min(bMin[axis], point[axis]);
                    bMax[axis] = std::max(bMax[axis], point[axis]);
                }
            return aMax.x() > bMin.x() && bMax.x() > aMin.x()
                && aMax.y() > bMin.y() && bMax.y() > aMin.y();
        }

        void subtract(std::vector<Polygon>& pieces, const Polygon& mask, double margin, Budget& budget)
        {
            std::vector<Polygon> result;
            for (const auto& piece : pieces)
            {
                if (!boundsOverlap(piece, mask) || intersection(piece, mask, budget).empty())
                {
                    result.push_back(piece);
                    continue;
                }
                auto inside = piece;
                // Each outside piece is disjoint from the pieces emitted for
                // earlier edges. The last inside polygon is precisely the
                // covered part and is discarded.
                for (std::size_t edge = 0; edge < mask.size() && !inside.empty(); ++edge)
                {
                    auto outside = halfPlane(inside, mask[edge], mask[(edge + 1) % mask.size()],
                        -1, margin, budget);
                    if (!outside.empty()) result.push_back(std::move(outside));
                    inside = halfPlane(inside, mask[edge], mask[(edge + 1) % mask.size()],
                        1, -margin, budget);
                    if (result.size() > MaxPieces) throw BudgetExhausted{};
                }
            }
            pieces = std::move(result);
        }

        struct TriangleBounds
        {
            double mMinimum[3], mMaximum[3];

            explicit TriangleBounds(const osg::Vec3f* vertices)
            {
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    mMinimum[axis] = std::min({ double(vertices[0][axis]),
                        double(vertices[1][axis]), double(vertices[2][axis]) });
                    mMaximum[axis] = std::max({ double(vertices[0][axis]),
                        double(vertices[1][axis]), double(vertices[2][axis]) });
                }
            }
        };

        struct Face
        {
            osg::Vec3d mOrigin, mNormal, mHorizontal, mVertical;
            double mMinimum[3]{}, mMaximum[3]{};
            Polygon mTriangle;
            double mMargin = 1e-5;
            double mPlaneTolerance = PlaneTolerance;

            explicit Face(const osg::Vec3f* vertices)
            {
                mOrigin = osg::Vec3d(vertices[0]);
                const auto a = osg::Vec3d(vertices[1]) - mOrigin;
                const auto b = osg::Vec3d(vertices[2]) - mOrigin;
                mNormal = a ^ b;
                if (!std::isfinite(mNormal.length2()) || mNormal.normalize() <= 1e-10 || a.length2() <= 1e-20)
                    return;
                // A clipped edge is rounded back to float world coordinates.
                // Leave a few ULPs of clearance so a load/rebuild cannot round
                // it into a third layer, including far from the world origin.
                double magnitude = 1;
                for (unsigned vertex = 0; vertex < 3; ++vertex)
                    for (unsigned axis = 0; axis < 3; ++axis)
                        magnitude = std::max(magnitude, std::abs(double(vertices[vertex][axis])));
                mMargin = std::max(mMargin, magnitude * double(std::numeric_limits<float>::epsilon()) * 4);
                mPlaneTolerance = std::max(PlaneTolerance, mMargin);
                const TriangleBounds bounds(vertices);
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    const double padding = std::abs(mNormal[axis]) * mPlaneTolerance;
                    mMinimum[axis] = bounds.mMinimum[axis] - padding;
                    mMaximum[axis] = bounds.mMaximum[axis] + padding;
                }
                mHorizontal = a;
                mHorizontal.normalize();
                mVertical = mNormal ^ mHorizontal;
                mTriangle = { project(vertices[0]), project(vertices[1]), project(vertices[2]) };
                clean(mTriangle);
            }

            osg::Vec2d project(const osg::Vec3f& point) const
            {
                const auto relative = osg::Vec3d(point) - mOrigin;
                return { relative * mHorizontal, relative * mVertical };
            }

            bool intersects(const TriangleBounds& bounds) const
            {
                return !(bounds.mMaximum[0] <= mMinimum[0] || bounds.mMinimum[0] >= mMaximum[0]
                    || bounds.mMaximum[1] <= mMinimum[1] || bounds.mMinimum[1] >= mMaximum[1]
                    || bounds.mMaximum[2] <= mMinimum[2] || bounds.mMinimum[2] >= mMaximum[2]);
            }

            Polygon coplanarTriangle(const osg::Vec3f* vertices, Budget& budget, bool boundsChecked = false) const
            {
                // Most receiver faces in a tessellated stamp are spatially
                // disjoint. Reject those using only scalar bounds before any
                // normals, polygon allocations or edge-topology comparisons.
                // Expand solely along the receiver normal: a floor's tolerance
                // must not make neighboring XY patches appear to overlap.
                if (!boundsChecked && !intersects(TriangleBounds(vertices))) return {};
                budget.spend();
                auto normal = (osg::Vec3d(vertices[1]) - osg::Vec3d(vertices[0]))
                    ^ (osg::Vec3d(vertices[2]) - osg::Vec3d(vertices[0]));
                if (!std::isfinite(normal.length2()) || normal.normalize() <= 1e-10
                    || std::abs(normal * mNormal) < ParallelNormals)
                    return {};
                Polygon result;
                result.reserve(3);
                for (unsigned vertex = 0; vertex < 3; ++vertex)
                {
                    const auto relative = osg::Vec3d(vertices[vertex]) - mOrigin;
                    if (!std::isfinite(relative.length2()) || std::abs(relative * mNormal) > mPlaneTolerance)
                        return {};
                    result.push_back(project(vertices[vertex]));
                }
                clean(result);
                if (!boundsOverlap(result, mTriangle)) return {};
                return intersection(mTriangle, result, budget);
            }

            bool adjacentTriangle(const osg::Vec3f* vertices) const
            {
                // Independently clipped receiver faces can round their common
                // edge to slightly different floats. Preserve genuine edge
                // neighbors, but not triangles on the same side of that edge.
                std::array<int, 3> matches{ -1, -1, -1 };
                unsigned count = 0;
                for (unsigned incoming = 0; incoming < 3; ++incoming)
                {
                    const auto world = mOrigin + mHorizontal * mTriangle[incoming].x()
                        + mVertical * mTriangle[incoming].y();
                    for (unsigned other = 0; other < 3; ++other)
                        if ((world - osg::Vec3d(vertices[other])).length2() <= mMargin * mMargin)
                        {
                            matches[incoming] = int(other);
                            ++count;
                            break;
                        }
                }
                if (count != 2) return false;
                unsigned first = 0;
                while (matches[first] < 0) ++first;
                unsigned second = first + 1;
                while (matches[second] < 0) ++second;
                if (matches[first] == matches[second]) return false;
                unsigned incomingThird = 0;
                while (incomingThird == first || incomingThird == second) ++incomingThird;
                unsigned otherThird = 0;
                while (int(otherThird) == matches[first] || int(otherThird) == matches[second]) ++otherThird;
                const auto edge = mTriangle[second] - mTriangle[first];
                return cross(edge, mTriangle[incomingThird] - mTriangle[first])
                        * cross(edge, project(vertices[otherThird]) - mTriangle[first])
                    < 0;
            }

            osg::Vec3f unproject(const osg::Vec2d& point) const
            {
                return osg::Vec3f(mOrigin + mHorizontal * point.x() + mVertical * point.y());
            }
        };
    }

    bool bloodStampContains(const BloodStamp& outer, const BloodStamp& inner, std::size_t maxWork)
    {
        if (outer.mTriangles.empty() || inner.mTriangles.empty()
            || outer.mTriangles.size() % 3 || inner.mTriangles.size() % 3) return false;
        Budget budget(maxWork);
        try
        {
            for (std::size_t vertex = 0; vertex < inner.mTriangles.size(); vertex += 3)
            {
                budget.spend();
                const Face face(inner.mTriangles.data() + vertex);
                if (face.mTriangle.empty()) return false;
                std::vector<Polygon> pieces{ face.mTriangle };
                for (std::size_t other = 0; other < outer.mTriangles.size() && !pieces.empty(); other += 3)
                {
                    const auto covered = face.coplanarTriangle(outer.mTriangles.data() + other, budget);
                    if (!covered.empty()) subtract(pieces, covered, 0, budget);
                }
                if (!pieces.empty()) return false;
            }
        }
        catch (const BudgetExhausted&) { return false; }
        return true;
    }

    std::vector<osg::Vec3f> clipBloodCoverage(const BloodStamp& incoming,
        const std::vector<const BloodStamp*>& existing, std::size_t maxVertices, std::size_t maxWork,
        bool* completed)
    {
        if (completed) *completed = true;
        std::vector<osg::Vec3f> result;
        maxVertices -= maxVertices % 3;
        if (incoming.mTriangles.empty() || incoming.mTriangles.size() % 3 || maxVertices == 0 || maxWork == 0)
        {
            if (completed) *completed = false;
            return result;
        }
        std::vector<const BloodStamp*> unique;
        unique.reserve(std::min(existing.size(), MaxCandidates));
        for (const auto* stamp : existing)
            if (stamp && stamp != &incoming && std::find(unique.begin(), unique.end(), stamp) == unique.end())
            {
                if (unique.size() == MaxCandidates)
                {
                    if (completed) *completed = false;
                    return {};
                }
                unique.push_back(stamp);
            }
        result.reserve(std::min(incoming.mTriangles.size(), maxVertices));
        Budget budget(maxWork);
        std::vector<std::array<osg::Vec3f, 3>> identities;
        identities.reserve(std::min(incoming.mTriangles.size(), maxVertices) / 3);
        std::vector<TriangleBounds> completedBounds;
        completedBounds.reserve(std::min(incoming.mTriangles.size(), maxVertices) / 3);
        for (std::size_t corner = 0; corner < incoming.mTriangles.size(); corner += 3)
        {
            try
            {
                budget.spend();
                const Face face(incoming.mTriangles.data() + corner);
                if (face.mTriangle.empty())
                {
                    if (completed) *completed = false;
                    continue;
                }
                std::array<osg::Vec3f, 3> identity{
                    incoming.mTriangles[corner], incoming.mTriangles[corner + 1], incoming.mTriangles[corner + 2] };
                std::sort(identity.begin(), identity.end());
                if (std::find(identities.begin(), identities.end(), identity) != identities.end()) continue;
                identities.push_back(identity);
                std::vector<Polygon> pieces{ face.mTriangle };
                std::vector<std::vector<Polygon>> masks;
                masks.reserve(unique.size());
                // One existing stain can only make this the second layer.
                // Avoid all external projection/mask allocations in that common
                // case while still normalizing duplicate receiver geometry.
                if (unique.size() >= 2)
                    for (const auto* stamp : unique)
                    {
                        if (stamp->mTriangles.size() % 3) continue;
                        std::vector<Polygon> group;
                        for (std::size_t other = 0; other < stamp->mTriangles.size(); other += 3)
                        {
                            auto polygon = face.coplanarTriangle(stamp->mTriangles.data() + other, budget);
                            if (!polygon.empty()) group.push_back(std::move(polygon));
                        }
                        if (!group.empty()) masks.push_back(std::move(group));
                    }
                for (std::size_t first = 0; first < masks.size() && !pieces.empty(); ++first)
                    for (std::size_t second = first + 1; second < masks.size() && !pieces.empty(); ++second)
                        for (const auto& a : masks[first])
                            for (const auto& b : masks[second])
                            {
                                budget.spend();
                                if (!boundsOverlap(a, b)) continue;
                                const auto covered = intersection(a, b, budget);
                                if (!covered.empty()) subtract(pieces, covered, face.mMargin, budget);
                                if (pieces.empty()) break;
                            }

                // Exact copies were handled cheaply above. Receiver geometry
                // can also contain the same patch triangulated with different
                // diagonals; remove those positive-area overlaps within a stain.
                // Do not erode genuine shared receiver edges on save/load.
                const double internalTolerance = std::max(1e-7, area(face.mTriangle) * 1e-8);
                for (std::size_t previous = 0; previous < result.size() && !pieces.empty(); previous += 3)
                {
                    if (!face.intersects(completedBounds[previous / 3])) continue;
                    const auto covered = face.coplanarTriangle(result.data() + previous, budget, true);
                    if (!covered.empty() && area(covered) > internalTolerance
                        && !face.adjacentTriangle(result.data() + previous))
                        subtract(pieces, covered, 0, budget);
                }
                if (pieces.size() == 1 && pieces.front() == face.mTriangle)
                {
                    if (result.size() + 3 > maxVertices)
                    {
                        if (completed) *completed = false;
                        return result;
                    }
                    result.insert(result.end(), incoming.mTriangles.begin() + corner,
                        incoming.mTriangles.begin() + corner + 3);
                    completedBounds.emplace_back(result.data() + result.size() - 3);
                    continue;
                }
                for (const auto& piece : pieces)
                    for (std::size_t point = 1; point + 1 < piece.size(); ++point)
                    {
                        if (std::abs(cross(piece[point] - piece[0], piece[point + 1] - piece[0])) <= AreaEpsilon * 2)
                            continue;
                        if (result.size() + 3 > maxVertices)
                        {
                            if (completed) *completed = false;
                            return result;
                        }
                        result.push_back(face.unproject(piece[0]));
                        result.push_back(face.unproject(piece[point]));
                        result.push_back(face.unproject(piece[point + 1]));
                        completedBounds.emplace_back(result.data() + result.size() - 3);
                    }
            }
            catch (const BudgetExhausted&)
            {
                // Growth replaces a live stain atomically. A partial update
                // would erase its unchecked old faces and make a pool shrink.
                if (completed) *completed = false;
                return {};
            }
        }
        return result;
    }
}
