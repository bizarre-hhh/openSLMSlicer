#pragma once

#include "types.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace pyslm {

struct Polygon { Path exterior; Paths interiors; };
using Polygons=std::vector<Polygon>;
enum class Geometry2DType : int { Point=0,LineString=1,LinearRing=2,Polygon=3,MultiPoint=4,MultiLineString=5,MultiPolygon=6,GeometryCollection=7 };
struct Geometry2D {
    Geometry2DType type=Geometry2DType::GeometryCollection;
    Path coords; // Point / LineString / LinearRing
    Polygon polygon;
    std::vector<Geometry2D> parts;
};
struct SliceOptions {
    bool fixPolygons=true;
    std::optional<double> simplificationFactor;
    bool simplificationPreserveTopology=true;
    std::string simplificationFactorMode="absolute";
};
struct Bitmap {
    std::size_t width=0,height=0;
    std::vector<std::uint8_t> pixels; // row-major, y increasing with source Y, 0 or 1
    std::uint8_t at(std::size_t x,std::size_t y) const { return pixels.at(y*width+x); }
};
struct PlanarSection {
    Path vertices;
    std::vector<Vec3> vertices3D; // representatives selected before planar projection
    std::vector<std::array<std::uint32_t,2>> edges;
    std::vector<std::vector<std::uint32_t>> entities;
    Paths paths; // closed, CCW discrete paths in original entity order
    std::vector<std::array<Vec3,2>> rawLines;
    std::vector<std::size_t> faceIndex;
    bool isClosed=false;
    bool gapRepairApplied=false;
    double z=0.0;
    Matrix4 to3D{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
};

class PYSLM_API DocumentObject {
public:
    explicit DocumentObject(std::string name);
    virtual ~DocumentObject()=default;
    std::string name,label="Document Object";
    std::vector<std::shared_ptr<DocumentObject>> attributes;
    void setName(const std::string& value) { name=value; }
    virtual Bounds3 boundingBox() const=0;
    virtual Vec3 extents() const=0;
    virtual bool usesBoundaryLayers() const { return false; }
    virtual void setRequiresBoundaryLayers() {}
};

class PYSLM_API Part : public DocumentObject {
public:
    static constexpr double POLYGON_FIX_EPSILON=1e-3;
    explicit Part(std::string name);
    Vec3 origin{0,0,0},rotation{0,0,0},scaleFactor{1,1,1};
    std::string partType="Part";
    std::string toString() const;
    bool isDirty() const noexcept;
    void setGeometry(const std::string& filename,bool fixGeometry=true,bool mergeVertices=true);
    void setGeometry(const Mesh& mesh,bool fixGeometry=true,bool mergeVertices=true);
    void setGeometryByMesh(const Mesh& mesh);
    bool hasGeometry() const noexcept { return hasGeometry_; }
    const Mesh& geometry() const;
    void regenerate() const;
    Matrix4 getTransform() const;
    Bounds3 boundingBox() const override;
    Vec3 extents() const override;
    double volume() const;
    double surfaceArea() const;
    bool checkGeometry() const;
    void dropToPlatform(double zPos=0.0);
    Path getProjectedHull() const;
    Polygon getProjectedHullPolygon() const;
    Polygons getProjectedArea() const;
    Geometry2D getProjectedGeometry() const;
    std::optional<PlanarSection> getTrimeshSlice(double z) const;
    Paths getVectorSlice(double z,const SliceOptions& options={}) const;
    Polygons getVectorSlicePolygons(double z,const SliceOptions& options={}) const;
    Bitmap getBitmapSlice(double z,double resolution,std::optional<Vec2> origin=std::nullopt) const;
    static Paths path2DToPathList(const Polygons& shapes);
private:
    Mesh source_;
    bool hasGeometry_=false;
    mutable Mesh transformed_;
    mutable bool dirty_=true;
    mutable Vec3 cachedOrigin_{},cachedRotation_{},cachedScale_{1,1,1};
};

class PYSLM_API Document {
public:
    void addObject(std::shared_ptr<DocumentObject> object);
    void recalculateDocument();
    std::vector<std::shared_ptr<DocumentObject>> getDependencyList() const;
    std::shared_ptr<DocumentObject> head() const;
    std::vector<std::shared_ptr<Part>> parts() const;
    Bounds3 boundingBox() const;
    Bounds3 partBoundingBox() const;
    Vec3 extents() const;
    Vec3 partExtents() const;
    // Graph export is consumable by a GUI without embedding a plotting library.
    std::string drawNetworkGraph() const;
    template<class T> std::vector<std::shared_ptr<T>> getObjectsByType() const {
        static_assert(std::is_base_of_v<DocumentObject,T>);
        std::vector<std::shared_ptr<T>> result;
        for(const auto& object:objects_) if(typeid(*object)==typeid(T)) result.push_back(std::static_pointer_cast<T>(object));
        return result;
    }
private:
    std::vector<std::shared_ptr<DocumentObject>> objects_;
    std::vector<std::array<std::size_t,2>> dependencies_;
};

PYSLM_API Mesh loadSTL(const std::string& filename);
PYSLM_API void mergeMeshVertices(Mesh& mesh,int digits=8);
PYSLM_API Matrix4 planeTransform(Vec3 origin,Vec3 normal);
PYSLM_API std::pair<std::vector<std::array<Vec3,2>>,std::vector<std::size_t>>
    meshPlane(const Mesh& mesh,Vec3 origin,Vec3 normal);
PYSLM_API std::optional<PlanarSection> sectionMesh(const Mesh& mesh,Vec3 origin,Vec3 normal);
PYSLM_API PlanarSection pathFromLines2D(const std::vector<std::array<Vec2,2>>& lines,int digits=5);

} // namespace pyslm
