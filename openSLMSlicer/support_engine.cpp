#include "support_engine.h"

#include <pyslm/core.hpp>
#include <pyslm/support.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
// Block support parameters validated by the pyslm_cpp PoC (out/support_poc):
// grid aligned solid block supports, 45 degree overhang, small region filtering.
constexpr double kGridSpacingMm = 3.0;
constexpr double kOverhangAngleDeg = kSupportOverhangAngleDeg;
constexpr double kMinimumAreaThresholdMm2 = 5.0;
constexpr double kRayProjectionResolutionMm = 0.2;
constexpr double kSupportBorderDistanceMm = 3.0;

// Support volume components below this volume are treated as degenerate and
// dropped. A real support solid is several mm^3, so this only removes slivers.
constexpr double kMinBlockVolumeMm3 = 1.0e-6;

// Vertex welding tolerance used to find connected support components.
constexpr double kWeldToleranceMm = 1.0e-6;
constexpr double kInverseWeldTolerance = 1.0 / kWeldToleranceMm;

bool IsFinitePoint(const std::array<double, 3>& point)
{
    return std::isfinite(point[0]) && std::isfinite(point[1]) &&
           std::isfinite(point[2]);
}

// Quantized vertex key: identical keys mean the vertices are welded together.
struct WeldedVertexKey
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
    bool operator==(const WeldedVertexKey&) const = default;
};

struct WeldedVertexKeyHash
{
    std::size_t operator()(const WeldedVertexKey& key) const noexcept
    {
        // FNV-1a over the three quantized coordinates, then an avalanche mix so
        // neighbouring coordinates spread over the bucket array.
        std::uint64_t hash = 1469598103934665603ull;
        const auto combine = [&hash](std::int64_t value) {
            hash ^= static_cast<std::uint64_t>(value);
            hash *= 1099511628211ull;
        };
        combine(key.x);
        combine(key.y);
        combine(key.z);
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdull;
        hash ^= hash >> 33;
        return static_cast<std::size_t>(hash);
    }
};

// Disjoint-set forest over triangles (path halving, union by size). Used by
// SplitTriangleComponents to keep the split linear in the triangle count.
class DisjointSets
{
public:
    explicit DisjointSets(std::size_t count) : parents_(count), sizes_(count, 1)
    {
        for (std::size_t index = 0; index < count; ++index)
            parents_[index] = index;
    }

    std::size_t Find(std::size_t index)
    {
        while (parents_[index] != index)
        {
            parents_[index] = parents_[parents_[index]];
            index = parents_[index];
        }
        return index;
    }

    void Unite(std::size_t first, std::size_t second)
    {
        first = Find(first);
        second = Find(second);
        if (first == second)
            return;
        if (sizes_[first] < sizes_[second])
            std::swap(first, second);
        parents_[second] = first;
        sizes_[first] += sizes_[second];
    }

private:
    std::vector<std::size_t> parents_;
    std::vector<std::size_t> sizes_;
};

std::string ModelLabel(const SupportModelInput& input)
{
    return input.name.empty() ? std::string("未命名模型") : input.name;
}

std::string ModelContext(const SupportModelInput& input)
{
    return "（模型「" + ModelLabel(input) + "」）";
}

// Converts a pyslm mesh into independent application triangles. Returns false
// when a face references a vertex outside the mesh.
bool AppendMeshTriangles(const pyslm::Mesh& mesh,
                         std::vector<SliceTriangle>& triangles,
                         std::string& error)
{
    triangles.reserve(triangles.size() + mesh.faces.size());
    for (const pyslm::Triangle& face : mesh.faces)
    {
        SliceTriangle triangle{};
        for (int corner = 0; corner < 3; ++corner)
        {
            const std::size_t index = static_cast<std::size_t>(face[corner]);
            if (index >= mesh.vertices.size())
            {
                error = "支撑网格包含越界顶点索引。";
                return false;
            }
            const pyslm::Vec3& vertex = mesh.vertices[index];
            triangle.vertices[corner] = {vertex.x, vertex.y, vertex.z};
        }
        triangles.push_back(triangle);
    }
    return true;
}

// Transforms application triangles into the pyslm mesh layout used by the
// support generator: every triangle gets its own three vertices.
bool BuildMeshFromTriangles(const std::vector<SliceTriangle>& triangles,
                            pyslm::Mesh& mesh, std::string& error)
{
    mesh.vertices.reserve(triangles.size() * 3);
    mesh.faces.reserve(triangles.size());
    for (std::size_t index = 0; index < triangles.size(); ++index)
    {
        const SliceTriangle& triangle = triangles[index];
        for (int corner = 0; corner < 3; ++corner)
        {
            const std::array<double, 3>& point = triangle.vertices[corner];
            if (!IsFinitePoint(point))
            {
                error = "模型三角面包含非有限坐标。";
                return false;
            }
            mesh.vertices.push_back({point[0], point[1], point[2]});
        }
        const std::uint32_t base = static_cast<std::uint32_t>(index * 3);
        mesh.faces.push_back({base, base + 1, base + 2});
    }
    return true;
}
} // namespace

bool GenerateSupportForModel(const SupportModelInput& input,
                             SupportGenerationOutput& output,
                             std::string& error)
{
    output = {};
    error.clear();

    if (input.world_triangles_mm.empty())
    {
        output.state = SupportState::NotNeeded;
        return true;
    }

    try
    {
        if (input.world_triangles_mm.size() >
            std::numeric_limits<std::uint32_t>::max() / 3)
        {
            error = "模型三角面数量超出支撑生成上限" + ModelContext(input) + "。";
            return false;
        }

        pyslm::Mesh part_mesh;
        if (!BuildMeshFromTriangles(input.world_triangles_mm, part_mesh, error))
        {
            error += ModelContext(input);
            return false;
        }

        // The support structures keep a non-owning pointer to this Part, so the
        // Part (and the mesh it owns) must stay alive until all support meshes
        // have been copied out below.
        pyslm::Part part(ModelLabel(input));
        part.setGeometry(part_mesh, /*fixGeometry=*/true, /*mergeVertices=*/true);

        pyslm::support::GridBlockSupportGenerator generator;
        generator.gridSpacing = {kGridSpacingMm, kGridSpacingMm};
        generator.overhangAngle = kOverhangAngleDeg;
        generator.minimumAreaThreshold = kMinimumAreaThresholdMm2;
        generator.rayProjectionResolution = kRayProjectionResolutionMm;
        generator.supportBorderDistance = kSupportBorderDistanceMm;

        const std::vector<pyslm::support::GridBlockSupport> supports =
            generator.identifySupportRegions(part, kOverhangAngleDeg,
                                             /*findSelfIntersectingSupport=*/true);

        // One pyslm support region can contain several disconnected solids in its
        // supportVolume mesh. Split every region into connected components and
        // emit one independent block per component so the caller can list,
        // show/hide and export them individually. Blocks with a zero volume or
        // an empty supportVolume are the "model needs no support" result and are
        // skipped.
        std::size_t total_triangles = 0;
        std::vector<SupportGenerationBlock> blocks;
        for (const pyslm::support::GridBlockSupport& support : supports)
        {
            if (!(support.volume() > 0.0))
                continue;
            if (!support.supportVolume || support.supportVolume->faces.empty())
                continue;

            std::vector<SliceTriangle> world_triangles;
            if (!AppendMeshTriangles(*support.supportVolume, world_triangles, error))
            {
                error += ModelContext(input);
                return false;
            }
            if (world_triangles.empty())
                continue;
            for (const SliceTriangle& triangle : world_triangles)
                for (int corner = 0; corner < 3; ++corner)
                    if (!IsFinitePoint(triangle.vertices[corner]))
                    {
                        error = "生成的支撑网格包含非有限坐标" +
                                ModelContext(input) + "。";
                        return false;
                    }

            const std::vector<std::vector<SliceTriangle>> components =
                SplitTriangleComponents(world_triangles);
            for (const std::vector<SliceTriangle>& component : components)
            {
                if (component.empty())
                    continue;

                const double volume_mm3 = SupportMeshVolumeMm3(component);
                if (!(volume_mm3 > kMinBlockVolumeMm3))
                    continue; // degenerate sliver component

                total_triangles += component.size();
                if (total_triangles > kMaxSupportTrianglesPerModel)
                {
                    error = "生成的支撑三角面数 " +
                            std::to_string(total_triangles) + " 超过上限 " +
                            std::to_string(kMaxSupportTrianglesPerModel) +
                            ModelContext(input) + "。";
                    return false;
                }

                SupportGenerationBlock block;
                block.volume_mm3 = volume_mm3;
                block.local_triangles =
                    SupportLocalTriangles(component, input.model_center_x_mm,
                                          input.model_center_y_mm);
                block.triangle_count = block.local_triangles.size();
                blocks.push_back(std::move(block));
            }
        }

        if (blocks.empty())
        {
            output.state = SupportState::NotNeeded;
            return true;
        }

        output.blocks = std::move(blocks);
        output.state = SupportState::Generated;
        return true;
    }
    catch (const std::exception& exception)
    {
        output = {};
        error = "生成支撑失败" + ModelContext(input) + "：" + exception.what();
        return false;
    }
    catch (...)
    {
        output = {};
        error = "生成支撑失败" + ModelContext(input) +
                "：支撑库抛出未知异常。";
        return false;
    }
}

std::vector<SliceTriangle> SupportLocalTriangles(
    const std::vector<SliceTriangle>& world_triangles, double center_x_mm,
    double center_y_mm)
{
    std::vector<SliceTriangle> local_triangles = world_triangles;
    for (SliceTriangle& triangle : local_triangles)
        for (std::array<double, 3>& vertex : triangle.vertices)
        {
            vertex[0] -= center_x_mm;
            vertex[1] -= center_y_mm;
        }
    return local_triangles;
}

std::vector<SliceTriangle> SupportWorldTriangles(
    const std::vector<SliceTriangle>& local_triangles, double center_x_mm,
    double center_y_mm)
{
    std::vector<SliceTriangle> world_triangles = local_triangles;
    for (SliceTriangle& triangle : world_triangles)
        for (std::array<double, 3>& vertex : triangle.vertices)
        {
            vertex[0] += center_x_mm;
            vertex[1] += center_y_mm;
        }
    return world_triangles;
}

std::vector<std::vector<SliceTriangle>> SplitTriangleComponents(
    const std::vector<SliceTriangle>& triangles)
{
    std::vector<std::vector<SliceTriangle>> components;
    if (triangles.empty())
        return components;

    // Union-find over triangles: two triangles belong to the same component as
    // soon as they share one welded vertex. Processing every vertex once keeps
    // the split linear in the number of triangles.
    DisjointSets sets(triangles.size());
    std::unordered_map<WeldedVertexKey, std::size_t, WeldedVertexKeyHash>
        vertex_owners;
    vertex_owners.reserve(triangles.size() * 3);

    for (std::size_t index = 0; index < triangles.size(); ++index)
    {
        for (int corner = 0; corner < 3; ++corner)
        {
            const std::array<double, 3>& point = triangles[index].vertices[corner];
            if (!IsFinitePoint(point))
                continue; // non-finite vertices never weld to anything
            const WeldedVertexKey key{
                static_cast<std::int64_t>(
                    std::llround(point[0] * kInverseWeldTolerance)),
                static_cast<std::int64_t>(
                    std::llround(point[1] * kInverseWeldTolerance)),
                static_cast<std::int64_t>(
                    std::llround(point[2] * kInverseWeldTolerance))};
            const auto inserted = vertex_owners.try_emplace(key, index);
            if (!inserted.second)
                sets.Unite(index, inserted.first->second);
        }
    }

    // Group triangle indices by their union-find root.
    constexpr std::size_t kNoGroup = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> root_groups(triangles.size(), kNoGroup);
    std::vector<std::vector<std::size_t>> grouped_triangles;
    for (std::size_t index = 0; index < triangles.size(); ++index)
    {
        const std::size_t root = sets.Find(index);
        std::size_t& group = root_groups[root];
        if (group == kNoGroup)
        {
            group = grouped_triangles.size();
            grouped_triangles.emplace_back();
        }
        grouped_triangles[group].push_back(index);
    }

    // Deterministic component order: bbox minimum X, Y, Z, then triangle count,
    // then the original first triangle index as the final tie breaker.
    struct GroupOrder
    {
        std::size_t group = 0;
        std::size_t first_triangle = 0;
        std::size_t triangle_count = 0;
        std::array<double, 3> min{{0.0, 0.0, 0.0}};
    };
    std::vector<GroupOrder> order;
    order.reserve(grouped_triangles.size());
    for (std::size_t group = 0; group < grouped_triangles.size(); ++group)
    {
        GroupOrder entry;
        entry.group = group;
        entry.first_triangle = grouped_triangles[group].front();
        entry.triangle_count = grouped_triangles[group].size();
        entry.min = {std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity()};
        for (std::size_t index : grouped_triangles[group])
            for (const std::array<double, 3>& point : triangles[index].vertices)
                for (int axis = 0; axis < 3; ++axis)
                    if (std::isfinite(point[axis]) &&
                        point[axis] < entry.min[axis])
                        entry.min[axis] = point[axis];
        order.push_back(entry);
    }
    std::sort(order.begin(), order.end(),
              [](const GroupOrder& first, const GroupOrder& second) {
                  for (int axis = 0; axis < 3; ++axis)
                      if (first.min[axis] != second.min[axis])
                          return first.min[axis] < second.min[axis];
                  if (first.triangle_count != second.triangle_count)
                      return first.triangle_count < second.triangle_count;
                  return first.first_triangle < second.first_triangle;
              });

    // Copy the triangles in component order, preserving the input order inside
    // each component and the original coordinates.
    components.reserve(order.size());
    for (const GroupOrder& entry : order)
    {
        std::vector<SliceTriangle> component;
        component.reserve(entry.triangle_count);
        for (std::size_t index : grouped_triangles[entry.group])
            component.push_back(triangles[index]);
        components.push_back(std::move(component));
    }
    return components;
}

double SupportMeshVolumeMm3(const std::vector<SliceTriangle>& triangles)
{
    // Signed volume of a closed triangle soup: sum over faces of
    // dot(a, cross(b, c)) / 6. Translation invariant for closed meshes.
    double signed_volume6 = 0.0;
    for (const SliceTriangle& triangle : triangles)
    {
        const std::array<double, 3>& a = triangle.vertices[0];
        const std::array<double, 3>& b = triangle.vertices[1];
        const std::array<double, 3>& c = triangle.vertices[2];
        if (!IsFinitePoint(a) || !IsFinitePoint(b) || !IsFinitePoint(c))
            return 0.0;
        signed_volume6 +=
            a[0] * (b[1] * c[2] - b[2] * c[1]) +
            a[1] * (b[2] * c[0] - b[0] * c[2]) +
            a[2] * (b[0] * c[1] - b[1] * c[0]);
    }
    if (!std::isfinite(signed_volume6))
        return 0.0;
    return std::abs(signed_volume6) / 6.0;
}
