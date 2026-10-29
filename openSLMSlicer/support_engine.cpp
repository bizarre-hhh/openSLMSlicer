#include "support_engine.h"

#include <pyslm/core.hpp>
#include <pyslm/support.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
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

bool IsFinitePoint(const std::array<double, 3>& point)
{
    return std::isfinite(point[0]) && std::isfinite(point[1]) &&
           std::isfinite(point[2]);
}

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

        // Merge the solid support volumes of all non-degenerate blocks into one
        // world-space triangle soup. Blocks with a zero volume or an empty
        // supportVolume are the "model needs no support" result and are skipped.
        pyslm::Mesh merged;
        for (const pyslm::support::GridBlockSupport& support : supports)
        {
            if (!(support.volume() > 0.0))
                continue;
            if (!support.supportVolume || support.supportVolume->faces.empty())
                continue;

            const pyslm::Mesh& volume = *support.supportVolume;
            if (merged.vertices.size() + volume.vertices.size() >
                static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            {
                error = "支撑网格顶点数超出上限" + ModelContext(input) + "。";
                return false;
            }

            const std::uint32_t base =
                static_cast<std::uint32_t>(merged.vertices.size());
            merged.vertices.insert(merged.vertices.end(), volume.vertices.begin(),
                                   volume.vertices.end());
            merged.faces.reserve(merged.faces.size() + volume.faces.size());
            for (const pyslm::Triangle& face : volume.faces)
                merged.faces.push_back({base + face[0], base + face[1], base + face[2]});
        }

        if (merged.faces.empty())
        {
            output.state = SupportState::NotNeeded;
            return true;
        }

        if (merged.faces.size() > kMaxSupportTrianglesPerModel)
        {
            error = "生成的支撑三角面数 " + std::to_string(merged.faces.size()) +
                    " 超过上限 " +
                    std::to_string(kMaxSupportTrianglesPerModel) +
                    ModelContext(input) + "。";
            return false;
        }

        std::vector<SliceTriangle> world_triangles;
        if (!AppendMeshTriangles(merged, world_triangles, error))
        {
            error += ModelContext(input);
            return false;
        }

        for (const SliceTriangle& triangle : world_triangles)
            for (int corner = 0; corner < 3; ++corner)
                if (!IsFinitePoint(triangle.vertices[corner]))
                {
                    error = "生成的支撑网格包含非有限坐标" +
                            ModelContext(input) + "。";
                    return false;
                }

        output.volume_mm3 = SupportMeshVolumeMm3(world_triangles);
        output.local_triangles =
            SupportLocalTriangles(world_triangles, input.model_center_x_mm,
                                  input.model_center_y_mm);
        output.triangle_count = output.local_triangles.size();
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
