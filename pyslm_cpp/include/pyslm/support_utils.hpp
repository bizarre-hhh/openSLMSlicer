#pragma once
#include "support_geometry.hpp"

namespace pyslm::support {
struct NumericMatrix {
    std::size_t rows=0,cols=0;
    std::vector<double> values;
    double& at(std::size_t row,std::size_t col) { return values.at(row*cols+col); }
    double at(std::size_t row,std::size_t col) const { return values.at(row*cols+col); }
};
using FaceAdjacency=std::vector<std::pair<std::size_t,std::vector<std::size_t>>>;
struct ProjectionSupportCost { double cost=0.; NumericMatrix heightMap; };
PYSLM_API FaceAdjacency getAdjacentFaces(const Mesh& mesh);
PYSLM_API std::vector<double> getSupportAngles(const Part& part,std::optional<Vec3> unitNormal=std::nullopt,bool useConnectivity=true);
PYSLM_API std::vector<double> getFaceZProjectionWeight(const Mesh& mesh,bool useConnectivity=false);
PYSLM_API Mesh getOverhangMesh(const Part& part,double overhangAngle,bool useConnectivity=false);
PYSLM_API std::vector<Mesh> getOverhangMeshes(const Part& part,double overhangAngle,bool useConnectivity=false);
PYSLM_API double approximateSupportMomentArea(const Part& part,double overhangAngle);
// The original returns per-face areas despite its float annotation/name.
PYSLM_API std::vector<double> getApproximateSupportArea(const Part& part,double overhangAngle,bool projected=false);
// Preserves original NxN broadcast result, not an invented Nx1 correction.
PYSLM_API NumericMatrix approximateSupportMapByCentroid(const Part& part,double overhangAngle,bool includeTriangleVertices=false);
PYSLM_API ProjectionSupportCost approximateProjectionSupportCost(const Part& part,double overhangAngle,bool includeTriangleVertices=false);
// Original raster arrays are X-major: rows=X count, cols=Y count.
PYSLM_API NumericMatrix generateHeightMap(const Mesh& mesh,Vec3 upVec={0,0,1},double resolution=0.5,const Polygons& offsetPolygons={});
PYSLM_API NumericMatrix generateHeightMap2(const Mesh& mesh,Vec3 upVec={0,0,1},double resolution=0.5,const Polygons& offsetPolygons={});
} // namespace pyslm::support
