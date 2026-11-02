#pragma once

#include "core.hpp"
#include <optional>
#include <utility>

namespace pyslm::support {

class PYSLM_API SupportStructure {
public:
    // Non-owning part reference; caller keeps Part alive while supports are used.
    const Part* supportObject=nullptr;
    std::optional<Mesh> supportVolume,supportSurface;
    bool intersectsPart=false;
    SupportStructure(const Part* object=nullptr,std::optional<Mesh> volume=std::nullopt,
                     std::optional<Mesh> surface=std::nullopt,bool intersects=false);
    virtual ~SupportStructure()=default;
    virtual Mesh geometry() const=0;
    virtual double volume() const=0;
    static Polygon flattenSupportRegion(const Mesh& region);
    double projectedSupportArea() const;
    double supportArea() const;
};
class PYSLM_API BlockSupportBase : public SupportStructure {
public:
    using SupportStructure::SupportStructure;
    Mesh geometry() const override;
    double volume() const override;
    Mesh supportBoundary() const;
    static Mesh triangulateSections(const std::vector<std::optional<PlanarSection>>& sections);
};
class PYSLM_API BaseSupportGenerator {
public:
    static constexpr double PYCLIPPER_SCALEFACTOR=1e4;
    static constexpr double POINT_OVERHANG_TOLERANCE=0.05;
    virtual ~BaseSupportGenerator()=default;
    static std::vector<std::size_t> findOverhangPoints(const Part& part);
    static std::vector<std::array<std::uint32_t,2>> findOverhangEdges(
                           const Part& part,double overhangAngle=45.0,double edgeOverhangAngle=10.0);
};
class PYSLM_API BlockSupportGenerator : public BaseSupportGenerator {
public:
    static constexpr double supportSkinSideTolerance=1.0-1e-3;
    static constexpr double intersectionVolumeTolerance=50.0;
    static constexpr double gaussianBlurSigma=1.0;
    double minimumAreaThreshold=5.0,rayProjectionResolution=0.2;
    double lowerProjectionOffset=0.05,upperProjectionOffset=0.05;
    double innerSupportEdgeGap=0.2,outerSupportEdgeGap=0.5;
    double triangulationSpacing=2.0,simplifyPolygonFactor=0.5,overhangAngle=45.0;
    bool useApproxBasePlateSupport=false;
    std::optional<double> splineSimplificationFactor=20.0;
    static double gradThreshold(double projectionDistance,double overhangAngle);
    void filterSupportRegion(const Mesh& region) const;
    void generateIntersectionHeightMap() const;
    std::vector<BlockSupportBase> identifySupportRegions(const Part& part,double angle,
                                                       bool findSelfIntersectingSupport=true) const;
};

enum class GridMeshType : int { BOUNDARY=0,SLICE_X=1,SLICE_Y=2 };
struct GridSupportParameters {
    std::array<double,2> gridSpacing{3.0,3.0};
    bool useSupportBorder=true,useSupportSkin=true,generateTrussGrid=true,mergeMesh=false;
    bool useUpperSupportTeeth=true,useLowerSupportTeeth=false;
    int numSkinMeshSubdivideIterations=2;
    double supportWallThickness=0.5,supportBorderDistance=3.0,trussWidth=1.0,trussAngle=45.0;
    double supportTeethHeight=1.5,supportTeethTopLength=0.1,supportTeethBottomLength=1.5;
    double supportTeethBaseInterval=0.2,supportTeethUpperPenetration=0.2;
};
class PYSLM_API GridBlockSupport : public BlockSupportBase,public GridSupportParameters {
public:
    using BlockSupportBase::BlockSupportBase;
    static constexpr double pairTolerance=0.1;
    static Polygon holeGeometry();
    static Paths clipLines(const Paths& boundaries,const HatchLines& lines);
    static HatchLines generateMeshGrid(const Polygon& polygon,double spacing=5.0,double angle=45.0);
    void setSupportWallThickness(double value);
    Mesh geometry() const override;
    std::vector<Mesh> borderGeometry() const;
    std::pair<Mesh,Mesh> sliceGeometry() const;
    Polygon generateSliceBoundingBoxPolygon(const PlanarSection& section) const;
    Path toothProfile() const;
    std::optional<Polygons> generateSliceGeometry(const PlanarSection& section) const;
    Polygons generateSliceGeometryDepr(const Polygons& sections) const;
    Paths generateSupportSkinInfill(const Path& polygon,bool returnPolyNodes=false) const;
    std::vector<Mesh> generateSupportSkins(std::map<std::string,Paths>* diagnostics=nullptr) const;
    std::pair<Mesh,Mesh> generateSupportSlices() const;
    static Mesh concatenateMeshes(const std::vector<Mesh>& meshes);
    static std::vector<std::optional<PlanarSection>> sectionMultiplane(
                    const Mesh& volume,Vec3 origin,Vec3 normal,const std::vector<double>& heights);
    std::pair<std::vector<std::optional<PlanarSection>>,std::vector<std::optional<PlanarSection>>> generateGridSlices() const;
    static std::pair<Paths,Paths> slice(const std::vector<Mesh>& supports,double z);
};
class PYSLM_API GridBlockSupportGenerator : public BlockSupportGenerator,public GridSupportParameters {
public:
    GridBlockSupportGenerator();
    std::vector<GridBlockSupport> identifySupportRegions(const Part& part,double angle,
                                                       bool findSelfIntersectingSupport=true) const;
};

} // namespace pyslm::support
