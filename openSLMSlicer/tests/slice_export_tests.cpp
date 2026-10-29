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
    if (!ExportSampleArtifacts(directory.path()))
        return 18;

    std::cout << "PASSED\n";
    return 0;
}
