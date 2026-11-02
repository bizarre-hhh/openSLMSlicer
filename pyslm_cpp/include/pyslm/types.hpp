#pragma once

#include "export.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace pyslm {

struct Vec2 {
    double x = 0.0, y = 0.0;
    double& operator[](std::size_t i) { return i == 0 ? x : y; }
    double operator[](std::size_t i) const { return i == 0 ? x : y; }
};
struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    double& operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : z); }
    double operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x+b.x, a.y+b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x-b.x, a.y-b.y}; }
inline Vec2 operator*(Vec2 a, double s) { return {a.x*s, a.y*s}; }
inline Vec2 operator/(Vec2 a, double s) { return {a.x/s, a.y/s}; }
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x*s, a.y*s, a.z*s}; }
inline Vec3 operator/(Vec3 a, double s) { return {a.x/s, a.y/s, a.z/s}; }
inline double dot(Vec2 a, Vec2 b) { return a.x*b.x+a.y*b.y; }
inline double dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline double norm(Vec2 a) { return std::hypot(a.x,a.y); }
inline double norm(Vec3 a) { return std::sqrt(dot(a,a)); }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
using Path = std::vector<Vec2>;
using Paths = std::vector<Path>;
using Bounds2 = std::array<double,4>;
using Bounds3 = std::array<double,6>;
using Matrix4 = std::array<double,16>;
using HatchLine = std::array<Vec3,2>;
using HatchLines = std::vector<HatchLine>;
using Triangle = std::array<std::uint32_t,3>;

struct Mesh {
    std::vector<Vec3> vertices;
    std::vector<Triangle> faces;
    std::map<std::string,std::vector<double>> faceAttributes;
};

enum class LaserMode : int { CW=0, Pulse=1 };
enum class LayerGeometryType : int { Invalid=0, Polygon=1, Hatch=2, Pnts=3 };
enum class ScanMode : int { Default=0, ContourFirst=1, HatchFirst=2 };

struct Header {
    std::string filename;
    std::array<int,2> version{0,0};
    int zUnit=1000;
};
struct BuildStyle {
    std::string name, description;
    int bid=0;
    double laserPower=0.0, laserSpeed=0.0, laserFocus=0.0;
    int laserId=1;
    LaserMode laserMode=LaserMode::Pulse;
    int pointDistance=0, pointExposureTime=0, pointDelay=0, jumpDelay=0;
    double jumpSpeed=0.0;
    // Optional dynamic-language identity for strict reference/import validation.
    // Unset means ordinary C++ numeric validation; false preserves Python float.
    std::optional<bool> jumpSpeedSourceIsInteger;
    PYSLM_API void setStyle(int id, int focus, double power, int exposureTime,
        int exposureDistance, double speed=0.0, int laser=1,
        LaserMode mode=LaserMode::Pulse, const std::string& styleName={},
        const std::string& styleDescription={});
};
struct Model {
    explicit Model(int modelId=0):mid(modelId){}
    int mid=0, topLayerId=0;
    std::string name, buildStyleDescription, buildStyleName;
    std::vector<BuildStyle> buildStyles;
};
// Value semantics preserve geometry kind when stored in Layer::geometry.
struct LayerGeometry {
    LayerGeometryType kind=LayerGeometryType::Invalid;
    int mid=0, bid=0;
    std::string subType;
    Path coords;
    LayerGeometryType type() const noexcept { return kind; }
    PYSLM_API Bounds2 boundingBox() const;
    double numHatches() const noexcept { return static_cast<double>(coords.size())/2.0; }
    std::ptrdiff_t numContours() const noexcept { return static_cast<std::ptrdiff_t>(coords.size())-1; }
    std::size_t numPoints() const noexcept { return coords.size(); }
};
struct HatchGeometry : LayerGeometry {
    HatchGeometry() { kind=LayerGeometryType::Hatch; }
    HatchGeometry(int model, int style, Path points={}) { kind=LayerGeometryType::Hatch; mid=model; bid=style; coords=std::move(points); }
};
struct ContourGeometry : LayerGeometry {
    ContourGeometry() { kind=LayerGeometryType::Polygon; }
    ContourGeometry(int model, int style, Path points={}) { kind=LayerGeometryType::Polygon; mid=model; bid=style; coords=std::move(points); }
};
struct PointsGeometry : LayerGeometry {
    PointsGeometry() { kind=LayerGeometryType::Pnts; }
    PointsGeometry(int model, int style, Path points={}) { kind=LayerGeometryType::Pnts; mid=model; bid=style; coords=std::move(points); }
};
struct Layer {
    std::int64_t z=0;
    int id=0;
    std::string name;
    std::uint64_t layerFilePosition=0;
    std::vector<LayerGeometry> geometry;
    Layer(std::int64_t height=0,int layerId=0):z(height),id(layerId){}
    bool isLoaded() const noexcept { return true; }
    int layerId() const noexcept { return id; }
    void setLayerId(int value) noexcept { id=value; }
    void appendGeometry(const LayerGeometry& value) { geometry.push_back(value); }
    PYSLM_API std::vector<LayerGeometry> getGeometry(ScanMode mode=ScanMode::Default) const;
    PYSLM_API std::vector<LayerGeometry> getContourGeometry() const;
    PYSLM_API std::vector<LayerGeometry> getHatchGeometry() const;
    PYSLM_API std::vector<LayerGeometry> getPointsGeometry() const;
};

PYSLM_API const char* version() noexcept;

} // namespace pyslm
