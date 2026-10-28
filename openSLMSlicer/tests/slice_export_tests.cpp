#include "slice_export.h"
#include "cli_format.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr double kTolerance = 1.0e-9;

bool Require(bool condition, const std::string& message)
{
    if (!condition)
        std::cerr << "FAILED: " << message << '\n';
    return condition;
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
    if (!TestSlmSliceRoundTrip(directory.path()))
        return 3;
    if (!TestCliWriteAndParse(directory.path()))
        return 4;
    if (!TestCliSkipsEmptyLayers(directory.path()))
        return 5;
    if (!TestCancellationKeepsOldFile(directory.path()))
        return 6;
    if (!TestInvalidPathFails(directory.path()))
        return 7;

    std::cout << "PASSED\n";
    return 0;
}
