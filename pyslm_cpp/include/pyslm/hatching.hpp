#pragma once

#include "types.hpp"
#include <memory>
#include <optional>
#include <utility>

namespace pyslm::hatching {

struct ClosedPolygon {
    Path exterior;
    Paths interiors;
};
PYSLM_API std::vector<ClosedPolygon> pathsToClosedPolygons(const Paths& paths);
PYSLM_API Paths getContoursFromPolygon(const ClosedPolygon& polygon);
PYSLM_API std::pair<Paths,Paths> getContourRingsFromPolygon(const ClosedPolygon& polygon);
PYSLM_API Paths poly2Paths(const ClosedPolygon& polygon);
// Original MultiPolygon branch returns exteriors only; this overload preserves that behavior.
PYSLM_API Paths poly2Paths(const std::vector<ClosedPolygon>& polygons);
PYSLM_API std::vector<ClosedPolygon> simplifyBoundaries(const std::vector<ClosedPolygon>& polygons,
                                                      double tolerance=0.5);

// Flat hatch arrays contain consecutively stored start/end pairs.
PYSLM_API bool isValidHatchArray(const Path& vectors) noexcept;
PYSLM_API HatchLines to3DHatchArray(const Path& vectors);
PYSLM_API Path from3DHatchArray(const HatchLines& vectors);
PYSLM_API std::vector<std::vector<Vec3>> paths2clipper(const Paths& paths);
PYSLM_API Paths clipper2Paths(const std::vector<std::vector<Vec3>>& paths,
                             double scaleFactor, bool close=false);
PYSLM_API Paths simplifyBoundaries(const Paths& paths, double tolerance=0.5);
// Pinned NumPy sorting dispatch, exposed for custom native scan recipes.
PYSLM_API std::vector<std::size_t> numpyArgsort(const std::vector<double>& values);
// Each pair is (intersects, overlaps), matching Shapely GEOS predicates.
PYSLM_API std::vector<std::pair<bool,bool>> classifyIslandBoundaries(const Paths& region,
                                                                 const Paths& islandBoundaries);

class PYSLM_API BaseSort {
public:
    virtual ~BaseSort() = default;
    virtual Path sort(const Path& vectors) const = 0;
};
class PYSLM_API UnidirectionalSort : public BaseSort {
public: Path sort(const Path& vectors) const override;
};
class PYSLM_API FlipSort : public BaseSort {
public: Path sort(const Path& vectors) const override;
};
class PYSLM_API AlternateSort : public BaseSort {
public: Path sort(const Path& vectors) const override;
};
class PYSLM_API HatchDirectionalSort : public BaseSort {
public: Path sort(const Path& vectors) const override;
};
class PYSLM_API ChainSort : public BaseSort {
public:
    std::vector<std::shared_ptr<BaseSort>> sorters;
    explicit ChainSort(std::vector<std::shared_ptr<BaseSort>> chain={});
    Path sort(const Path& vectors) const override;
};
class PYSLM_API LinearSort : public BaseSort {
public:
    double hatchAngle=0.0;
    explicit LinearSort(double angle=0.0):hatchAngle(angle){}
    Path sort(const Path& vectors) const override;
};
class PYSLM_API GreedySort : public BaseSort {
public:
    double hatchAngle=0.0, hatchTol=0.5, clusterDistance=5.0;
    bool sortY=false;
    explicit GreedySort(double angle=0.0, double tolerance=0.5)
        :hatchAngle(angle),hatchTol(tolerance==0.0?0.5:tolerance){}
    Path sort(const Path& vectors) const override;
};

class PYSLM_API BaseHatcher {
public:
    static constexpr std::int64_t CLIPPER_SCALEFACTOR=100000;
    virtual ~BaseHatcher() = default;
    static constexpr double error() noexcept { return 1.0/CLIPPER_SCALEFACTOR; }
    static Paths offsetBoundary(const Paths& paths, double offset);
    static std::vector<Paths> offsetPolygons(const std::vector<Paths>& polygons, double offset);
    static Bounds2 polygonBoundingBox(const Paths& paths);
    static Bounds2 polygonBoundingBox(const Path& path);
    static Bounds2 boundaryBoundingBox(const Paths& paths);
    static HatchLines clipLines(const Paths& paths, const HatchLines& lines);
    static std::vector<std::vector<Vec3>> clipContourLines(const Paths& paths,
                                            const std::vector<std::vector<Vec3>>& contours);
    virtual HatchLines generateHatching(const Paths& paths, double spacing,
                                       double angle=90.0) const;
    virtual std::optional<Layer> hatch(const Paths& boundary) const = 0;
};

class PYSLM_API InnerHatchRegion {
public:
    Vec2 origin{};
    // Radians, matching the original InnerHatchRegion API.
    double orientation=0.0;
    virtual ~InnerHatchRegion() = default;
    Path transformCoordinates2D(const Path& coords) const;
    HatchLines transformCoordinates(const HatchLines& coords) const;
    std::array<double,4> rotationMatrix2D() const;
    std::array<double,9> rotationMatrix3D() const;
    void setIntersecting(bool state) noexcept { intersecting_=state; }
    void setRequiresClipping(bool state) noexcept { requiresClipping_=state; }
    bool isIntersecting() const noexcept { return intersecting_; }
    bool requiresClipping() const noexcept { return requiresClipping_; }
    virtual Path boundary() const = 0;
    virtual HatchLines hatch() const = 0;
private:
    bool intersecting_=false, requiresClipping_=false;
};

class PYSLM_API Hatcher : public BaseHatcher {
public:
    bool scanContourFirst=false, hatchingEnabled=true;
    int numInnerContours=1, numOuterContours=1;
    double spotCompensation=0.08, contourOffset=0.08, volumeOffsetHatch=0.08;
    double layerAngleIncrement=0.0, hatchDistance=0.08, hatchAngle=45.0;
    std::shared_ptr<BaseSort> hatchSortMethod;
    std::optional<Layer> hatch(const Paths& boundary) const override;
};
class PYSLM_API StripeHatcher : public Hatcher {
public:
    double stripeWidth=5.0, stripeOverlap=0.1, stripeOffset=0.5;
    HatchLines generateHatching(const Paths& paths,double spacing,double angle=90.0) const override;
};
class PYSLM_API BasicIslandHatcher : public Hatcher {
public:
    double islandWidth=5.0, islandOverlap=0.1, islandOffset=0.5;
    HatchLines generateHatching(const Paths& paths,double spacing,double angle=90.0) const override;
};
class PYSLM_API Island : public InnerHatchRegion {
public:
    std::array<int,2> posId{0,0};
    int id=0;
    double islandWidth=0.0, islandOverlap=0.0, hatchDistance=0.1;
    Island(Vec2 position={},double radians=0.0,double width=0.0,double overlap=0.0,double spacing=0.1);
    Path localBoundary() const;
    Path boundary() const override;
    HatchLines generateInternalHatch(bool isOdd=true) const;
    HatchLines hatch() const override;
    // Original caches a single boundary across all instances; explicitly reset between unrelated recipes.
    static void resetBoundaryCache();
};
class PYSLM_API IslandHatcher : public Hatcher {
public:
    double islandWidth=5.0, islandOverlap=0.1, islandOffset=0.5;
    virtual std::vector<Island> generateIslands(const Paths& paths,double angle=90.0) const;
    std::pair<std::vector<std::size_t>,std::vector<std::size_t>> intersectIslands(
                                                    const Paths& paths,std::vector<Island>& islands) const;
    Paths clipIslands(const Paths& paths,const Paths& subjects) const;
    std::optional<Layer> hatch(const Paths& boundary) const override;
};

// XY plus deposited energy in z; includePowerDeposited=false returns zero z.
PYSLM_API std::vector<Vec3> getExposurePoints(const Layer& layer,const std::vector<Model>& models,
                                           bool includePowerDeposited=true);

} // namespace pyslm::hatching
