#pragma once

#include "types.hpp"
#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace pyslm::analysis {

// Units match the reference: coordinates mm, speed mm/s, delays microseconds.
PYSLM_API double getLayerGeometryJumpDistance(const LayerGeometry& layerGeom);
PYSLM_API double getLayerGeometryPathLength(const LayerGeometry& layerGeom);
PYSLM_API std::size_t getLayerGeomTotalJumps(const LayerGeometry& layerGeom);
PYSLM_API double getIntraLayerGeometryJumpLength(const Layer& layer);
PYSLM_API double getLayerJumpLength(const Layer& layer);
PYSLM_API double getLayerPathLength(const Layer& layer);
PYSLM_API double getEffectiveLaserSpeed(const BuildStyle& style);
PYSLM_API double getLayerGeometryTime(const LayerGeometry& layerGeom,const std::vector<Model>& models,
                                    bool includeJumpTime=false,double jumpSpeed=5000.0,double jumpDelay=0.0);
// Preserves reference quirks documented in docs/analysis_deviations.md.
PYSLM_API double getLayerTime(const Layer& layer,const std::vector<Model>& models,
                            bool includeJumpTime=true,double laserJumpSpeed=5000.0);

using ScanVector=std::array<Vec2,2>;
using ScanVectors=std::vector<ScanVector>;
struct LaserState {
    Vec2 position;
    const BuildStyle* buildStyle=nullptr;
    bool isActive=false;
};

struct PYSLM_API TimeNode {
    TimeNode* parent=nullptr;
    std::size_t id=0;
    const Layer* layer=nullptr;
    const LayerGeometry* geometry=nullptr;
    double time=0.0;
    std::vector<TimeNode> children;
    double getChildrenTime() const;
};

class PYSLM_API ScanVectorIterator {
public:
    explicit ScanVectorIterator(std::vector<Layer> layers);
    ScanVectors initialise();
    static ScanVectors reshapeVectors(const Paths& vectorList);
    static Paths getLayerVectors(const Layer& layer);
    bool next(ScanVector& vector);
    void reset();
    const ScanVectors& vectors() const noexcept { return vectors_; }
private:
    std::vector<Layer> layers_;
    ScanVectors vectors_;
    ScanVectors layerVectors_;
    std::size_t index_=0,layerIndex_=0;
};

class PYSLM_API Iterator {
public:
    // Preserves the original iterator's last-geometry omission and strict
    // time-boundary rules; see docs/analysis_deviations.md before using.
    Iterator(std::vector<Model> models,std::vector<Layer> layers);
    virtual ~Iterator()=default;
    Iterator(const Iterator&)=delete;
    Iterator& operator=(const Iterator&)=delete;
    double time() const noexcept { return time_; }
    double currentLayerGeometryTime() const noexcept { return layerGeomTime_; }
    const TimeNode& tree() const;
    virtual double dwellTime() const { return recoaterTime_; }
    double recoaterTime() const noexcept { return recoaterTime_; }
    void setRecoaterTime(double seconds);
    const std::vector<Layer>& layers() const noexcept { return layers_; }
    const std::vector<Model>& models() const noexcept { return models_; }
    void setLayers(std::vector<Layer> layers);
    void setModels(std::vector<Model> models);
    double getBuildTime() const;
    double getLayerGeomTime(std::size_t layerId,std::size_t layerGeomId) const;
    double getLayerTime(std::size_t layerId) const;
    double getTimeByLayerGeometryId(std::size_t layerId,std::size_t layerGeomId) const;
    const TimeNode* getLayerGeometryNodeByTime(double time) const;
    const LayerGeometry* getLayerGeometryByTime(double time) const;
    std::optional<std::size_t> getLayerGeometryIdByTime(double time) const;
    const TimeNode* getLayerNodeByTime(double time) const;
    const Layer* getLayerByTime(double time) const;
    std::optional<std::size_t> getLayerIdByTime(double time) const;
    double getTimeByLayerId(std::size_t layerId) const;
    const LayerGeometry& getCurrentLayerGeometry() const;
    const Layer& getCurrentLayer() const;
    void seekByLayerGeometry(std::size_t layerId,std::size_t layerGeomId);
    void seekByLayer(std::size_t layerId);
    virtual void seek(double time);
    virtual void reset();
    virtual bool next(const LayerGeometry*& geometry);
protected:
    void generateCache() const;
    std::vector<Model> models_;
    std::vector<Layer> layers_;
    double time_=0.0,layerGeomTime_=0.0,recoaterTime_=0.0;
    std::size_t layerIndex_=0,geometryIndex_=0;
    mutable bool cacheValid_=false;
    mutable TimeNode tree_;
};

class PYSLM_API LayerGeometryIterator : public Iterator {
public:
    using Iterator::Iterator;
};

struct ScanSample { Vec2 position; double time=0.0; bool laserOn=false; };
class PYSLM_API ScanIterator : public Iterator {
public:
    using Iterator::Iterator;
    double timestep=1e-3;
    bool isLaserOn() const;
    Vec2 getPointInLayerGeometry(double timeOffset,const LayerGeometry& geometry) const;
    Vec2 getCurrentLaserPosition() const;
    const BuildStyle& getCurrentBuildStyle() const;
    bool next(ScanSample& sample);
};

} // namespace pyslm::analysis
