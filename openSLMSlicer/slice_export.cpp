#include "slice_export.h"

#include "cli_format.h"
#include "slmslice_format.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
std::size_t ExpectedWrittenLayers(const SliceBuildSnapshot& snapshot,
                                  const SliceExportOptions& options)
{
    if (options.format == SliceExportFormat::SlmSlice && options.include_empty_layers)
        return snapshot.layers.size();
    std::size_t count = 0;
    for (const SliceLayer& layer : snapshot.layers)
        if (!layer.empty)
            ++count;
    return count;
}

std::size_t ExpectedContours(const SliceBuildSnapshot& snapshot)
{
    std::size_t count = 0;
    for (const SliceLayer& layer : snapshot.layers)
        count += layer.contours.size();
    return count;
}

std::size_t ExpectedHatches(const SliceBuildSnapshot& snapshot,
                            const SliceExportOptions& options)
{
    if (!options.include_hatches)
        return 0;
    std::size_t count = 0;
    for (const SliceLayer& layer : snapshot.layers)
        count += layer.hatches.size();
    return count;
}

bool ValidateWrittenFile(const QString& path,
                         const SliceExportOptions& options,
                         const SliceBuildSnapshot& snapshot,
                         SliceExportResult& result)
{
    QString error;
    SliceFileSummary summary;
    if (!SummarizeSliceFile(path, options.format, summary, error))
    {
        result.issues.push_back(QStringLiteral("回读校验失败：%1").arg(error));
        return false;
    }

    const std::size_t expected_layers = ExpectedWrittenLayers(snapshot, options);
    const std::size_t expected_contours = ExpectedContours(snapshot);
    const std::size_t expected_hatches = ExpectedHatches(snapshot, options);
    bool ok = true;
    if (summary.layers != expected_layers)
    {
        result.issues.push_back(
            QStringLiteral("层数不一致：文件 %1，预期 %2")
                .arg(static_cast<qulonglong>(summary.layers))
                .arg(static_cast<qulonglong>(expected_layers)));
        ok = false;
    }
    if (summary.contours != expected_contours)
    {
        result.issues.push_back(
            QStringLiteral("轮廓数不一致：文件 %1，预期 %2")
                .arg(static_cast<qulonglong>(summary.contours))
                .arg(static_cast<qulonglong>(expected_contours)));
        ok = false;
    }
    if (summary.hatches != expected_hatches)
    {
        result.issues.push_back(
            QStringLiteral("填充数不一致：文件 %1，预期 %2")
                .arg(static_cast<qulonglong>(summary.hatches))
                .arg(static_cast<qulonglong>(expected_hatches)));
        ok = false;
    }
    return ok;
}
} // namespace

QString SliceExportFormatName(SliceExportFormat format)
{
    switch (format)
    {
    case SliceExportFormat::SlmSlice:
        return QStringLiteral("openSLMSlicer 切片 (.slmslice)");
    case SliceExportFormat::Cli:
        return QStringLiteral("CLI 2.0 ASCII (.cli)");
    }
    return QString();
}

QString SliceExportFileFilter(SliceExportFormat format)
{
    switch (format)
    {
    case SliceExportFormat::SlmSlice:
        return QStringLiteral("openSLMSlicer 切片 (*.slmslice)");
    case SliceExportFormat::Cli:
        return QStringLiteral("Common Layer Interface (*.cli)");
    }
    return QString();
}

QString SliceExportSuffix(SliceExportFormat format)
{
    switch (format)
    {
    case SliceExportFormat::SlmSlice:
        return QStringLiteral("slmslice");
    case SliceExportFormat::Cli:
        return QStringLiteral("cli");
    }
    return QString();
}

SliceBuildSnapshot BuildSliceBuildSnapshot(
    std::vector<SliceLayer> layers,
    const SliceSettings& settings,
    std::vector<SlicePartInfo> parts,
    const std::array<double, 3>& plate_size_mm,
    std::uint64_t source_project_revision,
    const QString& app_version)
{
    SliceBuildSnapshot snapshot;
    snapshot.settings = settings;
    snapshot.parts = std::move(parts);
    snapshot.plate_size_mm = plate_size_mm;
    snapshot.source_project_revision = source_project_revision;
    snapshot.app_version = app_version;

    if (layers.empty())
        return snapshot;

    std::sort(layers.begin(), layers.end(),
              [](const SliceLayer& a, const SliceLayer& b)
              {
                  return a.grid_index < b.grid_index;
              });
    const std::int64_t first = layers.front().grid_index;
    const std::int64_t last = layers.back().grid_index;
    if (last < first)
        return snapshot;

    const std::uint64_t count =
        static_cast<std::uint64_t>(last - first) + 1u;
    if (count > static_cast<std::uint64_t>(kMaxSliceLayers) * 2u)
        return snapshot;

    const double thickness = settings.layer_thickness_mm;

    std::vector<SliceLayer> filled(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < filled.size(); ++i)
    {
        SliceLayer& layer = filled[i];
        layer.grid_index = first + static_cast<std::int64_t>(i);
        layer.index = i + 1;
        layer.z_mm = thickness * 0.5 +
                     static_cast<double>(layer.grid_index) * thickness;
        layer.empty = true;
    }

    for (SliceLayer& layer : layers)
    {
        const std::uint64_t position =
            static_cast<std::uint64_t>(layer.grid_index - first);
        if (position >= count)
            continue;
        SliceLayer& target = filled[static_cast<std::size_t>(position)];
        target = std::move(layer);
        target.index = static_cast<std::size_t>(position) + 1;
        target.empty = target.contours.empty() && target.hatches.empty();
    }

    snapshot.layers = std::move(filled);
    return snapshot;
}

SliceExportResult ExportSliceBuild(
    const SliceBuildSnapshot& snapshot,
    const SliceExportOptions& options,
    const SliceExportProgress& progress)
{
    SliceExportResult result;
    if (options.output_path.trimmed().isEmpty())
    {
        result.error = QStringLiteral("未指定导出文件路径。");
        return result;
    }
    if (snapshot.layers.empty())
    {
        result.error = QStringLiteral("没有可导出的切片数据。");
        return result;
    }

    switch (options.format)
    {
    case SliceExportFormat::SlmSlice:
        if (!WriteSlmSliceV1(snapshot, options, progress, result))
            return result;
        break;
    case SliceExportFormat::Cli:
        if (!WriteCliAscii(snapshot, options, progress, result))
            return result;
        break;
    default:
        result.error = QStringLiteral("不支持的导出格式。");
        return result;
    }

    result.ok = true;
    if (options.validate_after_write)
    {
        if (!ValidateWrittenFile(options.output_path, options, snapshot, result))
        {
            result.ok = false;
            result.error = QStringLiteral("导出已完成，但回读校验未通过。");
            return result;
        }
    }

    const QFileInfo info(options.output_path);
    result.bytes_written = info.exists()
        ? static_cast<std::uint64_t>(info.size()) : 0;
    return result;
}

bool ReadSlmSliceFile(const QString& path, SliceBuildSnapshot& snapshot,
                     QString& error)
{
    SliceFileSummary summary;
    return ReadSlmSliceV1(path, snapshot, summary, error);
}

bool ReadSlmSliceFileInfo(const QString& path, SliceFileInfo& info,
                         QString& error)
{
    return ReadSlmSliceV1Info(path, info, error);
}

bool SummarizeSliceFile(const QString& path, SliceExportFormat format,
                        SliceFileSummary& summary, QString& error)
{
    summary = {};
    SliceBuildSnapshot snapshot;
    switch (format)
    {
    case SliceExportFormat::SlmSlice:
        return ReadSlmSliceV1(path, snapshot, summary, error);
    case SliceExportFormat::Cli:
        return ParseCliAscii(path, summary, error);
    }
    error = QStringLiteral("不支持的切片文件格式。");
    return false;
}

bool ValidateSliceFile(const QString& path, SliceExportFormat format,
                       QString& error)
{
    SliceFileSummary summary;
    if (!SummarizeSliceFile(path, format, summary, error))
        return false;
    if (summary.layers == 0)
    {
        error = QStringLiteral("切片文件不包含任何层。");
        return false;
    }
    return true;
}
