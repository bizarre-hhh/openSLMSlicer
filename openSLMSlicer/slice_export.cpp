#include "slice_export.h"

#include "cli_format.h"
#include "slice_report_export.h"
#include "slmslice_format.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace
{
bool IncludesEmptyLayers(SliceExportFormat format,
                         const SliceExportOptions& options)
{
    return options.include_empty_layers &&
           (format == SliceExportFormat::SlmSlice ||
            format == SliceExportFormat::Csv);
}

std::size_t ExpectedWrittenLayers(const SliceBuildSnapshot& snapshot,
                                  const SliceExportOptions& options)
{
    if (IncludesEmptyLayers(options.format, options))
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

double ExpectedContourLengthMm(const SliceBuildSnapshot& snapshot)
{
    double length = 0.0;
    for (const SliceLayer& layer : snapshot.layers)
        length += SliceLayerContourLengthMm(layer);
    return length;
}

double ExpectedHatchLengthMm(const SliceBuildSnapshot& snapshot,
                             const SliceExportOptions& options)
{
    if (!options.include_hatches)
        return 0.0;
    double length = 0.0;
    for (const SliceLayer& layer : snapshot.layers)
        length += SliceLayerHatchLengthMm(layer);
    return length;
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

    // CLI rounds coordinates to 4 decimals, so its parsed path lengths are not
    // compared; every other format reports lengths computed from the snapshot.
    if (ok && options.format != SliceExportFormat::Cli)
    {
        constexpr double kLengthToleranceMm = 0.05;
        const double expected_contour_length = ExpectedContourLengthMm(snapshot);
        const double expected_hatch_length =
            ExpectedHatchLengthMm(snapshot, options);
        if (std::abs(summary.contour_length_mm - expected_contour_length) >
            kLengthToleranceMm)
        {
            result.issues.push_back(
                QStringLiteral("轮廓长度不一致：文件 %1 mm，预期 %2 mm")
                    .arg(QString::number(summary.contour_length_mm, 'f', 3),
                         QString::number(expected_contour_length, 'f', 3)));
            ok = false;
        }
        if (std::abs(summary.hatch_length_mm - expected_hatch_length) >
            kLengthToleranceMm)
        {
            result.issues.push_back(
                QStringLiteral("填充长度不一致：文件 %1 mm，预期 %2 mm")
                    .arg(QString::number(summary.hatch_length_mm, 'f', 3),
                         QString::number(expected_hatch_length, 'f', 3)));
            ok = false;
        }
    }
    return ok;
}

class SlmSliceWriter final : public IJobWriter
{
public:
    SliceExportFormat format() const override
    {
        return SliceExportFormat::SlmSlice;
    }
    QString name() const override
    {
        return QStringLiteral("openSLMSlicer 切片 (.slmslice)");
    }
    QString fileFilter() const override
    {
        return QStringLiteral("openSLMSlicer 切片 (*.slmslice)");
    }
    QString suffix() const override { return QStringLiteral("slmslice"); }

    bool write(const SliceBuildSnapshot& snapshot,
               const SliceExportOptions& options,
               const SliceExportProgress& progress,
               SliceExportResult& result) const override
    {
        return WriteSlmSliceV1(snapshot, options, progress, result);
    }
    bool summarize(const QString& path, SliceFileSummary& summary,
                   QString& error) const override
    {
        SliceBuildSnapshot ignored;
        return ReadSlmSliceV1(path, ignored, summary, error);
    }
};

class CliWriter final : public IJobWriter
{
public:
    SliceExportFormat format() const override
    {
        return SliceExportFormat::Cli;
    }
    QString name() const override
    {
        return QStringLiteral("CLI 2.0 ASCII (.cli)");
    }
    QString fileFilter() const override
    {
        return QStringLiteral("Common Layer Interface (*.cli)");
    }
    QString suffix() const override { return QStringLiteral("cli"); }

    bool write(const SliceBuildSnapshot& snapshot,
               const SliceExportOptions& options,
               const SliceExportProgress& progress,
               SliceExportResult& result) const override
    {
        return WriteCliAscii(snapshot, options, progress, result);
    }
    bool summarize(const QString& path, SliceFileSummary& summary,
                   QString& error) const override
    {
        return ParseCliAscii(path, summary, error);
    }
};

class SvgWriter final : public IJobWriter
{
public:
    SliceExportFormat format() const override
    {
        return SliceExportFormat::Svg;
    }
    QString name() const override
    {
        return QStringLiteral("SVG 联系人表 (.svg)");
    }
    QString fileFilter() const override
    {
        return QStringLiteral("SVG 联系人表 (*.svg)");
    }
    QString suffix() const override { return QStringLiteral("svg"); }

    bool write(const SliceBuildSnapshot& snapshot,
               const SliceExportOptions& options,
               const SliceExportProgress& progress,
               SliceExportResult& result) const override
    {
        return WriteSvgReport(snapshot, options, progress, result);
    }
    bool summarize(const QString& path, SliceFileSummary& summary,
                   QString& error) const override
    {
        return SummarizeSvgReport(path, summary, error);
    }
};

class PngWriter final : public IJobWriter
{
public:
    SliceExportFormat format() const override
    {
        return SliceExportFormat::Png;
    }
    QString name() const override
    {
        return QStringLiteral("PNG 联系人表 (.png)");
    }
    QString fileFilter() const override
    {
        return QStringLiteral("PNG 联系人表 (*.png)");
    }
    QString suffix() const override { return QStringLiteral("png"); }

    bool write(const SliceBuildSnapshot& snapshot,
               const SliceExportOptions& options,
               const SliceExportProgress& progress,
               SliceExportResult& result) const override
    {
        return WritePngReport(snapshot, options, progress, result);
    }
    bool summarize(const QString& path, SliceFileSummary& summary,
                   QString& error) const override
    {
        return SummarizePngReport(path, summary, error);
    }
};

class CsvWriter final : public IJobWriter
{
public:
    SliceExportFormat format() const override
    {
        return SliceExportFormat::Csv;
    }
    QString name() const override
    {
        return QStringLiteral("CSV 逐层统计 (.csv)");
    }
    QString fileFilter() const override
    {
        return QStringLiteral("CSV 逐层统计 (*.csv)");
    }
    QString suffix() const override { return QStringLiteral("csv"); }

    bool write(const SliceBuildSnapshot& snapshot,
               const SliceExportOptions& options,
               const SliceExportProgress& progress,
               SliceExportResult& result) const override
    {
        return WriteCsvReport(snapshot, options, progress, result);
    }
    bool summarize(const QString& path, SliceFileSummary& summary,
                   QString& error) const override
    {
        return SummarizeCsvReport(path, summary, error);
    }
};
} // namespace

SliceExportRegistry::SliceExportRegistry()
{
    owned_.push_back(std::make_unique<SlmSliceWriter>());
    owned_.push_back(std::make_unique<CliWriter>());
    owned_.push_back(std::make_unique<SvgWriter>());
    owned_.push_back(std::make_unique<PngWriter>());
    owned_.push_back(std::make_unique<CsvWriter>());
    writers_.reserve(owned_.size());
    for (const std::unique_ptr<IJobWriter>& writer : owned_)
        writers_.push_back(writer.get());
}

const SliceExportRegistry& SliceExportRegistry::Instance()
{
    static const SliceExportRegistry registry;
    return registry;
}

const IJobWriter* SliceExportRegistry::WriterFor(SliceExportFormat format) const
{
    for (const IJobWriter* writer : writers_)
        if (writer->format() == format)
            return writer;
    return nullptr;
}

QString SliceExportFormatName(SliceExportFormat format)
{
    const IJobWriter* writer = SliceExportRegistry::Instance().WriterFor(format);
    return writer ? writer->name() : QString();
}

QString SliceExportFileFilter(SliceExportFormat format)
{
    const IJobWriter* writer = SliceExportRegistry::Instance().WriterFor(format);
    return writer ? writer->fileFilter() : QString();
}

QString SliceExportSuffix(SliceExportFormat format)
{
    const IJobWriter* writer = SliceExportRegistry::Instance().WriterFor(format);
    return writer ? writer->suffix() : QString();
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

    const IJobWriter* writer =
        SliceExportRegistry::Instance().WriterFor(options.format);
    if (writer == nullptr)
    {
        result.error = QStringLiteral("不支持的导出格式。");
        return result;
    }
    if (!writer->write(snapshot, options, progress, result))
        return result;

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
    const IJobWriter* writer =
        SliceExportRegistry::Instance().WriterFor(format);
    if (writer == nullptr)
    {
        error = QStringLiteral("不支持的切片文件格式。");
        return false;
    }
    return writer->summarize(path, summary, error);
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
