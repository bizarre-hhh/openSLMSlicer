#ifndef SLICE_EXPORT_H
#define SLICE_EXPORT_H

#include "slice_engine.h"

#include <QString>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// Export-facing snapshot of one generated slice build. The scene viewport owns
// the preview data; this value type is what writers/readers and tests use.
struct SlicePartInfo
{
    std::uint64_t id = 0;
    QString name;
    QString source_path;
    std::size_t triangle_count = 0;
};

struct SliceBuildSnapshot
{
    std::vector<SlicePartInfo> parts;
    // Non-empty layers plus the empty grid layers between the first and last
    // non-empty layer (empty=true). Coordinates are build-plate millimeters;
    // z_mm is the layer mid plane (Z = (grid_index + 0.5) * thickness).
    std::vector<SliceLayer> layers;
    SliceSettings settings;
    std::array<double, 3> plate_size_mm{100.0, 100.0, 100.0};
    std::uint64_t source_project_revision = 0;
    QString app_version;
};

enum class SliceExportFormat
{
    SlmSlice,
    Cli,
    Svg,
    Png,
    Csv
};

struct SliceExportOptions
{
    SliceExportFormat format = SliceExportFormat::Cli;
    QString output_path;
    // .slmslice and CSV can keep the empty grid layers between the first and
    // last non-empty layer; CLI, SVG and PNG always skip empty layers.
    bool include_empty_layers = true;
    bool include_hatches = true;
    bool validate_after_write = true;
};

struct SliceExportResult
{
    bool ok = false;
    bool cancelled = false;
    QString error;
    std::vector<QString> issues;
    std::size_t layers_written = 0;
    std::size_t contours_written = 0;
    std::size_t hatches_written = 0;
    std::uint64_t bytes_written = 0;
};

struct SliceFileSummary
{
    std::size_t layers = 0;
    std::size_t contours = 0;
    std::size_t hatches = 0;
    double contour_length_mm = 0.0;
    double hatch_length_mm = 0.0;
};

struct SliceLayerChecksum
{
    std::int64_t grid_index = 0;
    std::uint64_t bytes = 0;
    QString sha256;
};

// Metadata-only view of a .slmslice file. Reading this validates the container
// and each layer header without keeping the geometry in memory.
struct SliceFileInfo
{
    QString header_json;
    QString app_version;
    QString created_utc;
    std::array<double, 3> plate_size_mm{};
    double layer_thickness_mm = 0.0;
    bool include_empty_layers = true;
    std::size_t layer_count = 0;
    std::size_t empty_layer_count = 0;
    std::size_t contour_count = 0;
    std::size_t hatch_count = 0;
    std::vector<SlicePartInfo> parts;
    std::vector<SliceLayerChecksum> checksums;
};

using SliceExportProgress =
    std::function<bool(std::size_t completed, std::size_t total)>;

// Length helpers shared by the writers and the read-back validation. They are
// computed from the stored geometry, not from the cached per-layer totals, so
// hand-built snapshots and loaded files use the same numbers.
inline double SlicePathLengthMm(const SlicePath& path)
{
    double length = 0.0;
    for (std::size_t i = 1; i < path.points.size(); ++i)
    {
        const double dx = path.points[i][0] - path.points[i - 1][0];
        const double dy = path.points[i][1] - path.points[i - 1][1];
        length += std::hypot(dx, dy);
    }
    if (path.closed && path.points.size() > 2)
    {
        const auto& first = path.points.front();
        const auto& last = path.points.back();
        if (first[0] != last[0] || first[1] != last[1])
            length += std::hypot(first[0] - last[0], first[1] - last[1]);
    }
    return length;
}

inline double SliceHatchLengthMm(const SliceHatchLine& line)
{
    return std::hypot(line.end[0] - line.start[0],
                      line.end[1] - line.start[1]);
}

inline double SliceLayerContourLengthMm(const SliceLayer& layer)
{
    double length = 0.0;
    for (const SlicePath& path : layer.contours)
        length += SlicePathLengthMm(path);
    return length;
}

inline double SliceLayerHatchLengthMm(const SliceLayer& layer)
{
    double length = 0.0;
    for (const SliceHatchLine& line : layer.hatches)
        length += SliceHatchLengthMm(line);
    return length;
}

// One export format. Writers only serialize a snapshot; they never read UI
// state and never re-slice.
class IJobWriter
{
public:
    virtual ~IJobWriter() = default;

    virtual SliceExportFormat format() const = 0;
    virtual QString name() const = 0;       // "openSLMSlicer 切片 (.slmslice)"
    virtual QString fileFilter() const = 0; // "openSLMSlicer 切片 (*.slmslice)"
    virtual QString suffix() const = 0;     // "slmslice"

    virtual bool write(const SliceBuildSnapshot& snapshot,
                       const SliceExportOptions& options,
                       const SliceExportProgress& progress,
                       SliceExportResult& result) const = 0;

    // Validates the container and returns the geometry statistics stored in it.
    virtual bool summarize(const QString& path, SliceFileSummary& summary,
                           QString& error) const = 0;
};

// Process-wide registry of export formats. Writers are registered once and are
// safe to use from the UI thread only.
class SliceExportRegistry
{
public:
    static const SliceExportRegistry& Instance();

    const IJobWriter* WriterFor(SliceExportFormat format) const;
    const std::vector<const IJobWriter*>& Writers() const { return writers_; }

private:
    SliceExportRegistry();

    std::vector<std::unique_ptr<IJobWriter>> owned_;
    std::vector<const IJobWriter*> writers_;
};

QString SliceExportFormatName(SliceExportFormat format);
QString SliceExportFileFilter(SliceExportFormat format);
QString SliceExportSuffix(SliceExportFormat format);

// Copy the engine layers into an export snapshot and fill the empty grid layers
// between the first and last non-empty layer.
SliceBuildSnapshot BuildSliceBuildSnapshot(
    std::vector<SliceLayer> layers,
    const SliceSettings& settings,
    std::vector<SlicePartInfo> parts,
    const std::array<double, 3>& plate_size_mm,
    std::uint64_t source_project_revision,
    const QString& app_version);

SliceExportResult ExportSliceBuild(
    const SliceBuildSnapshot& snapshot,
    const SliceExportOptions& options,
    const SliceExportProgress& progress = {});

bool ReadSlmSliceFile(const QString& path, SliceBuildSnapshot& snapshot,
                     QString& error);
bool ReadSlmSliceFileInfo(const QString& path, SliceFileInfo& info,
                         QString& error);
bool SummarizeSliceFile(const QString& path, SliceExportFormat format,
                        SliceFileSummary& summary, QString& error);
bool ValidateSliceFile(const QString& path, SliceExportFormat format,
                       QString& error);

#endif
