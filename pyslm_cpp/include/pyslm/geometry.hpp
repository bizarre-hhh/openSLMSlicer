#pragma once

#include "types.hpp"
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace pyslm::geometry {

// Insertion order is significant: Python dictionaries retain first insertion order.
class PYSLM_API LayerDict {
public:
    using Entry = std::pair<int, Layer>;
    using Container = std::vector<Entry>;
    Container entries;
    Layer& at(int layerId);
    const Layer& at(int layerId) const;
    Layer& operator[](int layerId);
    std::size_t size() const noexcept { return entries.size(); }
    bool empty() const noexcept { return entries.empty(); }
    Container::iterator begin() noexcept { return entries.begin(); }
    Container::iterator end() noexcept { return entries.end(); }
    Container::const_iterator begin() const noexcept { return entries.begin(); }
    Container::const_iterator end() const noexcept { return entries.end(); }
};

PYSLM_API LayerDict createLayerDict(const std::vector<Layer>& layerList);
// C++ values are independent copies; merging does not mutate the source layers.
PYSLM_API LayerDict mergeLayers(const std::vector<LayerDict>& layerLists);
PYSLM_API void translateLayerGeoms(LayerGeometry& geometry, Vec2 translation);
PYSLM_API void translateLayerGeoms(std::vector<LayerGeometry>& geometries, Vec2 translation);
PYSLM_API void transformLayerGeoms(LayerGeometry& geometry, const std::array<double,4>& transform);
PYSLM_API void transformLayerGeoms(LayerGeometry& geometry, const std::array<double,9>& transform);
PYSLM_API void transformLayerGeoms(std::vector<LayerGeometry>& geometries, const std::array<double,4>& transform);
PYSLM_API void transformLayerGeoms(std::vector<LayerGeometry>& geometries, const std::array<double,9>& transform);
PYSLM_API const BuildStyle& getBuildStyleById(const std::vector<Model>& models, int mid, int bid);
PYSLM_API BuildStyle& getBuildStyleById(std::vector<Model>& models, int mid, int bid);
PYSLM_API const Layer& getLayerById(const std::vector<Layer>& layers, int layerId);
PYSLM_API Layer& getLayerById(std::vector<Layer>& layers, int layerId);
PYSLM_API const Model& getModel(const std::vector<Model>& models, int mid);
PYSLM_API Model& getModel(std::vector<Model>& models, int mid);
PYSLM_API std::string toString(const BuildStyle& style);
PYSLM_API std::string toString(const LayerGeometry& geometry);
PYSLM_API std::string toString(const Layer& layer);

class PYSLM_API ModelValidator {
public:
    static bool validateBuildStyle(const BuildStyle& style);
    static bool validateModel(const Model& model);
    // Warnings have an explicit recipient instead of a process-global warning hook.
    static bool validateBuild(const std::vector<Model>& models, const std::vector<Layer>& layers,
                              std::vector<std::string>* warnings = nullptr);
};

} // namespace pyslm::geometry
