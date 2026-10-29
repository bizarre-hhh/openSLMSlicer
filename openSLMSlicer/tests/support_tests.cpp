#include "support_engine.h"

#include <pyslm/types.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

void Require(bool condition, const std::string& message)
{
    if (condition)
        return;
    std::cerr << "[FAIL] " << message << '\n';
    ++g_failures;
}

std::string Format(double value)
{
    return std::to_string(value);
}

SliceTriangle MakeTriangle(const std::array<double, 3>& a,
                           const std::array<double, 3>& b,
                           const std::array<double, 3>& c)
{
    SliceTriangle triangle{};
    triangle.vertices[0] = a;
    triangle.vertices[1] = b;
    triangle.vertices[2] = c;
    return triangle;
}

// Axis aligned closed box from min to max corner (12 outward facing triangles).
std::vector<SliceTriangle> MakeBox(double x_min, double y_min, double z_min,
                                   double x_max, double y_max, double z_max)
{
    const std::array<std::array<double, 3>, 8> vertices{{
        {{x_min, y_min, z_min}},
        {{x_max, y_min, z_min}},
        {{x_max, y_max, z_min}},
        {{x_min, y_max, z_min}},
        {{x_min, y_min, z_max}},
        {{x_max, y_min, z_max}},
        {{x_max, y_max, z_max}},
        {{x_min, y_max, z_max}}
    }};
    const std::array<std::array<int, 3>, 12> faces{{
        {{0, 2, 1}}, {{0, 3, 2}},
        {{4, 5, 6}}, {{4, 6, 7}},
        {{0, 1, 5}}, {{0, 5, 4}},
        {{1, 2, 6}}, {{1, 6, 5}},
        {{2, 3, 7}}, {{2, 7, 6}},
        {{3, 0, 4}}, {{3, 4, 7}}
    }};
    std::vector<SliceTriangle> triangles;
    triangles.reserve(faces.size());
    for (const std::array<int, 3>& face : faces)
    {
        SliceTriangle triangle{};
        for (int corner = 0; corner < 3; ++corner)
            triangle.vertices[corner] = vertices[face[corner]];
        triangles.push_back(triangle);
    }
    return triangles;
}

bool AllCoordinatesFinite(const std::vector<SliceTriangle>& triangles)
{
    for (const SliceTriangle& triangle : triangles)
        for (const std::array<double, 3>& vertex : triangle.vertices)
            for (double value : vertex)
                if (!std::isfinite(value))
                    return false;
    return true;
}

struct MeshBounds
{
    std::array<double, 3> min{{0.0, 0.0, 0.0}};
    std::array<double, 3> max{{0.0, 0.0, 0.0}};
    bool valid = false;
};

MeshBounds ComputeBounds(const std::vector<SliceTriangle>& triangles)
{
    MeshBounds bounds;
    for (const SliceTriangle& triangle : triangles)
        for (const std::array<double, 3>& vertex : triangle.vertices)
        {
            if (!bounds.valid)
            {
                bounds.min = vertex;
                bounds.max = vertex;
                bounds.valid = true;
                continue;
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                if (vertex[axis] < bounds.min[axis])
                    bounds.min[axis] = vertex[axis];
                if (vertex[axis] > bounds.max[axis])
                    bounds.max[axis] = vertex[axis];
            }
        }
    return bounds;
}

std::string DescribeBounds(const MeshBounds& bounds)
{
    if (!bounds.valid)
        return "empty";
    return "[" + Format(bounds.min[0]) + ", " + Format(bounds.min[1]) + ", " +
           Format(bounds.min[2]) + "] .. [" + Format(bounds.max[0]) + ", " +
           Format(bounds.max[1]) + "]";
}

void CheckLocalWorldCoordinates()
{
    const std::vector<SliceTriangle> world = MakeBox(9.0, -6.0, 1.0, 11.0, -4.0, 3.0);
    const double center_x = 10.0;
    const double center_y = -5.0;

    const std::vector<SliceTriangle> local =
        SupportLocalTriangles(world, center_x, center_y);
    Require(local.size() == world.size(),
            "local conversion changed the triangle count");
    bool local_ok = local.size() == world.size();
    for (std::size_t index = 0; local_ok && index < world.size(); ++index)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis)
            {
                const double offset =
                    axis == 0 ? center_x : (axis == 1 ? center_y : 0.0);
                const double expected = world[index].vertices[corner][axis] - offset;
                if (std::abs(local[index].vertices[corner][axis] - expected) > 1.0e-12)
                    local_ok = false;
            }
    Require(local_ok, "local conversion did not subtract the model XY center");

    const std::vector<SliceTriangle> roundtrip =
        SupportWorldTriangles(local, center_x, center_y);
    Require(roundtrip.size() == world.size(),
            "world conversion changed the triangle count");
    bool roundtrip_ok = roundtrip.size() == world.size();
    for (std::size_t index = 0; roundtrip_ok && index < world.size(); ++index)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis)
                if (std::abs(roundtrip[index].vertices[corner][axis] -
                             world[index].vertices[corner][axis]) > 1.0e-12)
                    roundtrip_ok = false;
    Require(roundtrip_ok, "local/world round trip did not restore the input");

    const std::vector<SliceTriangle> identity =
        SupportLocalTriangles(world, 0.0, 0.0);
    Require(identity.size() == world.size(),
            "identity local conversion changed the triangle count");
    bool identity_ok = identity.size() == world.size();
    for (std::size_t index = 0; identity_ok && index < world.size(); ++index)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis)
                if (identity[index].vertices[corner][axis] !=
                    world[index].vertices[corner][axis])
                    identity_ok = false;
    Require(identity_ok, "zero center conversion must keep coordinates unchanged");

    std::cout << "coordinate helpers: " << world.size()
              << " triangles converted to local and back\n";
}

void CheckMeshVolume()
{
    Require(SupportMeshVolumeMm3({}) == 0.0,
            "volume of an empty triangle list must be 0");

    const std::vector<SliceTriangle> unit_cube = MakeBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
    Require(std::abs(SupportMeshVolumeMm3(unit_cube) - 1.0) <= 1.0e-9,
            "unit cube volume is " + Format(SupportMeshVolumeMm3(unit_cube)) +
                ", expected 1");

    const std::vector<SliceTriangle> moved_cube =
        MakeBox(5.0, -3.0, 2.0, 6.0, -2.0, 3.0);
    Require(std::abs(SupportMeshVolumeMm3(moved_cube) - 1.0) <= 1.0e-9,
            "translation changed the cube volume to " +
                Format(SupportMeshVolumeMm3(moved_cube)));

    const std::vector<SliceTriangle> plate =
        MakeBox(-20.0, -20.0, 20.0, 20.0, 20.0, 22.0);
    Require(std::abs(SupportMeshVolumeMm3(plate) - 3200.0) <= 1.0e-6,
            "40x40x2 plate volume is " + Format(SupportMeshVolumeMm3(plate)) +
                ", expected 3200");

    std::vector<SliceTriangle> broken = unit_cube;
    broken[0].vertices[0][0] = std::numeric_limits<double>::quiet_NaN();
    Require(SupportMeshVolumeMm3(broken) == 0.0,
            "volume with NaN coordinates must be 0");
    broken = unit_cube;
    broken[3].vertices[2][2] = std::numeric_limits<double>::infinity();
    Require(SupportMeshVolumeMm3(broken) == 0.0,
            "volume with infinite coordinates must be 0");

    std::cout << "mesh volume: unit cube 1 mm^3, translation invariant, "
                 "non-finite input yields 0\n";
}

void CheckEmptyModelNeedsNoSupport()
{
    SupportModelInput input;
    input.name = "empty";
    SupportGenerationOutput output;
    output.state = SupportState::Generated; // must be reset by the engine
    output.triangle_count = 7;
    std::string error = "stale error";

    const bool ok = GenerateSupportForModel(input, output, error);
    Require(ok, "empty model generation should succeed, got: " + error);
    Require(output.state == SupportState::NotNeeded,
            "empty model should report NotNeeded");
    Require(output.local_triangles.empty(),
            "empty model should not produce support triangles");
    Require(output.triangle_count == 0, "empty model triangle count should be 0");
    Require(output.volume_mm3 == 0.0, "empty model volume should be 0");
    Require(error.empty(), "successful generation should clear the error: " + error);

    std::cout << "empty model: NotNeeded, no triangles\n";
}

void CheckHoveringPlateGeneratesSupports()
{
    // 40x40x2 mm plate hovering 20 mm above the build plate. The pyslm_cpp PoC
    // measured volume 28791.99 mm^3 and 1266 supportVolume triangles for it.
    SupportModelInput input;
    input.name = "hover_plate";
    input.world_triangles_mm = MakeBox(-20.0, -20.0, 20.0, 20.0, 20.0, 22.0);
    input.model_center_x_mm = 0.0;
    input.model_center_y_mm = 0.0;

    SupportGenerationOutput output;
    std::string error;
    const bool ok = GenerateSupportForModel(input, output, error);
    if (!ok)
    {
        std::cerr << "[FAIL] hovering plate support generation failed: " << error
                  << '\n';
        ++g_failures;
        return;
    }

    const MeshBounds bounds = ComputeBounds(output.local_triangles);
    std::cout << "hovering plate: state="
              << (output.state == SupportState::Generated ? "Generated" : "other")
              << ", triangles=" << output.triangle_count
              << ", volume_mm3=" << Format(output.volume_mm3)
              << ", bounds=" << DescribeBounds(bounds) << '\n';

    Require(output.state == SupportState::Generated,
            "hovering plate should generate supports, got state " +
                std::to_string(static_cast<int>(output.state)));
    Require(!output.local_triangles.empty(),
            "hovering plate support triangles must not be empty");
    Require(output.triangle_count == output.local_triangles.size(),
            "triangle_count does not match local_triangles size");
    Require(AllCoordinatesFinite(output.local_triangles),
            "hovering plate support contains non-finite coordinates");
    Require(output.volume_mm3 > 0.0,
            "hovering plate support volume must be positive");
    Require(bounds.valid && bounds.min[2] >= -0.5,
            "support mesh reaches below the build plate: min z " +
                Format(bounds.min[2]));
    Require(bounds.valid && bounds.max[2] <= 21.0,
            "support mesh rises above the overhang face: max z " +
                Format(bounds.max[2]));
    Require(std::abs(output.volume_mm3 - 28791.99) <= 0.10 * 28791.99,
            "hovering plate support volume " + Format(output.volume_mm3) +
                " deviates more than 10% from the PoC value 28791.99");
    Require(output.triangle_count >= 100,
            "hovering plate support triangle count " +
                std::to_string(output.triangle_count) + " is unexpectedly small");
    Require(bounds.valid && bounds.min[0] >= -25.0 && bounds.max[0] <= 25.0 &&
                bounds.min[1] >= -25.0 && bounds.max[1] <= 25.0,
            "support footprint is not under the 40x40 plate: " +
                DescribeBounds(bounds));
}

void CheckOnPlateModelNeedsNoSupport()
{
    // A cube sitting on the build plate has no overhang: pyslm returns a single
    // degenerate support block (volume 0, empty supportVolume) that must be
    // filtered into SupportState::NotNeeded.
    SupportModelInput input;
    input.name = "plate_cube";
    input.world_triangles_mm = MakeBox(-5.0, -5.0, 0.0, 5.0, 5.0, 10.0);
    input.model_center_x_mm = 0.0;
    input.model_center_y_mm = 0.0;

    SupportGenerationOutput output;
    std::string error;
    const bool ok = GenerateSupportForModel(input, output, error);
    if (!ok)
    {
        std::cerr << "[FAIL] on-plate cube support generation failed: " << error
                  << '\n';
        ++g_failures;
        return;
    }

    std::cout << "on-plate cube: state="
              << (output.state == SupportState::NotNeeded ? "NotNeeded"
                                                          : "other")
              << ", triangles=" << output.triangle_count << '\n';
    Require(output.state == SupportState::NotNeeded,
            "cube resting on the build plate should report NotNeeded");
    Require(output.local_triangles.empty(),
            "cube resting on the build plate should not need supports");
    Require(output.volume_mm3 == 0.0,
            "cube resting on the build plate should have zero support volume");
}

void CheckLocalCoordinatesFollowModelCenter()
{
    // Same hovering plate, but translated to model center (20, -10). The local
    // triangles must be centered on that model center so the viewport can move
    // the supports with the model.
    SupportModelInput input;
    input.name = "hover_plate_offset";
    input.world_triangles_mm = MakeBox(0.0, -30.0, 20.0, 40.0, 10.0, 22.0);
    input.model_center_x_mm = 20.0;
    input.model_center_y_mm = -10.0;

    SupportGenerationOutput output;
    std::string error;
    const bool ok = GenerateSupportForModel(input, output, error);
    if (!ok)
    {
        std::cerr << "[FAIL] offset hovering plate generation failed: " << error
                  << '\n';
        ++g_failures;
        return;
    }

    const MeshBounds bounds = ComputeBounds(output.local_triangles);
    std::cout << "offset hovering plate: state="
              << (output.state == SupportState::Generated ? "Generated" : "other")
              << ", triangles=" << output.triangle_count
              << ", volume_mm3=" << Format(output.volume_mm3)
              << ", local bounds=" << DescribeBounds(bounds) << '\n';

    Require(output.state == SupportState::Generated,
            "offset hovering plate should generate supports");
    Require(!output.local_triangles.empty(),
            "offset hovering plate support triangles must not be empty");
    Require(AllCoordinatesFinite(output.local_triangles),
            "offset hovering plate support contains non-finite coordinates");
    Require(bounds.valid && bounds.min[2] >= -0.5,
            "offset support mesh reaches below the build plate");
    // The plate spans local X/Y [-20, 20]; the support footprint must be in
    // local coordinates around the model center, not in world coordinates
    // (world coordinates would be X [0, 40] and Y [-30, 10]).
    Require(bounds.valid && bounds.min[0] >= -25.0 && bounds.max[0] <= 25.0 &&
                bounds.min[1] >= -25.0 && bounds.max[1] <= 25.0,
            "offset support footprints are not in local coordinates: " +
                DescribeBounds(bounds));
    Require(output.volume_mm3 > 0.0,
            "offset hovering plate support volume must be positive");
}
} // namespace

int main()
{
    std::cout << "pyslm version: " << pyslm::version() << '\n';

    CheckLocalWorldCoordinates();
    CheckMeshVolume();
    CheckEmptyModelNeedsNoSupport();
    CheckHoveringPlateGeneratesSupports();
    CheckOnPlateModelNeedsNoSupport();
    CheckLocalCoordinatesFollowModelCenter();

    if (g_failures != 0)
    {
        std::cerr << g_failures << " support test check(s) failed\n";
        return 1;
    }
    std::cout << "PASSED\n";
    return 0;
}
