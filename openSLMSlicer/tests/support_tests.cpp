#include "support_engine.h"

#include <pyslm/types.hpp>

#include <array>
#include <chrono>
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

std::vector<SliceTriangle> CombineBlocks(const SupportGenerationOutput& output)
{
    std::vector<SliceTriangle> combined;
    for (const SupportGenerationBlock& block : output.blocks)
        combined.insert(combined.end(), block.local_triangles.begin(),
                        block.local_triangles.end());
    return combined;
}

double TotalVolume(const SupportGenerationOutput& output)
{
    double volume = 0.0;
    for (const SupportGenerationBlock& block : output.blocks)
        volume += block.volume_mm3;
    return volume;
}

std::size_t TotalTriangles(const SupportGenerationOutput& output)
{
    std::size_t count = 0;
    for (const SupportGenerationBlock& block : output.blocks)
        count += block.triangle_count;
    return count;
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
    output.blocks.push_back({});            // must be reset by the engine
    std::string error = "stale error";

    const bool ok = GenerateSupportForModel(input, output, error);
    Require(ok, "empty model generation should succeed, got: " + error);
    Require(output.state == SupportState::NotNeeded,
            "empty model should report NotNeeded");
    Require(output.blocks.empty(),
            "empty model should not produce support blocks");
    Require(error.empty(), "successful generation should clear the error: " + error);

    std::cout << "empty model: NotNeeded, no blocks\n";
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

    const std::vector<SliceTriangle> local = CombineBlocks(output);
    const MeshBounds bounds = ComputeBounds(local);
    const double volume = TotalVolume(output);
    const std::size_t triangles = TotalTriangles(output);
    std::cout << "hovering plate: state="
              << (output.state == SupportState::Generated ? "Generated" : "other")
              << ", blocks=" << output.blocks.size()
              << ", triangles=" << triangles
              << ", volume_mm3=" << Format(volume)
              << ", bounds=" << DescribeBounds(bounds) << '\n';

    Require(output.state == SupportState::Generated,
            "hovering plate should generate supports, got state " +
                std::to_string(static_cast<int>(output.state)));
    Require(output.blocks.size() == 1,
            "one hovering plate should produce exactly one support block, got " +
                std::to_string(output.blocks.size()));
    Require(!local.empty(),
            "hovering plate support triangles must not be empty");
    for (const SupportGenerationBlock& block : output.blocks)
        Require(block.triangle_count == block.local_triangles.size(),
                "block triangle_count does not match local_triangles size");
    Require(AllCoordinatesFinite(local),
            "hovering plate support contains non-finite coordinates");
    Require(volume > 0.0,
            "hovering plate support volume must be positive");
    Require(bounds.valid && bounds.min[2] >= -0.5,
            "support mesh reaches below the build plate: min z " +
                Format(bounds.min[2]));
    Require(bounds.valid && bounds.max[2] <= 21.0,
            "support mesh rises above the overhang face: max z " +
                Format(bounds.max[2]));
    Require(std::abs(volume - 28791.99) <= 0.10 * 28791.99,
            "hovering plate support volume " + Format(volume) +
                " deviates more than 10% from the PoC value 28791.99");
    Require(triangles >= 100,
            "hovering plate support triangle count " +
                std::to_string(triangles) + " is unexpectedly small");
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
              << ", blocks=" << output.blocks.size() << '\n';
    Require(output.state == SupportState::NotNeeded,
            "cube resting on the build plate should report NotNeeded");
    Require(output.blocks.empty(),
            "cube resting on the build plate should not need supports");
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

    const std::vector<SliceTriangle> local = CombineBlocks(output);
    const MeshBounds bounds = ComputeBounds(local);
    const double volume = TotalVolume(output);
    const std::size_t triangles = TotalTriangles(output);
    std::cout << "offset hovering plate: state="
              << (output.state == SupportState::Generated ? "Generated" : "other")
              << ", blocks=" << output.blocks.size()
              << ", triangles=" << triangles
              << ", volume_mm3=" << Format(volume)
              << ", local bounds=" << DescribeBounds(bounds) << '\n';

    Require(output.state == SupportState::Generated,
            "offset hovering plate should generate supports");
    Require(!local.empty(),
            "offset hovering plate support triangles must not be empty");
    Require(AllCoordinatesFinite(local),
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
    Require(volume > 0.0,
            "offset hovering plate support volume must be positive");
}

void CheckMultipleSupportBlocks()
{
    // A single model with two disconnected hovering plates must produce two
    // independent support blocks, so the UI can list/show/hide them separately.
    SupportModelInput input;
    input.name = "two_plates";
    std::vector<SliceTriangle> triangles =
        MakeBox(-40.0, -10.0, 20.0, -20.0, 10.0, 22.0);
    const std::vector<SliceTriangle> second =
        MakeBox(20.0, -10.0, 20.0, 40.0, 10.0, 22.0);
    triangles.insert(triangles.end(), second.begin(), second.end());
    input.world_triangles_mm = std::move(triangles);
    input.model_center_x_mm = 0.0;
    input.model_center_y_mm = 0.0;

    SupportGenerationOutput output;
    std::string error;
    const bool ok = GenerateSupportForModel(input, output, error);
    if (!ok)
    {
        std::cerr << "[FAIL] two-plate support generation failed: " << error
                  << '\n';
        ++g_failures;
        return;
    }

    std::cout << "two plates: state="
              << (output.state == SupportState::Generated ? "Generated" : "other")
              << ", blocks=" << output.blocks.size()
              << ", triangles=" << TotalTriangles(output)
              << ", volume_mm3=" << Format(TotalVolume(output)) << '\n';

    Require(output.state == SupportState::Generated,
            "two hovering plates should generate supports");
    Require(output.blocks.size() == 2,
            "two disconnected overhang regions should yield two support blocks, got " +
                std::to_string(output.blocks.size()));
    if (output.blocks.size() != 2)
        return;
    for (const SupportGenerationBlock& block : output.blocks)
    {
        Require(!block.local_triangles.empty(),
                "support block geometry must not be empty");
        Require(block.volume_mm3 > 0.0,
                "support block volume must be positive");
        Require(AllCoordinatesFinite(block.local_triangles),
                "support block contains non-finite coordinates");
    }
    const MeshBounds first_bounds = ComputeBounds(output.blocks[0].local_triangles);
    const MeshBounds second_bounds = ComputeBounds(output.blocks[1].local_triangles);
    Require(first_bounds.valid && second_bounds.valid,
            "support block bounds are invalid");
    Require(first_bounds.max[0] < second_bounds.min[0] ||
                second_bounds.max[0] < first_bounds.min[0],
            "the two support blocks overlap in X instead of being separate");
}
void CheckSplitTriangleComponents()
{
    // Empty soup has no components.
    Require(SplitTriangleComponents({}).empty(),
            "splitting an empty triangle soup must return no components");

    // A single closed box stays one component, in input order and coordinates.
    const std::vector<SliceTriangle> single_box =
        MakeBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
    const std::vector<std::vector<SliceTriangle>> single_components =
        SplitTriangleComponents(single_box);
    Require(single_components.size() == 1,
            "a single box must be one component, got " +
                std::to_string(single_components.size()));
    if (single_components.size() == 1)
    {
        Require(single_components[0].size() == single_box.size(),
                "single box component lost triangles");
        Require(std::abs(SupportMeshVolumeMm3(single_components[0]) - 1.0) <=
                    1.0e-9,
                "single box component volume is " +
                    Format(SupportMeshVolumeMm3(single_components[0])) +
                    ", expected 1");
        bool order_ok = single_components[0].size() == single_box.size();
        for (std::size_t index = 0; order_ok && index < single_box.size(); ++index)
            for (int corner = 0; corner < 3; ++corner)
                for (int axis = 0; axis < 3; ++axis)
                    if (single_components[0][index].vertices[corner][axis] !=
                        single_box[index].vertices[corner][axis])
                        order_ok = false;
        Require(order_ok,
                "component splitting must keep the input triangle order and "
                "coordinates");
    }

    // Two boxes that share no vertex split into two components ordered by their
    // bounding box minimum X (the near box is inserted first here).
    std::vector<SliceTriangle> far_boxes =
        MakeBox(10.0, 0.0, 0.0, 11.0, 1.0, 1.0);
    const std::vector<SliceTriangle> near_box =
        MakeBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
    far_boxes.insert(far_boxes.begin(), near_box.begin(), near_box.end());
    const std::vector<std::vector<SliceTriangle>> far_components =
        SplitTriangleComponents(far_boxes);
    Require(far_components.size() == 2,
            "two separate boxes must split into two components, got " +
                std::to_string(far_components.size()));
    if (far_components.size() == 2)
    {
        Require(far_components[0].size() == 12 && far_components[1].size() == 12,
                "each separated box component must keep its 12 triangles");
        const MeshBounds first_bounds = ComputeBounds(far_components[0]);
        const MeshBounds second_bounds = ComputeBounds(far_components[1]);
        Require(first_bounds.valid && second_bounds.valid,
                "split component bounds are invalid");
        Require(first_bounds.min[0] <= 1.0e-9 &&
                    second_bounds.min[0] >= 10.0 - 1.0e-9,
                "split components must be ordered by bounding box minimum X");
        Require(std::abs(SupportMeshVolumeMm3(far_components[0]) - 1.0) <=
                        1.0e-9 &&
                    std::abs(SupportMeshVolumeMm3(far_components[1]) - 1.0) <=
                        1.0e-9,
                "separated box component volume must stay 1 mm^3");
    }

    // Two boxes sharing a whole face weld into one component through the shared
    // vertices.
    std::vector<SliceTriangle> welded_boxes =
        MakeBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
    const std::vector<SliceTriangle> right_box =
        MakeBox(1.0, 0.0, 0.0, 2.0, 1.0, 1.0);
    welded_boxes.insert(welded_boxes.end(), right_box.begin(), right_box.end());
    const std::vector<std::vector<SliceTriangle>> welded_components =
        SplitTriangleComponents(welded_boxes);
    Require(welded_components.size() == 1,
            "boxes sharing a face must weld into one component, got " +
                std::to_string(welded_components.size()));
    if (welded_components.size() == 1)
    {
        Require(welded_components[0].size() == 24,
                "welded component must keep all 24 triangles");
        Require(std::abs(SupportMeshVolumeMm3(welded_components[0]) - 2.0) <=
                    1.0e-9,
                "welded two-box component volume must be 2 mm^3, got " +
                    Format(SupportMeshVolumeMm3(welded_components[0])));
    }

    // Exactly coincident duplicated boxes also weld into a single component.
    std::vector<SliceTriangle> duplicated_boxes =
        MakeBox(-1.0, -1.0, -1.0, 0.0, 0.0, 0.0);
    const std::vector<SliceTriangle> duplicate_box =
        MakeBox(-1.0, -1.0, -1.0, 0.0, 0.0, 0.0);
    duplicated_boxes.insert(duplicated_boxes.end(), duplicate_box.begin(),
                            duplicate_box.end());
    Require(SplitTriangleComponents(duplicated_boxes).size() == 1,
            "coincident duplicated boxes must weld into one component");

    // A gap larger than the 1e-6 mm weld tolerance keeps the boxes separate.
    std::vector<SliceTriangle> gap_boxes =
        MakeBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
    const std::vector<SliceTriangle> shifted_box =
        MakeBox(1.001, 0.0, 0.0, 2.001, 1.0, 1.0);
    gap_boxes.insert(gap_boxes.end(), shifted_box.begin(), shifted_box.end());
    Require(SplitTriangleComponents(gap_boxes).size() == 2,
            "boxes separated by more than the weld tolerance must stay "
            "separate");

    // Linear-time smoke test: thousands of unrelated components must not show
    // quadratic behaviour (the split is a single hash pass plus a sort).
    std::vector<SliceTriangle> many_boxes;
    many_boxes.reserve(3000 * 12);
    for (int index = 0; index < 3000; ++index)
    {
        const double x = static_cast<double>(index) * 3.0;
        const std::vector<SliceTriangle> box =
            MakeBox(x, 0.0, 0.0, x + 1.0, 1.0, 1.0);
        many_boxes.insert(many_boxes.end(), box.begin(), box.end());
    }
    const auto split_start = std::chrono::steady_clock::now();
    const std::vector<std::vector<SliceTriangle>> many_components =
        SplitTriangleComponents(many_boxes);
    const auto split_end = std::chrono::steady_clock::now();
    const double split_ms =
        std::chrono::duration<double, std::milli>(split_end - split_start)
            .count();
    Require(many_components.size() == 3000,
            "3000 separate boxes must split into 3000 components, got " +
                std::to_string(many_components.size()));

    std::cout << "component split: single box 1, far boxes 2, welded face 1, "
                 "duplicated box 1, gap 2, 3000 disjoint boxes -> "
              << many_components.size() << " components in " << Format(split_ms)
              << " ms\n";
}
} // namespace

int main()
{
    std::cout << "pyslm version: " << pyslm::version() << '\n';

    CheckLocalWorldCoordinates();
    CheckMeshVolume();
    CheckSplitTriangleComponents();
    CheckEmptyModelNeedsNoSupport();
    CheckHoveringPlateGeneratesSupports();
    CheckOnPlateModelNeedsNoSupport();
    CheckLocalCoordinatesFollowModelCenter();
    CheckMultipleSupportBlocks();

    if (g_failures != 0)
    {
        std::cerr << g_failures << " support test check(s) failed\n";
        return 1;
    }
    std::cout << "PASSED\n";
    return 0;
}
