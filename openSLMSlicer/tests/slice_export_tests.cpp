#include "slice_export.h"
#include "cli_format.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr double kTolerance = 1.0e-9;
constexpr double kReportLengthTolerance = 0.01;

bool Require(bool condition, const std::string& message)
{
    if (!condition)
        std::cerr << "FAILED: " << message << '\n';
    return condition;
}

bool ReadFileBytes(const QString& path, QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    bytes = file.readAll();
    return true;
}

SlicePath MakeRing(std::uint64_t model_id, SliceRingRole role,
                   double offset_x, double offset_y, double size)
{
    SlicePath path;
    path.model_id = model_id;
    path.role = role;
    path.closed = true;
    // Closed ring with the first point repeated at the end, matching the
    // slicer's internal representation.
    path.points = {
        {offset_x, offset_y},
        {offset_x + size, offset_y},
        {offset_x + size, offset_y + size},
        {offset_x, offset_y + size},
        {offset_x, offset_y}
    };
    return path;
}

SliceHatchLine MakeHatch(std::uint64_t model_id, double x1, double y1,
                         double x2, double y2)
{
    SliceHatchLine line;
    line.model_id = model_id;
    line.start = {x1, y1};
    line.end = {x2, y2};
    return line;
}

SliceBuildSnapshot MakeSnapshot(std::vector<SliceLayer> layers)
{
    std::vector<SlicePartInfo> parts;
    SlicePartInfo part;
    part.id = 1;
    part.name = QStringLiteral("立方体");
    part.source_path = QStringLiteral("D:/模型/cube.stl");
    part.triangle_count = 12;
    parts.push_back(part);
    SlicePartInfo second;
    second.id = 2;
    second.name = QStringLiteral("cube");
    second.source_path = QStringLiteral("D:/models/cube.stl");
    second.triangle_count = 12;
    parts.push_back(second);

    SliceSettings settings;
    settings.layer_thickness_mm = 0.03;
    settings.hatch.enabled = true;
    settings.hatch.spacing_mm = 0.08;
    settings.hatch.angle_deg = 45.0;
    settings.hatch.layer_angle_increment_deg = 67.0;
    settings.hatch.spot_compensation_mm = 0.08;

    return BuildSliceBuildSnapshot(std::move(layers), settings, std::move(parts),
                                   {100.0, 100.0, 100.0}, 42,
                                   QStringLiteral("0.1.0-test"));
}

SliceLayer MakeLayer(std::int64_t grid_index, std::size_t contour_count,
                     std::size_t hatch_count)
{
    SliceLayer layer;
    layer.grid_index = grid_index;
    layer.z_mm = 0.03 * 0.5 + static_cast<double>(grid_index) * 0.03;
    for (std::size_t i = 0; i < contour_count; ++i)
    {
        const SliceRingRole role = i == 0 ? SliceRingRole::Outer
                                          : SliceRingRole::Inner;
        layer.contours.push_back(MakeRing(
            1, role, static_cast<double>(i) * 2.0, 0.0, 10.0));
    }
    for (std::size_t i = 0; i < hatch_count; ++i)
        layer.hatches.push_back(MakeHatch(1, 0.0, static_cast<double>(i) * 0.1,
                                          10.0, static_cast<double>(i) * 0.1));
    return layer;
}

// Two non-empty layers with one empty grid layer between them. Every contour
// is a 10 x 10 mm square (40 mm) and every hatch line is 10 mm long.
SliceBuildSnapshot MakeReportSnapshot(std::vector<SliceLayer>* layers_out = nullptr)
{
    std::vector<SliceLayer> layers;
    layers.push_back(MakeLayer(0, 2, 2));
    layers.push_back(MakeLayer(2, 1, 1));
    SliceBuildSnapshot snapshot = MakeSnapshot(std::move(layers));
    if (layers_out != nullptr)
        *layers_out = snapshot.layers;
    return snapshot;
}

// Support geometry ids start at 1e9 (see support_types.h) so they never collide
// with model ids; tests only need the value, not the support headers.
constexpr std::uint64_t kSupportPartId = 1000000000ull;

// One model part (id 1) and one support part (id 1e9) with geometry on the same
// layer: a 10 x 10 mm model square/hatch plus a 5 x 5 mm support square/hatch.
SliceBuildSnapshot MakeSupportSnapshot()
{
    SliceLayer layer;
    layer.grid_index = 0;
    layer.z_mm = 0.015;
    layer.contours.push_back(
        MakeRing(1, SliceRingRole::Outer, 0.0, 0.0, 10.0));
    layer.contours.push_back(
        MakeRing(kSupportPartId, SliceRingRole::Outer, 20.0, 0.0, 5.0));
    layer.hatches.push_back(MakeHatch(1, 0.0, 0.0, 10.0, 0.0));
    layer.hatches.push_back(
        MakeHatch(kSupportPartId, 20.0, 0.0, 25.0, 0.0));

    SliceBuildSnapshot snapshot = MakeSnapshot({layer});
    SlicePartInfo support;
    support.id = kSupportPartId;
    support.name = QStringLiteral("立方体 支撑");
    support.source_path = QStringLiteral("D:/模型/cube.stl");
    support.triangle_count = 64;
    support.kind = SlicePartKind::Support;
    snapshot.parts.push_back(support);
    return snapshot;
}

// Reads the JSON header of a .slmslice file, reusing the layout documented in
// docs/slmslice-v1.md: magic (8) + version (4) + header_bytes (4) + header.
bool ReadSlmSliceHeader(const QString& path, QJsonObject& header)
{
    QByteArray bytes;
    if (!ReadFileBytes(path, bytes) || bytes.size() < 16)
        return false;
    const quint32 header_bytes = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(bytes.constData()) + 12);
    if (static_cast<qint64>(header_bytes) > bytes.size() - 16)
        return false;
    const QJsonDocument document =
        QJsonDocument::fromJson(bytes.mid(16, static_cast<int>(header_bytes)));
    if (!document.isObject())
        return false;
    header = document.object();
    return true;
}

// Rewrites a .slmslice file with a modified header parts[] array, keeping the
// version, payload and every other header field intact. Used to emulate files
// written by an older version (no "kind") or by a newer writer (unknown kind).
bool RewriteSlmSliceParts(
    const QString& source, const QString& target,
    const std::function<void(QJsonObject&)>& mutate_part)
{
    QByteArray bytes;
    if (!ReadFileBytes(source, bytes) || bytes.size() < 16)
        return false;
    const quint32 old_header_bytes = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(bytes.constData()) + 12);
    if (static_cast<qint64>(old_header_bytes) > bytes.size() - 16)
        return false;
    const QJsonDocument document =
        QJsonDocument::fromJson(bytes.mid(16, static_cast<int>(old_header_bytes)));
    if (!document.isObject())
        return false;

    QJsonObject header = document.object();
    QJsonArray parts = header.value(QStringLiteral("parts")).toArray();
    for (int i = 0; i < parts.size(); ++i)
    {
        QJsonObject part = parts.at(i).toObject();
        mutate_part(part);
        parts.replace(i, part);
    }
    header.insert(QStringLiteral("parts"), parts);
    const QByteArray header_raw =
        QJsonDocument(header).toJson(QJsonDocument::Indented);

    QByteArray rewritten;
    rewritten.append(bytes.constData(), 8);       // magic
    rewritten.append(bytes.constData() + 8, 4);   // version (little-endian)
    uchar header_bytes_le[4] = {0, 0, 0, 0};
    qToLittleEndian<quint32>(static_cast<quint32>(header_raw.size()),
                             static_cast<void*>(header_bytes_le));
    rewritten.append(reinterpret_cast<const char*>(header_bytes_le), 4);
    rewritten.append(header_raw);
    rewritten.append(bytes.constData() + 16 + static_cast<int>(old_header_bytes),
                     bytes.size() - 16 - static_cast<int>(old_header_bytes));

    QFile file(target);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(rewritten) != rewritten.size())
        return false;
    file.close();
    return true;
}

bool TestSnapshotEmptyLayerFill()
{
    std::vector<SliceLayer> layers;
    layers.push_back(MakeLayer(0, 1, 2));
    layers.push_back(MakeLayer(2, 1, 0));
    const SliceBuildSnapshot snapshot = MakeSnapshot(std::move(layers));
    return Require(snapshot.layers.size() == 3, "snapshot should fill grid 1") &&
           Require(snapshot.layers[0].grid_index == 0, "first grid index") &&
           Require(snapshot.layers[1].grid_index == 1, "filled grid index") &&
           Require(snapshot.layers[1].empty, "filled layer should be empty") &&
           Require(!snapshot.layers[2].empty, "last layer should not be empty") &&
           Require(snapshot.layers[1].index == 2, "filled layer index") &&
           Require(std::abs(snapshot.layers[1].z_mm - 0.045) < 1.0e-9,
                   "filled layer z should be mid plane") &&
           Require(snapshot.parts.size() == 2, "parts should be copied") &&
           Require(snapshot.source_project_revision == 42,
                   "source revision should be copied") &&
           Require(snapshot.settings.layer_thickness_mm == 0.03,
                   "settings should be copied");
}

bool TestRegistry()
{
    const SliceExportRegistry& registry = SliceExportRegistry::Instance();
    if (!Require(registry.Writers().size() == 5, "registry should have 5 writers"))
        return false;
    const SliceExportFormat formats[] = {
        SliceExportFormat::SlmSlice, SliceExportFormat::Cli,
        SliceExportFormat::Svg, SliceExportFormat::Png,
        SliceExportFormat::Csv
    };
    for (SliceExportFormat format : formats)
    {
        const IJobWriter* writer = registry.WriterFor(format);
        if (!Require(writer != nullptr, "registry writer lookup") ||
            !Require(!writer->name().isEmpty(), "writer name") ||
            !Require(!writer->fileFilter().isEmpty(), "writer filter") ||
            !Require(!writer->suffix().isEmpty(), "writer suffix") ||
            !Require(!SliceExportFormatName(format).isEmpty(),
                     "format name helper") ||
            !Require(!SliceExportFileFilter(format).isEmpty(),
                     "format filter helper") ||
            !Require(SliceExportSuffix(format) == writer->suffix(),
                     "format suffix helper"))
            return false;
    }
    return true;
}

bool TestSlmSliceRoundTrip(const QString& directory)
{
    std::vector<SliceLayer> layers;
    layers.push_back(MakeLayer(0, 2, 2));
    layers.push_back(MakeLayer(2, 1, 1));
    SliceBuildSnapshot snapshot = MakeSnapshot(std::move(layers));

    SliceExportOptions options;
    options.format = SliceExportFormat::SlmSlice;
    options.output_path = QDir(directory).filePath(QStringLiteral("roundtrip.slmslice"));
    options.include_empty_layers = true;
    options.validate_after_write = true;

    std::size_t progress_total = 0;
    const SliceExportResult result = ExportSliceBuild(
        snapshot, options,
        [&progress_total](std::size_t, std::size_t total)
        {
            progress_total = total;
            return true;
        });
    if (!Require(result.ok, std::string("slmslice export failed: ") +
                              result.error.toStdString()))
        return false;
    if (!Require(progress_total == 3, "slmslice progress total should include empty layer"))
        return false;
    if (!Require(QFileInfo(options.output_path).size() > 0, "slmslice output should exist"))
        return false;

    SliceBuildSnapshot loaded;
    QString error;
    if (!Require(ReadSlmSliceFile(options.output_path, loaded, error),
                 "slmslice read failed: " + error.toStdString()))
        return false;
    if (!Require(loaded.layers.size() == 3, "slmslice layer count") ||
        !Require(loaded.parts.size() == 2, "slmslice part count") ||
        !Require(loaded.parts[0].name == QStringLiteral("立方体"),
                 "slmslice unicode part name") ||
        !Require(loaded.source_project_revision == 42, "slmslice revision") ||
        !Require(loaded.settings.hatch.enabled, "slmslice hatch settings"))
        return false;
    if (!Require(loaded.layers[0].contours.size() == 2, "slmslice contours") ||
        !Require(loaded.layers[0].contours[1].role == SliceRingRole::Inner,
                 "slmslice inner role") ||
        !Require(loaded.layers[0].contours[1].model_id == 1,
                 "slmslice model id") ||
        !Require(loaded.layers[0].contours[0].closed, "slmslice closed flag") ||
        !Require(loaded.layers[0].hatches.size() == 2, "slmslice hatches") ||
        !Require(loaded.layers[1].empty, "slmslice empty layer"))
        return false;
    if (!Require(std::abs(loaded.layers[0].contour_path_length_mm - 80.0) <
                     1.0e-6,
                 "slmslice contour length") ||
        !Require(std::abs(loaded.layers[0].hatch_path_length_mm - 20.0) <
                     1.0e-6,
                 "slmslice hatch length"))
        return false;

    SliceFileInfo file_info;
    if (!Require(ReadSlmSliceFileInfo(options.output_path, file_info, error),
                 "slmslice info read failed: " + error.toStdString()))
        return false;
    if (!Require(file_info.header_json.contains(QStringLiteral("slmslice")),
                 "slmslice info header") ||
        !Require(file_info.parts.size() == 2, "slmslice info parts") ||
        !Require(file_info.layer_count == 3, "slmslice info layer count") ||
        !Require(file_info.empty_layer_count == 1, "slmslice info empty count") ||
        !Require(file_info.contour_count == 3, "slmslice info contour count") ||
        !Require(file_info.hatch_count == 3, "slmslice info hatch count") ||
        !Require(file_info.checksums.size() == 3, "slmslice info checksums") ||
        !Require(file_info.checksums[1].sha256.size() == 64,
                 "slmslice info checksum hex") ||
        !Require(std::abs(file_info.layer_thickness_mm - 0.03) < 1.0e-12,
                 "slmslice info thickness"))
        return false;
    const auto& source_points = snapshot.layers[0].contours[0].points;
    const auto& loaded_points = loaded.layers[0].contours[0].points;
    if (!Require(source_points.size() == loaded_points.size(),
                 "slmslice point count"))
        return false;
    for (std::size_t i = 0; i < source_points.size(); ++i)
        if (!Require(std::abs(source_points[i][0] - loaded_points[i][0]) < kTolerance &&
                     std::abs(source_points[i][1] - loaded_points[i][1]) < kTolerance,
                     "slmslice point coordinates"))
            return false;

    // Corrupt the last payload byte: checksum validation must reject it.
    QFile corrupted(options.output_path);
    if (!Require(corrupted.open(QIODevice::ReadWrite), "open slmslice for corruption"))
        return false;
    const qint64 size = corrupted.size();
    if (!Require(size > 20, "slmslice size"))
        return false;
    corrupted.seek(size - 1);
    char byte = 0;
    if (!Require(corrupted.getChar(&byte), "read slmslice last byte"))
        return false;
    byte = static_cast<char>(byte ^ 0x5a);
    corrupted.seek(size - 1);
    corrupted.putChar(byte);
    corrupted.close();
    SliceBuildSnapshot rejected;
    if (!Require(!ReadSlmSliceFile(options.output_path, rejected, error),
                 "corrupted slmslice should fail checksum"))
        return false;
    return true;
}

bool TestSlmSliceMultiModelRoundTrip(const QString& directory)
{
    SliceLayer layer;
    layer.grid_index = 0;
    layer.z_mm = 0.015;
    layer.contours.push_back(MakeRing(1, SliceRingRole::Outer, 0.0, 0.0, 10.0));
    layer.contours.push_back(MakeRing(1, SliceRingRole::Inner, 2.0, 2.0, 2.0));
    layer.contours.push_back(MakeRing(2, SliceRingRole::Outer, 20.0, 0.0, 5.0));
    layer.hatches.push_back(MakeHatch(2, 20.0, 0.0, 25.0, 0.0));
    const SliceBuildSnapshot snapshot = MakeSnapshot({layer});

    SliceExportOptions options;
    options.format = SliceExportFormat::SlmSlice;
    options.output_path =
        QDir(directory).filePath(QStringLiteral("multi-model.slmslice"));
    options.include_empty_layers = true;
    options.validate_after_write = true;
    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    if (!Require(result.ok, "multi-model slmslice export failed: " +
                                result.error.toStdString()))
        return false;

    SliceBuildSnapshot loaded;
    QString error;
    if (!Require(ReadSlmSliceFile(options.output_path, loaded, error),
                 "multi-model read failed: " + error.toStdString()))
        return false;
    if (!Require(loaded.parts.size() == 2, "multi-model part count"))
        return false;
    if (!Require(loaded.layers.size() == 1 && loaded.layers[0].contours.size() == 3,
                 "multi-model contour count"))
        return false;
    return Require(loaded.layers[0].contours[0].model_id == 1 &&
                       loaded.layers[0].contours[1].model_id == 1 &&
                       loaded.layers[0].contours[2].model_id == 2,
                   "multi-model ring attribution") &&
           Require(loaded.layers[0].hatches.size() == 1 &&
                       loaded.layers[0].hatches[0].model_id == 2,
                   "multi-model hatch attribution");
}

bool TestCliWriteAndParse(const QString& directory)
{
    std::vector<SliceLayer> layers;
    layers.push_back(MakeLayer(0, 2, 2));
    layers.push_back(MakeLayer(2, 1, 1));
    const SliceBuildSnapshot snapshot = MakeSnapshot(std::move(layers));

    SliceExportOptions options;
    options.format = SliceExportFormat::Cli;
    options.output_path = QDir(directory).filePath(QStringLiteral("roundtrip.cli"));
    options.include_empty_layers = true;
    options.validate_after_write = true;

    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    if (!Require(result.ok, std::string("CLI export failed: ") +
                              result.error.toStdString()))
        return false;

    QFile file(options.output_path);
    if (!Require(file.open(QIODevice::ReadOnly), "open CLI output"))
        return false;
    const QString content = QString::fromUtf8(file.readAll());
    if (!Require(content.contains(QStringLiteral("$$HEADERSTART")),
                 "CLI header start") ||
        !Require(content.contains(QStringLiteral("$$ASCII")), "CLI ASCII") ||
        !Require(content.contains(QStringLiteral("$$UNITS/1.0")), "CLI units") ||
        !Require(content.contains(QStringLiteral("$$LAYERS/2")), "CLI layer count") ||
        !Require(content.contains(QStringLiteral("$$POLYLINE/0,1,")),
                 "CLI outer CCW direction") ||
        !Require(content.contains(QStringLiteral("$$POLYLINE/0,0,")),
                 "CLI inner CW direction") ||
        !Require(content.contains(QStringLiteral("$$HATCHES/0,")),
                 "CLI hatches") ||
        !Require(!content.contains(QStringLiteral("$$LABEL")),
                 "CLI M1 should not write labels") ||
        !Require(content.contains(QStringLiteral("$$GEOMETRYEND")),
                 "CLI geometry end"))
        return false;

    SliceFileSummary summary;
    QString error;
    if (!Require(ParseCliAscii(options.output_path, summary, error),
                 "CLI parse failed: " + error.toStdString()))
        return false;
    if (!Require(summary.layers == 2, "CLI parsed layers") ||
        !Require(summary.contours == 3, "CLI parsed contours") ||
        !Require(summary.hatches == 3, "CLI parsed hatches"))
        return false;
    return true;
}

bool TestCliDecimalFormatting(const QString& directory)
{
    SliceLayer layer;
    layer.grid_index = 0;
    layer.z_mm = 0.015;
    SlicePath path;
    path.closed = true;
    path.role = SliceRingRole::Outer;
    path.points = {{1.23456789, -2.345678}, {10.0, -2.345678},
                   {10.0, 8.0}, {1.23456789, -2.345678}};
    layer.contours.push_back(path);
    const SliceBuildSnapshot snapshot = MakeSnapshot({layer});

    SliceExportOptions options;
    options.format = SliceExportFormat::Cli;
    options.output_path =
        QDir(directory).filePath(QStringLiteral("precision.cli"));
    options.validate_after_write = false;
    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    if (!Require(result.ok, "precision CLI export failed"))
        return false;
    QFile file(options.output_path);
    if (!Require(file.open(QIODevice::ReadOnly), "open precision CLI"))
        return false;
    const QString content = QString::fromUtf8(file.readAll());
    return Require(content.contains(QStringLiteral("1.2346")),
                   "CLI should round coordinates to 4 decimals") &&
           Require(content.contains(QStringLiteral("-2.3457")),
                   "CLI should round negative coordinates");
}

bool TestCliSkipsEmptyLayers(const QString& directory)
{
    std::vector<SliceLayer> layers;
    layers.push_back(MakeLayer(0, 1, 0));
    layers.push_back(MakeLayer(3, 1, 0));
    const SliceBuildSnapshot snapshot = MakeSnapshot(std::move(layers));
    SliceExportOptions options;
    options.format = SliceExportFormat::Cli;
    options.output_path = QDir(directory).filePath(QStringLiteral("empty.cli"));
    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    if (!Require(result.ok, "CLI empty-layer export failed"))
        return false;
    SliceFileSummary summary;
    QString error;
    return Require(ParseCliAscii(options.output_path, summary, error),
                   "CLI empty-layer parse") &&
           Require(summary.layers == 2, "CLI should skip empty layers");
}

bool TestCsvReport(const QString& directory)
{
    const SliceBuildSnapshot snapshot = MakeReportSnapshot();

    SliceExportOptions options;
    options.format = SliceExportFormat::Csv;
    options.output_path = QDir(directory).filePath(QStringLiteral("report.csv"));
    options.include_empty_layers = true;
    options.validate_after_write = true;
    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    if (!Require(result.ok, "CSV export failed: " +
                                result.error.toStdString()))
        return false;

    SliceFileSummary summary;
    QString error;
    if (!Require(SummarizeSliceFile(options.output_path,
                                    SliceExportFormat::Csv, summary, error),
                 "CSV summary failed: " + error.toStdString()))
        return false;
    if (!Require(summary.layers == 3, "CSV row count with empty layers") ||
        !Require(summary.contours == 3, "CSV contour count") ||
        !Require(summary.hatches == 3, "CSV hatch count") ||
        !Require(std::abs(summary.contour_length_mm - 120.0) <
                     kReportLengthTolerance,
                 "CSV contour length") ||
        !Require(std::abs(summary.hatch_length_mm - 30.0) <
                     kReportLengthTolerance,
                 "CSV hatch length"))
        return false;

    QFile file(options.output_path);
    if (!Require(file.open(QIODevice::ReadOnly), "open CSV"))
        return false;
    const QString content = QString::fromUtf8(file.readAll());
    if (!Require(content.startsWith(QStringLiteral(
                     "grid_index,z_mm,empty,contours,hatches,"
                     "contour_length_mm,hatch_length_mm,total_length_mm\n")),
                 "CSV header"))
        return false;
    if (!Require(content.count(QLatin1Char('\n')) == 4,
                 "CSV should contain header + 3 rows"))
        return false;
    if (!Require(content.contains(QStringLiteral(
                     "0,0.015000,0,2,2,80.000000,20.000000,100.000000")),
                 "CSV first layer row"))
        return false;
    if (!Require(content.contains(QStringLiteral(
                     "1,0.045000,1,0,0,0.000000,0.000000,0.000000")),
                 "CSV empty layer row"))
        return false;

    SliceExportOptions without_empty = options;
    without_empty.output_path =
        QDir(directory).filePath(QStringLiteral("report-no-empty.csv"));
    without_empty.include_empty_layers = false;
    const SliceExportResult second = ExportSliceBuild(snapshot, without_empty);
    if (!Require(second.ok, "CSV without-empty export failed"))
        return false;
    return Require(ValidateSliceFile(without_empty.output_path,
                                     SliceExportFormat::Csv, error),
                   "CSV without-empty validation") &&
           Require(SummarizeSliceFile(without_empty.output_path,
                                      SliceExportFormat::Csv, summary, error) &&
                       summary.layers == 2,
                   "CSV should skip empty layers when disabled");
}

bool TestSvgReport(const QString& directory)
{
    const SliceBuildSnapshot snapshot = MakeReportSnapshot();

    SliceExportOptions options;
    options.format = SliceExportFormat::Svg;
    options.output_path = QDir(directory).filePath(QStringLiteral("report.svg"));
    options.validate_after_write = true;
    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    if (!Require(result.ok, "SVG export failed: " +
                                result.error.toStdString()))
        return false;

    SliceFileSummary summary;
    QString error;
    if (!Require(SummarizeSliceFile(options.output_path,
                                    SliceExportFormat::Svg, summary, error),
                 "SVG summary failed: " + error.toStdString()))
        return false;
    if (!Require(summary.layers == 2, "SVG layer count") ||
        !Require(summary.contours == 3, "SVG contour count") ||
        !Require(summary.hatches == 3, "SVG hatch count") ||
        !Require(std::abs(summary.contour_length_mm - 120.0) <
                     kReportLengthTolerance,
                 "SVG contour length") ||
        !Require(std::abs(summary.hatch_length_mm - 30.0) <
                     kReportLengthTolerance,
                 "SVG hatch length"))
        return false;

    QFile file(options.output_path);
    if (!Require(file.open(QIODevice::ReadOnly), "open SVG"))
        return false;
    const QString content = QString::fromUtf8(file.readAll());
    if (!Require(content.startsWith(QStringLiteral("<?xml")),
                 "SVG XML prolog") ||
        !Require(content.contains(QStringLiteral("<desc>")),
                 "SVG metadata desc") ||
        !Require(content.contains(QStringLiteral("format=svg")),
                 "SVG metadata format") ||
        !Require(content.contains(QStringLiteral("layers=2")),
                 "SVG metadata layers") ||
        !Require(content.count(QStringLiteral("class=\"contour outer\"")) == 2,
                 "SVG outer contour elements") ||
        !Require(content.count(QStringLiteral("class=\"contour inner\"")) == 1,
                 "SVG inner contour elements") ||
        !Require(content.count(QStringLiteral("class=\"hatch\"")) == 3,
                 "SVG hatch elements") ||
        !Require(content.contains(QStringLiteral("L0 z=0.015")),
                 "SVG layer label"))
        return false;

    // A truncated SVG (metadata stripped) must fail validation.
    QFile broken(options.output_path);
    if (!Require(broken.open(QIODevice::ReadWrite), "open SVG for corruption"))
        return false;
    QByteArray bytes = broken.readAll();
    const int desc = bytes.indexOf("<desc>");
    if (desc < 0)
        return false;
    bytes.remove(desc, 32);
    broken.resize(0);
    broken.seek(0);
    broken.write(bytes);
    broken.close();
    return Require(!SummarizeSliceFile(options.output_path,
                                      SliceExportFormat::Svg, summary, error),
                   "SVG without metadata should fail validation");
}

bool TestPngReport(const QString& directory)
{
    const SliceBuildSnapshot snapshot = MakeReportSnapshot();

    SliceExportOptions options;
    options.format = SliceExportFormat::Png;
    options.output_path = QDir(directory).filePath(QStringLiteral("report.png"));
    options.validate_after_write = true;
    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    if (!Require(result.ok, "PNG export failed: " +
                                result.error.toStdString()))
        return false;

    SliceFileSummary summary;
    QString error;
    if (!Require(SummarizeSliceFile(options.output_path,
                                    SliceExportFormat::Png, summary, error),
                 "PNG summary failed: " + error.toStdString()))
        return false;
    if (!Require(summary.layers == 2, "PNG layer count") ||
        !Require(summary.contours == 3, "PNG contour count") ||
        !Require(summary.hatches == 3, "PNG hatch count") ||
        !Require(std::abs(summary.contour_length_mm - 120.0) <
                     kReportLengthTolerance,
                 "PNG contour length"))
        return false;

    QByteArray bytes;
    if (!Require(ReadFileBytes(options.output_path, bytes), "read PNG bytes"))
        return false;
    if (!Require(bytes.size() > 32, "PNG file size") ||
        !Require(static_cast<unsigned char>(bytes.at(0)) == 0x89 &&
                     bytes.mid(1, 3) == QByteArrayLiteral("PNG"),
                 "PNG signature"))
        return false;
    if (!Require(bytes.mid(12, 4) == QByteArrayLiteral("IHDR"),
                 "PNG IHDR chunk"))
        return false;

    // Flip one byte inside a tEXt/IDAT chunk: the chunk CRC check must reject.
    QByteArray corrupted = bytes;
    corrupted[corrupted.size() - 20] =
        static_cast<char>(corrupted.at(corrupted.size() - 20) ^ 0x33);
    const QString corrupted_path =
        QDir(directory).filePath(QStringLiteral("report-corrupted.png"));
    QFile broken(corrupted_path);
    if (!Require(broken.open(QIODevice::WriteOnly), "write corrupted PNG"))
        return false;
    broken.write(corrupted);
    broken.close();
    return Require(!SummarizeSliceFile(corrupted_path, SliceExportFormat::Png,
                                       summary, error),
                   "corrupted PNG should fail CRC validation");
}

bool TestReportOptionsHonored(const QString& directory)
{
    const SliceBuildSnapshot snapshot = MakeReportSnapshot();
    const SliceExportFormat formats[] = {
        SliceExportFormat::Svg, SliceExportFormat::Png,
        SliceExportFormat::Csv
    };
    for (SliceExportFormat format : formats)
    {
        SliceExportOptions options;
        options.format = format;
        options.output_path = QDir(directory).filePath(
            QStringLiteral("no-hatches.%1").arg(SliceExportSuffix(format)));
        options.include_hatches = false;
        options.validate_after_write = true;
        const SliceExportResult result = ExportSliceBuild(snapshot, options);
        if (!Require(result.ok, "no-hatch export failed for " +
                                    SliceExportSuffix(format).toStdString() +
                                    ": " + result.error.toStdString()))
            return false;
        SliceFileSummary summary;
        QString error;
        if (!Require(SummarizeSliceFile(options.output_path, format, summary,
                                        error),
                     "no-hatch summary"))
            return false;
        if (!Require(summary.hatches == 0, "no-hatch hatch count") ||
            !Require(std::abs(summary.hatch_length_mm) < 1.0e-9,
                     "no-hatch hatch length") ||
            !Require(summary.contours == 3, "no-hatch contour count"))
            return false;
    }
    return true;
}

bool TestDeterministicOutputs(const QString& directory)
{
    const SliceBuildSnapshot snapshot = MakeReportSnapshot();
    const SliceExportFormat formats[] = {
        SliceExportFormat::Cli, SliceExportFormat::Svg,
        SliceExportFormat::Png, SliceExportFormat::Csv
    };
    for (SliceExportFormat format : formats)
    {
        const QString suffix = SliceExportSuffix(format);
        SliceExportOptions options;
        options.format = format;
        options.validate_after_write = false;
        options.output_path = QDir(directory).filePath(
            QStringLiteral("deterministic-a.%1").arg(suffix));
        if (!Require(ExportSliceBuild(snapshot, options).ok,
                     "first deterministic export failed"))
            return false;
        options.output_path = QDir(directory).filePath(
            QStringLiteral("deterministic-b.%1").arg(suffix));
        if (!Require(ExportSliceBuild(snapshot, options).ok,
                     "second deterministic export failed"))
            return false;
        QByteArray first;
        QByteArray second;
        if (!Require(ReadFileBytes(QDir(directory).filePath(
                         QStringLiteral("deterministic-a.%1").arg(suffix)),
                     first),
                     "read first deterministic output") ||
            !Require(ReadFileBytes(QDir(directory).filePath(
                         QStringLiteral("deterministic-b.%1").arg(suffix)),
                     second),
                     "read second deterministic output"))
            return false;
        if (!Require(first == second,
                     "same snapshot must produce identical " +
                         suffix.toStdString() + " output"))
            return false;
    }
    return true;
}

bool TestCancellationKeepsOldFile(const QString& directory)
{
    std::vector<SliceLayer> layers;
    layers.push_back(MakeLayer(0, 1, 0));
    layers.push_back(MakeLayer(1, 1, 0));
    const SliceBuildSnapshot snapshot = MakeSnapshot(std::move(layers));
    const QString path = QDir(directory).filePath(QStringLiteral("cancel.cli"));
    QFile old(path);
    if (!Require(old.open(QIODevice::WriteOnly), "create old file"))
        return false;
    old.write("old-content");
    old.close();

    SliceExportOptions options;
    options.format = SliceExportFormat::Cli;
    options.output_path = path;
    options.validate_after_write = false;
    std::size_t calls = 0;
    const SliceExportResult result = ExportSliceBuild(
        snapshot, options, [&calls](std::size_t, std::size_t)
        {
            return ++calls <= 1;
        });
    QFile check(path);
    if (!Require(check.open(QIODevice::ReadOnly), "read old file"))
        return false;
    const QByteArray bytes = check.readAll();
    return Require(result.cancelled, "cancellation should be reported") &&
           Require(!result.ok, "cancelled export should not be ok") &&
           Require(bytes == QByteArray("old-content"),
                   "cancelled export must keep old file");
}

bool TestSlmSliceCancellationKeepsOldFile(const QString& directory)
{
    std::vector<SliceLayer> layers;
    layers.push_back(MakeLayer(0, 1, 0));
    layers.push_back(MakeLayer(1, 1, 0));
    const SliceBuildSnapshot snapshot = MakeSnapshot(std::move(layers));
    const QString path =
        QDir(directory).filePath(QStringLiteral("cancel.slmslice"));
    QFile old(path);
    if (!Require(old.open(QIODevice::WriteOnly), "create old slmslice"))
        return false;
    old.write("old-slice");
    old.close();

    SliceExportOptions options;
    options.format = SliceExportFormat::SlmSlice;
    options.output_path = path;
    options.validate_after_write = false;
    std::size_t calls = 0;
    const SliceExportResult result = ExportSliceBuild(
        snapshot, options, [&calls](std::size_t, std::size_t)
        {
            return ++calls <= 1;
        });
    QFile check(path);
    if (!Require(check.open(QIODevice::ReadOnly), "read old slmslice"))
        return false;
    const QByteArray bytes = check.readAll();
    return Require(result.cancelled, "slmslice cancellation should be reported") &&
           Require(!result.ok, "cancelled slmslice should not be ok") &&
           Require(bytes == QByteArray("old-slice"),
                   "cancelled slmslice export must keep old file");
}

bool TestWriteFailureKeepsExistingFile(const QString& directory)
{
    const SliceBuildSnapshot snapshot = MakeReportSnapshot();
    const QString blocker =
        QDir(directory).filePath(QStringLiteral("blocked"));
    QFile file(blocker);
    if (!Require(file.open(QIODevice::WriteOnly), "create blocker file"))
        return false;
    file.write("sentinel");
    file.close();

    SliceExportOptions options;
    options.format = SliceExportFormat::SlmSlice;
    options.output_path =
        QDir(directory).filePath(QStringLiteral("blocked/out.slmslice"));
    options.validate_after_write = false;
    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    QFile check(blocker);
    if (!Require(check.open(QIODevice::ReadOnly), "read blocker file"))
        return false;
    return Require(!result.ok, "export below a file must fail") &&
           Require(!result.error.isEmpty(), "write failure should report an error") &&
           Require(check.readAll() == QByteArray("sentinel"),
                   "failed export must not modify unrelated files");
}

bool TestInvalidPathFails(const QString& directory)
{
    const SliceBuildSnapshot snapshot = MakeSnapshot({MakeLayer(0, 1, 1)});
    SliceExportOptions options;
    options.format = SliceExportFormat::SlmSlice;
    options.output_path = QDir(directory).filePath(QStringLiteral("missing/out.slmslice"));
    options.validate_after_write = false;
    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    return Require(!result.ok, "export to missing directory should fail") &&
           Require(!result.error.isEmpty(), "invalid path should report an error");
}

// A support part must keep its kind through .slmslice export/read-back while the
// payload (layers, contours, hatches, lengths) stays exactly the v1 geometry.
bool TestSlmSliceSupportPartRoundTrip(const QString& directory)
{
    const SliceBuildSnapshot snapshot = MakeSupportSnapshot();
    SliceExportOptions options;
    options.format = SliceExportFormat::SlmSlice;
    options.output_path =
        QDir(directory).filePath(QStringLiteral("support.slmslice"));
    options.include_empty_layers = true;
    options.validate_after_write = true;
    const SliceExportResult result = ExportSliceBuild(snapshot, options);
    if (!Require(result.ok, "support slmslice export failed: " +
                                result.error.toStdString()))
        return false;
    if (!Require(result.contours_written == 2 && result.hatches_written == 2,
                 "support export geometry counters"))
        return false;

    // Header: v1 unchanged, only the support part is tagged; model parts must
    // not gain an optional field so support-free headers stay identical.
    QJsonObject header;
    if (!Require(ReadSlmSliceHeader(options.output_path, header),
                 "read support slmslice header"))
        return false;
    if (!Require(header.value(QStringLiteral("version")).toInt() == 1,
                 "support slmslice header version"))
        return false;
    const QJsonArray header_parts =
        header.value(QStringLiteral("parts")).toArray();
    if (!Require(header_parts.size() == 3, "support header part count"))
        return false;
    for (int i = 0; i < 2; ++i)
        if (!Require(!header_parts.at(i).toObject().contains(
                         QStringLiteral("kind")),
                     "model part must omit the optional kind field"))
            return false;
    if (!Require(header_parts.at(2).toObject()
                         .value(QStringLiteral("kind")).toString() ==
                     QStringLiteral("support"),
                 "support part header kind"))
        return false;

    SliceBuildSnapshot loaded;
    QString error;
    if (!Require(ReadSlmSliceFile(options.output_path, loaded, error),
                 "support slmslice read failed: " + error.toStdString()))
        return false;
    if (!Require(loaded.parts.size() == 3, "support read part count") ||
        !Require(loaded.parts[0].kind == SlicePartKind::Model,
                 "first model part kind roundtrip") ||
        !Require(loaded.parts[1].kind == SlicePartKind::Model,
                 "second model part kind roundtrip") ||
        !Require(loaded.parts[2].kind == SlicePartKind::Support,
                 "support part kind roundtrip") ||
        !Require(loaded.parts[2].id == kSupportPartId,
                 "support part id roundtrip") ||
        !Require(loaded.parts[2].name == QStringLiteral("立方体 支撑"),
                 "support part name roundtrip") ||
        !Require(loaded.parts[2].triangle_count == 64,
                 "support part triangle count roundtrip"))
        return false;

    SliceFileInfo info;
    if (!Require(ReadSlmSliceFileInfo(options.output_path, info, error),
                 "support slmslice info read failed: " + error.toStdString()))
        return false;
    if (!Require(info.parts.size() == 3, "support info part count") ||
        !Require(info.parts[0].kind == SlicePartKind::Model,
                 "support info model part kind") ||
        !Require(info.parts[2].kind == SlicePartKind::Support,
                 "support info part kind"))
        return false;

    if (!Require(loaded.layers.size() == snapshot.layers.size(),
                 "support layer count roundtrip"))
        return false;
    for (std::size_t i = 0; i < snapshot.layers.size(); ++i)
    {
        if (!Require(loaded.layers[i].contours.size() ==
                         snapshot.layers[i].contours.size(),
                     "support contour count roundtrip") ||
            !Require(loaded.layers[i].hatches.size() ==
                         snapshot.layers[i].hatches.size(),
                     "support hatch count roundtrip") ||
            !Require(std::abs(loaded.layers[i].contour_path_length_mm -
                              SliceLayerContourLengthMm(snapshot.layers[i])) <
                         kTolerance,
                     "support contour length roundtrip") ||
            !Require(std::abs(loaded.layers[i].hatch_path_length_mm -
                              SliceLayerHatchLengthMm(snapshot.layers[i])) <
                         kTolerance,
                     "support hatch length roundtrip"))
            return false;
    }
    return Require(loaded.layers[0].contours[1].model_id == kSupportPartId,
                   "support contour model id roundtrip") &&
           Require(loaded.layers[0].hatches[1].model_id == kSupportPartId,
                   "support hatch model id roundtrip") &&
           Require(std::abs(loaded.layers[0].contour_path_length_mm - 60.0) <
                       kTolerance,
                   "support contour length total") &&
           Require(std::abs(loaded.layers[0].hatch_path_length_mm - 15.0) <
                       kTolerance,
                   "support hatch length total");
}

// Backward/forward compatibility of the optional header field: files without
// "kind" (written by an older build, or hand-made) and files with unknown kind
// values must still read as Model parts instead of failing.
bool TestSlmSlicePartKindCompatibility(const QString& directory)
{
    // 1. A model-only snapshot writes exactly the pre-support v1 header.
    const SliceBuildSnapshot model_snapshot = MakeReportSnapshot();
    SliceExportOptions options;
    options.format = SliceExportFormat::SlmSlice;
    options.output_path =
        QDir(directory).filePath(QStringLiteral("legacy-parts.slmslice"));
    options.include_empty_layers = true;
    options.validate_after_write = true;
    const SliceExportResult legacy = ExportSliceBuild(model_snapshot, options);
    if (!Require(legacy.ok, "legacy parts export failed: " +
                                legacy.error.toStdString()))
        return false;

    QJsonObject header;
    if (!Require(ReadSlmSliceHeader(options.output_path, header),
                 "read legacy parts header"))
        return false;
    const QJsonArray legacy_parts =
        header.value(QStringLiteral("parts")).toArray();
    for (int i = 0; i < legacy_parts.size(); ++i)
        if (!Require(!legacy_parts.at(i).toObject().contains(
                         QStringLiteral("kind")),
                     "model-only header must not contain kind"))
            return false;

    SliceBuildSnapshot loaded;
    QString error;
    if (!Require(ReadSlmSliceFile(options.output_path, loaded, error),
                 "legacy slmslice read failed: " + error.toStdString()))
        return false;
    for (const SlicePartInfo& part : loaded.parts)
        if (!Require(part.kind == SlicePartKind::Model,
                     "missing kind must read as model"))
            return false;
    SliceFileInfo info;
    if (!Require(ReadSlmSliceFileInfo(options.output_path, info, error),
                 "legacy slmslice info read failed: " + error.toStdString()))
        return false;
    for (const SlicePartInfo& part : info.parts)
        if (!Require(part.kind == SlicePartKind::Model,
                     "missing info kind must read as model"))
            return false;

    // 2. Strip "kind" from a support file: an old reader (and our tolerant
    //    reader) sees plain model parts and the geometry is unchanged.
    const SliceBuildSnapshot support_snapshot = MakeSupportSnapshot();
    options.output_path =
        QDir(directory).filePath(QStringLiteral("support-full.slmslice"));
    const SliceExportResult full = ExportSliceBuild(support_snapshot, options);
    if (!Require(full.ok, "support reference export failed: " +
                              full.error.toStdString()))
        return false;

    const QString stripped_path =
        QDir(directory).filePath(QStringLiteral("support-stripped.slmslice"));
    if (!Require(RewriteSlmSliceParts(
                     options.output_path, stripped_path,
                     [](QJsonObject& part)
                     { part.remove(QStringLiteral("kind")); }),
                 "strip kind from support header"))
        return false;
    SliceBuildSnapshot stripped;
    if (!Require(ReadSlmSliceFile(stripped_path, stripped, error),
                 "stripped-kind read failed: " + error.toStdString()))
        return false;
    if (!Require(stripped.parts.size() == support_snapshot.parts.size(),
                 "stripped-kind part count") ||
        !Require(stripped.parts.back().kind == SlicePartKind::Model,
                 "stripped support kind must read as model") ||
        !Require(stripped.layers.size() == support_snapshot.layers.size(),
                 "stripped-kind layer count") ||
        !Require(stripped.layers[0].contours.size() == 2 &&
                     stripped.layers[0].hatches.size() == 2,
                 "stripped-kind geometry unchanged"))
        return false;

    // 3. Unknown kind values are tolerated and fall back to Model.
    const QString unknown_path =
        QDir(directory).filePath(QStringLiteral("support-unknown-kind.slmslice"));
    if (!Require(RewriteSlmSliceParts(
                     options.output_path, unknown_path,
                     [](QJsonObject& part)
                     {
                         if (part.value(QStringLiteral("kind")).toString() ==
                             QStringLiteral("support"))
                             part.insert(QStringLiteral("kind"),
                                         QStringLiteral("composite"));
                     }),
                 "rewrite unknown support kind"))
        return false;
    SliceBuildSnapshot unknown;
    if (!Require(ReadSlmSliceFile(unknown_path, unknown, error),
                 "unknown-kind read failed: " + error.toStdString()))
        return false;
    if (!Require(unknown.parts.back().kind == SlicePartKind::Model,
                 "unknown kind must read as model"))
        return false;
    SliceFileInfo unknown_info;
    return Require(ReadSlmSliceFileInfo(unknown_path, unknown_info, error),
                   "unknown-kind info read failed: " + error.toStdString()) &&
           Require(unknown_info.parts.back().kind == SlicePartKind::Model,
                   "unknown info kind must read as model");
}

// CLI/CSV reports do not know about part kinds: support geometry is counted
// like any other geometry (CLI ids stay 0, no $$LABEL), so the existing report
// structure and count conventions are preserved.
bool TestReportFormatsWithSupportParts(const QString& directory)
{
    const SliceBuildSnapshot snapshot = MakeSupportSnapshot();

    SliceExportOptions options;
    options.format = SliceExportFormat::Cli;
    options.output_path =
        QDir(directory).filePath(QStringLiteral("support.cli"));
    options.validate_after_write = true;
    const SliceExportResult cli = ExportSliceBuild(snapshot, options);
    if (!Require(cli.ok, "support CLI export failed: " +
                             cli.error.toStdString()))
        return false;
    QFile cli_file(options.output_path);
    if (!Require(cli_file.open(QIODevice::ReadOnly), "open support CLI"))
        return false;
    const QString cli_text = QString::fromUtf8(cli_file.readAll());
    cli_file.close();
    if (!Require(cli_text.contains(QStringLiteral("$$LAYERS/1")),
                 "support CLI layer count") ||
        !Require(cli_text.contains(QStringLiteral("$$POLYLINE/0,1,")),
                 "support CLI polyline id stays 0") ||
        !Require(cli_text.contains(QStringLiteral("$$HATCHES/0,")),
                 "support CLI hatches id stays 0") ||
        !Require(!cli_text.contains(QStringLiteral("$$LABEL")),
                 "support CLI must not write part labels") ||
        !Require(cli_text.contains(QStringLiteral("$$GEOMETRYEND")),
                 "support CLI geometry end"))
        return false;

    SliceFileSummary summary;
    QString error;
    if (!Require(SummarizeSliceFile(options.output_path,
                                    SliceExportFormat::Cli, summary, error),
                 "support CLI summarize failed: " + error.toStdString()))
        return false;
    if (!Require(summary.layers == 1 && summary.contours == 2 &&
                     summary.hatches == 2,
                 "support CLI summary counts"))
        return false;

    options.format = SliceExportFormat::Csv;
    options.output_path =
        QDir(directory).filePath(QStringLiteral("support.csv"));
    options.include_empty_layers = true;
    const SliceExportResult csv = ExportSliceBuild(snapshot, options);
    if (!Require(csv.ok, "support CSV export failed: " +
                             csv.error.toStdString()))
        return false;
    if (!Require(SummarizeSliceFile(options.output_path,
                                    SliceExportFormat::Csv, summary, error),
                 "support CSV summarize failed: " + error.toStdString()))
        return false;
    if (!Require(summary.layers == 1 && summary.contours == 2 &&
                     summary.hatches == 2,
                 "support CSV summary counts") ||
        !Require(std::abs(summary.contour_length_mm - 60.0) <
                     kReportLengthTolerance,
                 "support CSV contour length") ||
        !Require(std::abs(summary.hatch_length_mm - 15.0) <
                     kReportLengthTolerance,
                 "support CSV hatch length"))
        return false;
    QFile csv_file(options.output_path);
    if (!Require(csv_file.open(QIODevice::ReadOnly), "open support CSV"))
        return false;
    const QString csv_text = QString::fromUtf8(csv_file.readAll());
    return Require(csv_text.startsWith(QStringLiteral(
                       "grid_index,z_mm,empty,contours,hatches,"
                       "contour_length_mm,hatch_length_mm,total_length_mm\n")),
                   "support CSV header unchanged") &&
           Require(csv_text.contains(QStringLiteral(
                       "0,0.015000,0,2,2,60.000000,15.000000,75.000000")),
                   "support CSV data row counts all geometry");
}

// Optional debugging hook: set SLMSLICER_EXPORT_TEST_ARTIFACTS to a directory
// to keep one sample per report format for inspection with external tools.
bool ExportSampleArtifacts(const QString& directory)
{
    const QString target = qEnvironmentVariable("SLMSLICER_EXPORT_TEST_ARTIFACTS");
    if (target.isEmpty())
        return true;
    if (!Require(QDir().mkpath(target), "create artifact directory"))
        return false;
    const SliceBuildSnapshot snapshot = MakeReportSnapshot();
    const SliceExportFormat formats[] = {
        SliceExportFormat::SlmSlice, SliceExportFormat::Cli,
        SliceExportFormat::Svg, SliceExportFormat::Png,
        SliceExportFormat::Csv
    };
    for (SliceExportFormat format : formats)
    {
        SliceExportOptions options;
        options.format = format;
        options.output_path = QDir(target).filePath(
            QStringLiteral("sample.%1").arg(SliceExportSuffix(format)));
        options.validate_after_write = false;
        const SliceExportResult result = ExportSliceBuild(snapshot, options);
        if (!Require(result.ok, "artifact export failed: " +
                                    result.error.toStdString()))
            return false;
    }
    // Extra sample with a support part so the optional header kind can be
    // inspected with external tools.
    SliceExportOptions support_options;
    support_options.format = SliceExportFormat::SlmSlice;
    support_options.output_path = QDir(target).filePath(
        QStringLiteral("sample-support.slmslice"));
    support_options.validate_after_write = false;
    if (!Require(ExportSliceBuild(MakeSupportSnapshot(), support_options).ok,
                 "support artifact export failed"))
        return false;
    (void)directory;
    return true;
}
} // namespace

int main()
{
    QTemporaryDir directory;
    if (!directory.isValid())
    {
        std::cerr << "FAILED: cannot create temporary directory\n";
        return 1;
    }

    if (!TestSnapshotEmptyLayerFill())
        return 2;
    if (!TestRegistry())
        return 3;
    if (!TestSlmSliceRoundTrip(directory.path()))
        return 4;
    if (!TestSlmSliceMultiModelRoundTrip(directory.path()))
        return 5;
    if (!TestCliWriteAndParse(directory.path()))
        return 6;
    if (!TestCliDecimalFormatting(directory.path()))
        return 7;
    if (!TestCliSkipsEmptyLayers(directory.path()))
        return 8;
    if (!TestCsvReport(directory.path()))
        return 9;
    if (!TestSvgReport(directory.path()))
        return 10;
    if (!TestPngReport(directory.path()))
        return 11;
    if (!TestReportOptionsHonored(directory.path()))
        return 12;
    if (!TestDeterministicOutputs(directory.path()))
        return 13;
    if (!TestCancellationKeepsOldFile(directory.path()))
        return 14;
    if (!TestSlmSliceCancellationKeepsOldFile(directory.path()))
        return 15;
    if (!TestWriteFailureKeepsExistingFile(directory.path()))
        return 16;
    if (!TestInvalidPathFails(directory.path()))
        return 17;
    if (!TestSlmSliceSupportPartRoundTrip(directory.path()))
        return 18;
    if (!TestSlmSlicePartKindCompatibility(directory.path()))
        return 19;
    if (!TestReportFormatsWithSupportParts(directory.path()))
        return 20;
    if (!ExportSampleArtifacts(directory.path()))
        return 21;

    std::cout << "PASSED\n";
    return 0;
}
