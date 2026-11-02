#pragma once
#include "core.hpp"
#include <optional>
#include <string>

namespace pyslm::support {
using Path3D=std::vector<Vec3>;
using Edge=std::array<std::uint32_t,2>;
struct Triangulation { Path vertices; std::vector<Triangle> faces; };
struct StrutIntersection { bool intersects=false; Path3D locations; };
// Value representation of the actual PolyTree polygon/isHole/children fields.
struct PolygonTree { Path polygon; bool isHole=false; std::vector<PolygonTree> children; };

PYSLM_API std::vector<Vec3> meshFaceNormals(const Mesh& mesh);
PYSLM_API std::vector<Vec3> meshVertexNormals(const Mesh& mesh);
PYSLM_API std::vector<double> meshFaceAreas(const Mesh& mesh);
PYSLM_API double meshVolume(const Mesh& mesh);
PYSLM_API Bounds3 meshBounds(const Mesh& mesh);
// Trimesh hashable edge order: high endpoint, then low endpoint.
PYSLM_API std::vector<Edge> meshUniqueEdges(const Mesh& mesh);
PYSLM_API std::vector<std::array<std::size_t,2>> meshFaceAdjacency(const Mesh& mesh);
PYSLM_API std::vector<Edge> meshFaceAdjacencyEdges(const Mesh& mesh);
PYSLM_API Mesh processMesh(const Mesh& mesh,bool validate=false);
PYSLM_API Mesh selectMeshFaces(const Mesh& mesh,const std::vector<std::size_t>& faces,bool process=true);
PYSLM_API std::vector<Mesh> splitMeshComponents(const Mesh& mesh);
PYSLM_API std::vector<Path3D> meshOutline(const Mesh& mesh,int digits=8);

PYSLM_API Triangulation triangulateShapelyPolygon(const Polygon& polygon,const std::string& triangleArgs="p");
PYSLM_API Triangulation triangulatePolygonFromPaths(const Path& exterior,const Paths& interiors,const std::string& triangleArgs="p");
PYSLM_API Triangulation triangulatePolygonEarcut(const Polygons& polygons,bool isClosed=false);
PYSLM_API Mesh extrudeTriangulation(const Path& vertices,const std::vector<Triangle>& faces,double height);
PYSLM_API Mesh extrudeFace(const Mesh& mesh,std::optional<double> height=std::nullopt,const std::vector<double>& heightArray={});
PYSLM_API Mesh sweepPolygon(const Polygon& polygon,const Path3D& path,const std::vector<double>& angles={},const std::vector<double>& scaleFactors={},const std::string& triangleArgs="p");
PYSLM_API Mesh boolUnion(const Mesh& a,const Mesh& b);
PYSLM_API Mesh boolIntersect(const Mesh& a,const Mesh& b);
PYSLM_API Mesh boolDiff(const Mesh& a,const Mesh& b);
// Original function explicitly rejects this operation with Manifold.
PYSLM_API Mesh resolveIntersection(const Mesh& mesh);
PYSLM_API Polygon generatePolygonBoundingBox(const Bounds3& bounds);
PYSLM_API Polygons flattenSupportRegion(const Mesh& mesh);
PYSLM_API Paths path2DToPathList(const Polygons& polygons);
PYSLM_API Paths createPath2DfromPaths(const Paths& paths);
PYSLM_API std::pair<Paths,Paths> sortExteriorInteriorRings(const Polygons& polygons,bool closePolygon=false);
PYSLM_API std::pair<Paths,Paths> sortExteriorInteriorRings(const PolygonTree& tree,bool closePolygon=false);
PYSLM_API StrutIntersection checkStrutCylinderIntersection(Vec3 a,Vec3 b,double radius,const Mesh& mesh,int numPoints=6,bool centreOnly=false);
// Rays return the nearest nonnegative-distance location per input ray.
PYSLM_API std::optional<Vec3> firstRayIntersection(const Mesh& mesh,Vec3 origin,Vec3 direction);
} // namespace pyslm::support
