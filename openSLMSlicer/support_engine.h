#ifndef SUPPORT_ENGINE_H
#define SUPPORT_ENGINE_H

#include "support_types.h"

#include <string>
#include <vector>

// Pure C++ support generation layer built on top of pyslm::support.
//
// The engine works on build-plate millimeter triangles (the same data the slice
// engine consumes). It generates solid block support volumes using pyslm's
// projection/ray-casting support regions (supportVolume), not the unstable truss
// mesh path. Each detected region becomes one independent block so the caller
// can list, show/hide and export them separately. The caller owns model
// iteration, progress, cancellation, persistence and rendering.
struct SupportModelInput
{
    std::string name;
    std::vector<SliceTriangle> world_triangles_mm;
    // Owner model world XY center, used to store supports in local coordinates.
    double model_center_x_mm = 0.0;
    double model_center_y_mm = 0.0;
};

// One independent solid support block (one pyslm support region).
struct SupportGenerationBlock
{
    std::vector<SliceTriangle> local_triangles;
    double volume_mm3 = 0.0;
    std::size_t triangle_count = 0;
};

struct SupportGenerationOutput
{
    SupportState state = SupportState::NotNeeded;
    std::vector<SupportGenerationBlock> blocks;
};

// Generates support geometry for one model.
//
// Returns false when the library or the geometry fails (error receives a
// UTF-8 message). A successful call with state == SupportState::NotNeeded means
// the model does not need supports; state == SupportState::Generated means
// blocks is non-empty (one entry per detected support region).
//
// pyslm's support region identification uses a hidden OpenGL context internally
// on first use, so call this from a process with a usable desktop OpenGL
// environment.
bool GenerateSupportForModel(const SupportModelInput& input,
                             SupportGenerationOutput& output,
                             std::string& error);

// Coordinate helpers shared by the viewport and tests.
std::vector<SliceTriangle> SupportLocalTriangles(
    const std::vector<SliceTriangle>& world_triangles,
    double center_x_mm, double center_y_mm);
std::vector<SliceTriangle> SupportWorldTriangles(
    const std::vector<SliceTriangle>& local_triangles,
    double center_x_mm, double center_y_mm);

// Absolute mesh volume of a closed triangle soup (mm^3). Returns 0 for empty or
// non-finite input.
double SupportMeshVolumeMm3(const std::vector<SliceTriangle>& triangles);

// Splits a triangle soup into connected components welded by shared vertices.
// Vertices are welded when their quantized coordinate (rounded to 1e-6 mm) is
// identical, so triangles touching at a vertex, edge or face end up in the same
// component. Coordinates are copied unchanged and the triangle order inside a
// component is the input order. The component order is deterministic: bounding
// box minimum X, then minimum Y, then minimum Z, then triangle count (smaller
// first), then the original first triangle index as the final tie breaker.
//
// This is used to turn one pyslm support region whose supportVolume contains
// several disconnected solids into one independent block per solid.
std::vector<std::vector<SliceTriangle>> SplitTriangleComponents(
    const std::vector<SliceTriangle>& triangles);

#endif
