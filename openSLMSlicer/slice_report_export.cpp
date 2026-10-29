#include "slice_report_export.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
constexpr int kMaxReportDimension = 30000;
constexpr qint64 kMaxReportPixels = 16'000'000;
constexpr std::size_t kMaxPatternLength = 32;

// Palette indices of the PNG report.
enum ReportColor : unsigned char
{
    kColorBackground = 0,
    kColorHatch = 1,
    kColorOuter = 2,
    kColorInner = 3,
    kColorBorder = 4
};

constexpr unsigned char kReportPalette[5][3] = {
    {255, 255, 255},
    {158, 158, 158},
    {21, 62, 117},
    {176, 32, 32},
    {216, 216, 216}
};

constexpr unsigned char kPngSignature[8] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
};

QString FixedNumber(double value, int decimals)
{
    if (!std::isfinite(value))
        value = 0.0;
    return QString::number(value, 'f', decimals);
}

std::vector<const SliceLayer*> NonEmptyLayers(const SliceBuildSnapshot& snapshot)
{
    std::vector<const SliceLayer*> layers;
    layers.reserve(snapshot.layers.size());
    for (const SliceLayer& layer : snapshot.layers)
        if (!layer.empty)
            layers.push_back(&layer);
    return layers;
}

struct ReportCounts
{
    std::size_t layers = 0;
    std::size_t contours = 0;
    std::size_t hatches = 0;
    double contour_length_mm = 0.0;
    double hatch_length_mm = 0.0;
};

ReportCounts CountLayers(const std::vector<const SliceLayer*>& layers,
                          bool include_hatches)
{
    ReportCounts counts;
    counts.layers = layers.size();
    for (const SliceLayer* layer : layers)
    {
        counts.contours += layer->contours.size();
        counts.contour_length_mm += SliceLayerContourLengthMm(*layer);
        if (include_hatches)
        {
            counts.hatches += layer->hatches.size();
            counts.hatch_length_mm += SliceLayerHatchLengthMm(*layer);
        }
    }
    return counts;
}

bool ComputeLayerBounds(const SliceLayer& layer, bool include_hatches,
                        double& min_x, double& min_y,
                        double& max_x, double& max_y)
{
    bool any = false;
    auto add = [&](double x, double y)
    {
        if (!std::isfinite(x) || !std::isfinite(y))
            return;
        if (!any)
        {
            min_x = max_x = x;
            min_y = max_y = y;
            any = true;
            return;
        }
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    };

    for (const SlicePath& path : layer.contours)
        for (const auto& point : path.points)
            add(point[0], point[1]);
    if (include_hatches)
    {
        for (const SliceHatchLine& line : layer.hatches)
        {
            add(line.start[0], line.start[1]);
            add(line.end[0], line.end[1]);
        }
    }
    return any;
}

QString BuildReportMetadata(const QString& format, const ReportCounts& counts,
                            const SliceReportLayout& layout,
                            const SliceLayer* first, const SliceLayer* last,
                            bool include_hatches, qint64 raw_bytes)
{
    QString text =
        QStringLiteral("version=1;format=%1;layers=%2;contours=%3;hatches=%4")
            .arg(format)
            .arg(static_cast<qulonglong>(counts.layers))
            .arg(static_cast<qulonglong>(counts.contours))
            .arg(static_cast<qulonglong>(counts.hatches));
    text += QStringLiteral(";contour_length_mm=%1;hatch_length_mm=%2")
                .arg(FixedNumber(counts.contour_length_mm, 3),
                     FixedNumber(counts.hatch_length_mm, 3));
    text += QStringLiteral(";columns=%1;rows=%2;cell=%3;width=%4;height=%5")
                .arg(layout.columns)
                .arg(layout.rows)
                .arg(layout.cell_px)
                .arg(layout.width_px)
                .arg(layout.height_px);
    text += QStringLiteral(";first_grid=%1;last_grid=%2;include_hatches=%3")
                .arg(first ? first->grid_index : 0)
                .arg(last ? last->grid_index : 0)
                .arg(include_hatches ? 1 : 0);
    if (raw_bytes >= 0)
        text += QStringLiteral(";raw_bytes=%1")
                    .arg(static_cast<qulonglong>(raw_bytes));
    return text;
}

bool ParseReportMetadata(const QByteArray& bytes, QHash<QString, QString>& values)
{
    const QString text = QString::fromUtf8(bytes).trimmed();
    if (text.isEmpty())
        return false;
    const QStringList fields = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& field : fields)
    {
        const int equals = field.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        values.insert(field.left(equals).trimmed(), field.mid(equals + 1).trimmed());
    }
    return !values.isEmpty();
}

bool RequiredSize(const QHash<QString, QString>& values, const QString& key,
                  std::size_t& out, QString& error)
{
    const auto it = values.constFind(key);
    bool ok = false;
    const qulonglong value = it == values.constEnd() ? 0 : it.value().toULongLong(&ok);
    if (it == values.constEnd() || !ok)
    {
        error = QStringLiteral("报表元数据缺少字段 %1。").arg(key);
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

bool RequiredDouble(const QHash<QString, QString>& values, const QString& key,
                    double& out, QString& error)
{
    const auto it = values.constFind(key);
    bool ok = false;
    const double value = it == values.constEnd() ? 0.0 : it.value().toDouble(&ok);
    if (it == values.constEnd() || !ok)
    {
        error = QStringLiteral("报表元数据缺少字段 %1。").arg(key);
        return false;
    }
    out = value;
    return true;
}

void SetPixel(std::vector<unsigned char>& pixels, int width, int height,
              int x, int y, unsigned char color)
{
    if (x < 0 || y < 0 || x >= width || y >= height)
        return;
    pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)] = color;
}

void DrawLine(std::vector<unsigned char>& pixels, int width, int height,
              double x0, double y0, double x1, double y1, unsigned char color)
{
    if (!std::isfinite(x0) || !std::isfinite(y0) ||
        !std::isfinite(x1) || !std::isfinite(y1))
        return;
    int px0 = static_cast<int>(std::lround(x0));
    int py0 = static_cast<int>(std::lround(y0));
    const int px1 = static_cast<int>(std::lround(x1));
    const int py1 = static_cast<int>(std::lround(y1));
    const int dx = std::abs(px1 - px0);
    const int sx = px0 < px1 ? 1 : -1;
    const int dy = -std::abs(py1 - py0);
    const int sy = py0 < py1 ? 1 : -1;
    int error = dx + dy;
    for (;;)
    {
        SetPixel(pixels, width, height, px0, py0, color);
        if (px0 == px1 && py0 == py1)
            break;
        const int doubled = 2 * error;
        if (doubled >= dy)
        {
            error += dy;
            px0 += sx;
        }
        if (doubled <= dx)
        {
            error += dx;
            py0 += sy;
        }
    }
}

void DrawRectOutline(std::vector<unsigned char>& pixels, int width, int height,
                     int x, int y, int rect_width, int rect_height,
                     unsigned char color)
{
    if (rect_width <= 0 || rect_height <= 0)
        return;
    DrawLine(pixels, width, height, x, y, x + rect_width - 1, y, color);
    DrawLine(pixels, width, height, x, y + rect_height - 1,
             x + rect_width - 1, y + rect_height - 1, color);
    DrawLine(pixels, width, height, x, y, x, y + rect_height - 1, color);
    DrawLine(pixels, width, height, x + rect_width - 1, y,
             x + rect_width - 1, y + rect_height - 1, color);
}

quint32 Crc32(const unsigned char* data, std::size_t length)
{
    static quint32 table[256];
    static bool initialized = false;
    if (!initialized)
    {
        for (quint32 i = 0; i < 256; ++i)
        {
            quint32 value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1u) ? (0xedb88320u ^ (value >> 1)) : (value >> 1);
            table[i] = value;
        }
        initialized = true;
    }
    quint32 crc = 0xffffffffu;
    for (std::size_t i = 0; i < length; ++i)
        crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

void AppendU32(QByteArray& bytes, quint32 value)
{
    bytes.append(static_cast<char>((value >> 24) & 0xffu));
    bytes.append(static_cast<char>((value >> 16) & 0xffu));
    bytes.append(static_cast<char>((value >> 8) & 0xffu));
    bytes.append(static_cast<char>(value & 0xffu));
}

quint32 ReadU32(const QByteArray& bytes, int offset)
{
    const auto* data = reinterpret_cast<const unsigned char*>(
        bytes.constData() + offset);
    return (static_cast<quint32>(data[0]) << 24) |
           (static_cast<quint32>(data[1]) << 16) |
           (static_cast<quint32>(data[2]) << 8) |
           static_cast<quint32>(data[3]);
}

void AppendChunk(QByteArray& output, const char* type, const QByteArray& data)
{
    AppendU32(output, static_cast<quint32>(data.size()));
    const int start = output.size();
    output.append(type, 4);
    output.append(data);
    const quint32 crc = Crc32(
        reinterpret_cast<const unsigned char*>(output.constData() + start),
        static_cast<std::size_t>(4 + data.size()));
    AppendU32(output, crc);
}

bool EncodePng(const std::vector<unsigned char>& pixels, int width, int height,
               const QByteArray& metadata, QByteArray& output, QString& error)
{
    if (width <= 0 || height <= 0)
    {
        error = QStringLiteral("PNG 尺寸无效。");
        return false;
    }
    if (pixels.size() !=
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
    {
        error = QStringLiteral("PNG 像素缓冲区尺寸不一致。");
        return false;
    }

    QByteArray raw;
    raw.reserve(static_cast<int>(
        (static_cast<qint64>(width) + 1) * static_cast<qint64>(height)));
    for (int y = 0; y < height; ++y)
    {
        raw.append('\0');
        raw.append(reinterpret_cast<const char*>(
                       pixels.data() + static_cast<std::size_t>(y) *
                                           static_cast<std::size_t>(width)),
                   width);
    }

    const QByteArray compressed = qCompress(raw, 6);
    if (compressed.size() <= 4)
    {
        error = QStringLiteral("PNG 数据压缩失败。");
        return false;
    }
    const QByteArray idat = compressed.mid(4);

    output.clear();
    output.append(reinterpret_cast<const char*>(kPngSignature),
                  sizeof(kPngSignature));

    QByteArray ihdr;
    AppendU32(ihdr, static_cast<quint32>(width));
    AppendU32(ihdr, static_cast<quint32>(height));
    ihdr.append(static_cast<char>(8)); // bit depth
    ihdr.append(static_cast<char>(3)); // palette color type
    ihdr.append(static_cast<char>(0)); // deflate
    ihdr.append(static_cast<char>(0)); // adaptive filtering
    ihdr.append(static_cast<char>(0)); // no interlace
    AppendChunk(output, "IHDR", ihdr);

    QByteArray palette;
    for (const auto& color : kReportPalette)
        for (unsigned char component : color)
            palette.append(static_cast<char>(component));
    AppendChunk(output, "PLTE", palette);

    QByteArray text = QByteArrayLiteral("slmslicer");
    text.append('\0');
    text.append(metadata);
    AppendChunk(output, "tEXt", text);

    AppendChunk(output, "IDAT", idat);
    AppendChunk(output, "IEND", QByteArray());
    return true;
}

std::size_t CountOccurrences(const QByteArray& data, const QByteArray& pattern)
{
    if (pattern.isEmpty())
        return 0;
    std::size_t count = 0;
    int offset = 0;
    while (true)
    {
        const int found = data.indexOf(pattern, offset);
        if (found < 0)
            break;
        ++count;
        offset = found + pattern.size();
    }
    return count;
}

struct SvgPatternCounts
{
    std::size_t layer_groups = 0;
    std::size_t outer_contours = 0;
    std::size_t inner_contours = 0;
    std::size_t hatches = 0;
};
} // namespace

SliceReportLayout ComputeSliceReportLayout(std::size_t layer_count)
{
    SliceReportLayout layout;
    if (layer_count == 0)
        return layout;

    int columns = static_cast<int>(std::ceil(std::sqrt(
        static_cast<double>(layer_count))));
    columns = std::clamp(columns, 1, 8);
    int rows = static_cast<int>(
        (layer_count + static_cast<std::size_t>(columns) - 1) /
        static_cast<std::size_t>(columns));

    int cell = 160;
    if (layer_count > 16)
        cell = 128;
    if (layer_count > 64)
        cell = 96;
    if (layer_count > 256)
        cell = 72;
    if (layer_count > 1024)
        cell = 56;
    if (layer_count > 4096)
        cell = 44;

    for (int guard = 0; guard < 64; ++guard)
    {
        rows = static_cast<int>(
            (layer_count + static_cast<std::size_t>(columns) - 1) /
            static_cast<std::size_t>(columns));
        const qint64 width = static_cast<qint64>(columns) * cell;
        const qint64 height = static_cast<qint64>(rows) * cell;
        const qint64 pixels =
            static_cast<qint64>(layer_count) * cell * cell;
        if (width <= kMaxReportDimension && height <= kMaxReportDimension &&
            pixels <= kMaxReportPixels)
            break;
        if (height > kMaxReportDimension && columns < 64)
        {
            columns *= 2;
            continue;
        }
        if (cell <= 6)
            break;
        cell = std::max(6, cell * 3 / 4);
    }

    layout.columns = columns;
    layout.rows = rows;
    layout.cell_px = cell;
    layout.width_px = columns * cell;
    layout.height_px = rows * cell;
    return layout;
}

bool WriteSvgReport(const SliceBuildSnapshot& snapshot,
                    const SliceExportOptions& options,
                    const SliceExportProgress& progress,
                    SliceExportResult& result)
{
    const std::vector<const SliceLayer*> layers = NonEmptyLayers(snapshot);
    if (layers.empty())
    {
        result.error = QStringLiteral("没有可导出的非空层。");
        return false;
    }

    const SliceReportLayout layout = ComputeSliceReportLayout(layers.size());
    const ReportCounts counts = CountLayers(layers, options.include_hatches);
    const QString metadata = BuildReportMetadata(
        QStringLiteral("svg"), counts, layout, layers.front(), layers.back(),
        options.include_hatches, -1);

    QSaveFile output(options.output_path);
    if (!output.open(QIODevice::WriteOnly))
    {
        result.error = QStringLiteral("无法写入 SVG 文件：%1")
                           .arg(output.errorString());
        return false;
    }

    auto write_bytes = [&](const QByteArray& bytes) -> bool
    {
        if (output.write(bytes) != bytes.size())
        {
            result.error = QStringLiteral("写入 SVG 文件失败：%1")
                               .arg(output.errorString());
            output.cancelWriting();
            return false;
        }
        return true;
    };

    QByteArray header;
    header += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    header += QStringLiteral(
                  "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" "
                  "width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\">\n")
                  .arg(layout.width_px)
                  .arg(layout.height_px)
                  .toUtf8();
    header += "<title>openSLMSlicer slice report</title>\n";
    header += "<desc>" + metadata.toUtf8() + "</desc>\n";
    header += "<rect x=\"0\" y=\"0\" width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
    header += "<g id=\"layers\">\n";
    if (!write_bytes(header))
        return false;

    const double cell = static_cast<double>(layout.cell_px);
    const double padding = std::max(2.0, cell * 0.05);
    const bool labels = cell >= 32.0;
    const double font_size =
        labels ? std::min(12.0, std::max(6.0, cell * 0.11)) : 0.0;
    const double label_height = labels ? font_size * 1.7 : 0.0;
    const double content_x = padding;
    const double content_y = padding + label_height;
    const double content_width = cell - 2.0 * padding;
    const double content_height = cell - padding - content_y;

    for (std::size_t index = 0; index < layers.size(); ++index)
    {
        const SliceLayer& layer = *layers[index];
        const int column = static_cast<int>(index) % layout.columns;
        const int row = static_cast<int>(index) / layout.columns;
        const double cell_x = static_cast<double>(column) * cell;
        const double cell_y = static_cast<double>(row) * cell;

        QByteArray block;
        block += QStringLiteral(
                     "<g class=\"layer\" data-grid-index=\"%1\" "
                     "data-z-mm=\"%2\" transform=\"translate(%3,%4)\">\n")
                     .arg(layer.grid_index)
                     .arg(FixedNumber(layer.z_mm, 3))
                     .arg(FixedNumber(cell_x, 1))
                     .arg(FixedNumber(cell_y, 1))
                     .toUtf8();
        block += QStringLiteral(
                     "<rect class=\"cell\" x=\"0\" y=\"0\" width=\"%1\" "
                     "height=\"%2\" fill=\"#fbfbfb\" stroke=\"#d8d8d8\" "
                     "stroke-width=\"1\"/>\n")
                     .arg(FixedNumber(cell, 1))
                     .arg(FixedNumber(cell, 1))
                     .toUtf8();

        double min_x = 0.0;
        double min_y = 0.0;
        double max_x = 0.0;
        double max_y = 0.0;
        const bool has_bounds = ComputeLayerBounds(
            layer, options.include_hatches, min_x, min_y, max_x, max_y);
        if (has_bounds && content_width > 1.0 && content_height > 1.0)
        {
            const double span_x = std::max(max_x - min_x, 1.0e-9);
            const double span_y = std::max(max_y - min_y, 1.0e-9);
            const double scale =
                std::min(content_width / span_x, content_height / span_y);
            const double origin_x =
                content_x + (content_width - span_x * scale) * 0.5 -
                min_x * scale;
            const double origin_y =
                content_y + (content_height - span_y * scale) * 0.5 +
                max_y * scale;
            auto map_x = [&](double value)
            { return origin_x + value * scale; };
            auto map_y = [&](double value)
            { return origin_y - value * scale; };

            if (options.include_hatches && !layer.hatches.empty())
            {
                block += "<g class=\"hatches\" fill=\"none\" "
                         "stroke=\"#9a9a9a\" stroke-width=\"0.6\">\n";
                for (const SliceHatchLine& line : layer.hatches)
                {
                    block += QStringLiteral(
                                 "<line class=\"hatch\" x1=\"%1\" y1=\"%2\" "
                                 "x2=\"%3\" y2=\"%4\"/>\n")
                                 .arg(FixedNumber(map_x(line.start[0]), 2))
                                 .arg(FixedNumber(map_y(line.start[1]), 2))
                                 .arg(FixedNumber(map_x(line.end[0]), 2))
                                 .arg(FixedNumber(map_y(line.end[1]), 2))
                                 .toUtf8();
                }
                block += "</g>\n";
            }

            block += "<g class=\"contours\" fill=\"none\" "
                     "stroke-linejoin=\"round\" stroke-linecap=\"round\">\n";
            for (const SlicePath& path : layer.contours)
            {
                if (path.points.size() < 2)
                    continue;
                QString points;
                for (const auto& point : path.points)
                {
                    if (!points.isEmpty())
                        points += QLatin1Char(' ');
                    points += FixedNumber(map_x(point[0]), 2) +
                              QLatin1Char(',') +
                              FixedNumber(map_y(point[1]), 2);
                }
                if (path.closed && path.points.size() > 2)
                {
                    const auto& first = path.points.front();
                    const auto& last = path.points.back();
                    if (first[0] != last[0] || first[1] != last[1])
                        points += QLatin1Char(' ') +
                                  FixedNumber(map_x(first[0]), 2) +
                                  QLatin1Char(',') +
                                  FixedNumber(map_y(first[1]), 2);
                }
                const bool outer = path.role == SliceRingRole::Outer;
                block += QStringLiteral(
                             "<polyline class=\"contour %1\" stroke=\"%2\" "
                             "stroke-width=\"%3\" points=\"%4\"/>\n")
                             .arg(outer ? QStringLiteral("outer")
                                        : QStringLiteral("inner"))
                             .arg(outer ? QStringLiteral("#153e75")
                                        : QStringLiteral("#b02020"))
                             .arg(outer ? QStringLiteral("1.4")
                                        : QStringLiteral("1.1"))
                             .arg(points)
                             .toUtf8();
            }
            block += "</g>\n";
        }

        if (labels)
        {
            block += QStringLiteral(
                         "<text x=\"%1\" y=\"%2\" font-size=\"%3\" "
                         "font-family=\"Segoe UI, Arial, sans-serif\" "
                         "fill=\"#444444\">L%4 z=%5</text>\n")
                         .arg(FixedNumber(padding, 1))
                         .arg(FixedNumber(padding + font_size, 1))
                         .arg(FixedNumber(font_size, 1))
                         .arg(layer.grid_index)
                         .arg(FixedNumber(layer.z_mm, 3))
                         .toUtf8();
        }
        block += "</g>\n";
        if (!write_bytes(block))
            return false;

        result.layers_written = index + 1;
        result.contours_written += layer.contours.size();
        if (options.include_hatches)
            result.hatches_written += layer.hatches.size();
        if (progress && !progress(index + 1, layers.size()))
        {
            result.cancelled = true;
            result.error = QStringLiteral("已取消导出。");
            output.cancelWriting();
            return false;
        }
    }

    QByteArray footer("</g>\n</svg>\n");
    if (!write_bytes(footer) || !output.commit())
    {
        result.error = QStringLiteral("保存 SVG 文件失败：%1")
                           .arg(output.errorString());
        return false;
    }
    result.bytes_written =
        static_cast<std::uint64_t>(QFile(options.output_path).size());
    return true;
}

bool WritePngReport(const SliceBuildSnapshot& snapshot,
                    const SliceExportOptions& options,
                    const SliceExportProgress& progress,
                    SliceExportResult& result)
{
    const std::vector<const SliceLayer*> layers = NonEmptyLayers(snapshot);
    if (layers.empty())
    {
        result.error = QStringLiteral("没有可导出的非空层。");
        return false;
    }

    const SliceReportLayout layout = ComputeSliceReportLayout(layers.size());
    const ReportCounts counts = CountLayers(layers, options.include_hatches);
    const qint64 raw_bytes =
        static_cast<qint64>(layout.height_px) *
        (static_cast<qint64>(layout.width_px) + 1);
    const QString metadata = BuildReportMetadata(
        QStringLiteral("png"), counts, layout, layers.front(), layers.back(),
        options.include_hatches, raw_bytes);

    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(layout.width_px) *
            static_cast<std::size_t>(layout.height_px),
        kColorBackground);

    const double cell = static_cast<double>(layout.cell_px);
    const double padding = std::max(2.0, cell * 0.05);
    // Keep the geometry area identical to the SVG report even though the PNG
    // contact sheet has no text labels.
    const double label_height = cell >= 32.0
        ? std::min(12.0, std::max(6.0, cell * 0.11)) * 1.7 : 0.0;
    const double content_x = padding;
    const double content_y = padding + label_height;
    const double content_width = cell - 2.0 * padding;
    const double content_height = cell - padding - content_y;

    for (std::size_t index = 0; index < layers.size(); ++index)
    {
        const SliceLayer& layer = *layers[index];
        const int column = static_cast<int>(index) % layout.columns;
        const int row = static_cast<int>(index) / layout.columns;
        const int cell_x = column * layout.cell_px;
        const int cell_y = row * layout.cell_px;
        DrawRectOutline(pixels, layout.width_px, layout.height_px,
                        cell_x, cell_y, layout.cell_px, layout.cell_px,
                        kColorBorder);

        double min_x = 0.0;
        double min_y = 0.0;
        double max_x = 0.0;
        double max_y = 0.0;
        const bool has_bounds = ComputeLayerBounds(
            layer, options.include_hatches, min_x, min_y, max_x, max_y);
        if (has_bounds && content_width > 1.0 && content_height > 1.0)
        {
            const double span_x = std::max(max_x - min_x, 1.0e-9);
            const double span_y = std::max(max_y - min_y, 1.0e-9);
            const double scale =
                std::min(content_width / span_x, content_height / span_y);
            const double origin_x =
                static_cast<double>(cell_x) + content_x +
                (content_width - span_x * scale) * 0.5 - min_x * scale;
            const double origin_y =
                static_cast<double>(cell_y) + content_y +
                (content_height - span_y * scale) * 0.5 + max_y * scale;
            auto map_x = [&](double value)
            { return origin_x + value * scale; };
            auto map_y = [&](double value)
            { return origin_y - value * scale; };

            if (options.include_hatches)
            {
                for (const SliceHatchLine& line : layer.hatches)
                    DrawLine(pixels, layout.width_px, layout.height_px,
                             map_x(line.start[0]), map_y(line.start[1]),
                             map_x(line.end[0]), map_y(line.end[1]),
                             kColorHatch);
            }

            for (int pass = 0; pass < 2; ++pass)
            {
                const SliceRingRole role =
                    pass == 0 ? SliceRingRole::Outer : SliceRingRole::Inner;
                const unsigned char color =
                    pass == 0 ? kColorOuter : kColorInner;
                for (const SlicePath& path : layer.contours)
                {
                    if (path.role != role || path.points.size() < 2)
                        continue;
                    for (std::size_t point = 1; point < path.points.size();
                         ++point)
                        DrawLine(pixels, layout.width_px, layout.height_px,
                                 map_x(path.points[point - 1][0]),
                                 map_y(path.points[point - 1][1]),
                                 map_x(path.points[point][0]),
                                 map_y(path.points[point][1]), color);
                    if (path.closed && path.points.size() > 2)
                    {
                        const auto& first = path.points.front();
                        const auto& last = path.points.back();
                        if (first[0] != last[0] || first[1] != last[1])
                            DrawLine(pixels, layout.width_px,
                                     layout.height_px,
                                     map_x(last[0]), map_y(last[1]),
                                     map_x(first[0]), map_y(first[1]), color);
                    }
                }
            }
        }

        result.layers_written = index + 1;
        result.contours_written += layer.contours.size();
        if (options.include_hatches)
            result.hatches_written += layer.hatches.size();
        if (progress && !progress(index + 1, layers.size()))
        {
            result.cancelled = true;
            result.error = QStringLiteral("已取消导出。");
            return false;
        }
    }

    QByteArray encoded;
    QString error;
    if (!EncodePng(pixels, layout.width_px, layout.height_px,
                   metadata.toUtf8(), encoded, error))
    {
        result.error = error;
        return false;
    }

    QSaveFile output(options.output_path);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(encoded) != encoded.size() || !output.commit())
    {
        result.error = QStringLiteral("保存 PNG 文件失败：%1")
                           .arg(output.errorString());
        return false;
    }
    result.bytes_written =
        static_cast<std::uint64_t>(QFile(options.output_path).size());
    return true;
}

bool WriteCsvReport(const SliceBuildSnapshot& snapshot,
                    const SliceExportOptions& options,
                    const SliceExportProgress& progress,
                    SliceExportResult& result)
{
    std::vector<const SliceLayer*> layers;
    layers.reserve(snapshot.layers.size());
    for (const SliceLayer& layer : snapshot.layers)
        if (options.include_empty_layers || !layer.empty)
            layers.push_back(&layer);
    if (layers.empty())
    {
        result.error = QStringLiteral("没有可导出的层。");
        return false;
    }

    QSaveFile output(options.output_path);
    if (!output.open(QIODevice::WriteOnly))
    {
        result.error = QStringLiteral("无法写入 CSV 文件：%1")
                           .arg(output.errorString());
        return false;
    }

    auto write_bytes = [&](const QByteArray& bytes) -> bool
    {
        if (output.write(bytes) != bytes.size())
        {
            result.error = QStringLiteral("写入 CSV 文件失败：%1")
                               .arg(output.errorString());
            output.cancelWriting();
            return false;
        }
        return true;
    };

    QByteArray header =
        "grid_index,z_mm,empty,contours,hatches,"
        "contour_length_mm,hatch_length_mm,total_length_mm\n";
    if (!write_bytes(header))
        return false;

    for (std::size_t index = 0; index < layers.size(); ++index)
    {
        const SliceLayer& layer = *layers[index];
        const std::size_t hatch_count =
            options.include_hatches ? layer.hatches.size() : 0;
        const double contour_length = SliceLayerContourLengthMm(layer);
        const double hatch_length = options.include_hatches
            ? SliceLayerHatchLengthMm(layer) : 0.0;
        QByteArray row = QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8\n")
                             .arg(layer.grid_index)
                             .arg(FixedNumber(layer.z_mm, 6))
                             .arg(layer.empty ? 1 : 0)
                             .arg(static_cast<qulonglong>(layer.contours.size()))
                             .arg(static_cast<qulonglong>(hatch_count))
                             .arg(FixedNumber(contour_length, 6))
                             .arg(FixedNumber(hatch_length, 6))
                             .arg(FixedNumber(contour_length + hatch_length, 6))
                             .toUtf8();
        if (!write_bytes(row))
            return false;

        result.layers_written = index + 1;
        result.contours_written += layer.contours.size();
        result.hatches_written += hatch_count;
        if (progress && !progress(index + 1, layers.size()))
        {
            result.cancelled = true;
            result.error = QStringLiteral("已取消导出。");
            output.cancelWriting();
            return false;
        }
    }

    if (!output.commit())
    {
        result.error = QStringLiteral("保存 CSV 文件失败：%1")
                           .arg(output.errorString());
        return false;
    }
    result.bytes_written =
        static_cast<std::uint64_t>(QFile(options.output_path).size());
    return true;
}

bool SummarizeSvgReport(const QString& path, SliceFileSummary& summary,
                        QString& error)
{
    summary = {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("无法读取 SVG 文件：%1").arg(file.errorString());
        return false;
    }

    const QByteArray head = file.read(256 * 1024);
    if (!head.contains("<svg"))
    {
        error = QStringLiteral("不是有效的 SVG 报表。");
        return false;
    }
    const int desc_start = head.indexOf("<desc>");
    const int desc_end =
        desc_start >= 0 ? head.indexOf("</desc>", desc_start) : -1;
    if (desc_start < 0 || desc_end < 0)
    {
        error = QStringLiteral("SVG 报表缺少描述元数据。");
        return false;
    }
    QHash<QString, QString> values;
    if (!ParseReportMetadata(
            head.mid(desc_start + 6, desc_end - desc_start - 6), values))
    {
        error = QStringLiteral("SVG 报表描述元数据无效。");
        return false;
    }
    if (values.value(QStringLiteral("format")) != QStringLiteral("svg"))
    {
        error = QStringLiteral("SVG 报表格式标识不匹配。");
        return false;
    }
    if (!RequiredSize(values, QStringLiteral("layers"), summary.layers, error) ||
        !RequiredSize(values, QStringLiteral("contours"), summary.contours,
                      error) ||
        !RequiredSize(values, QStringLiteral("hatches"), summary.hatches,
                      error) ||
        !RequiredDouble(values, QStringLiteral("contour_length_mm"),
                        summary.contour_length_mm, error) ||
        !RequiredDouble(values, QStringLiteral("hatch_length_mm"),
                        summary.hatch_length_mm, error))
        return false;

    SvgPatternCounts patterns;
    file.seek(0);
    QByteArray carry;
    while (!file.atEnd())
    {
        const QByteArray chunk = file.read(1024 * 1024);
        const QByteArray data = carry + chunk;
        patterns.layer_groups +=
            CountOccurrences(data, QByteArrayLiteral("<g class=\"layer\""));
        patterns.outer_contours +=
            CountOccurrences(data, QByteArrayLiteral("class=\"contour outer\""));
        patterns.inner_contours +=
            CountOccurrences(data, QByteArrayLiteral("class=\"contour inner\""));
        patterns.hatches +=
            CountOccurrences(data, QByteArrayLiteral("class=\"hatch\""));
        const int keep = static_cast<int>(kMaxPatternLength) - 1;
        carry = data.size() > keep ? data.right(keep) : data;
    }

    if (patterns.layer_groups != summary.layers)
    {
        error = QStringLiteral("SVG 层数与元数据不一致。");
        return false;
    }
    if (patterns.outer_contours + patterns.inner_contours != summary.contours)
    {
        error = QStringLiteral("SVG 轮廓数与元数据不一致。");
        return false;
    }
    if (patterns.hatches != summary.hatches)
    {
        error = QStringLiteral("SVG 填充数与元数据不一致。");
        return false;
    }
    return true;
}

bool SummarizePngReport(const QString& path, SliceFileSummary& summary,
                        QString& error)
{
    summary = {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("无法读取 PNG 文件：%1").arg(file.errorString());
        return false;
    }
    const QByteArray data = file.readAll();
    if (data.size() < 8 ||
        std::memcmp(data.constData(), kPngSignature, sizeof(kPngSignature)) != 0)
    {
        error = QStringLiteral("不是有效的 PNG 报表。");
        return false;
    }

    quint32 width = 0;
    quint32 height = 0;
    bool saw_ihdr = false;
    bool saw_plte = false;
    bool saw_iend = false;
    QByteArray idat;
    QByteArray metadata;
    int offset = 8;
    while (offset + 12 <= data.size())
    {
        const quint32 length = ReadU32(data, offset);
        if (static_cast<quint64>(length) + 12u >
            static_cast<quint64>(data.size() - offset))
        {
            error = QStringLiteral("PNG 数据块长度越界。");
            return false;
        }
        const QByteArray type = data.mid(offset + 4, 4);
        const QByteArray chunk = data.mid(offset + 8, static_cast<int>(length));
        const quint32 expected_crc = ReadU32(data, offset + 8 + static_cast<int>(length));
        const quint32 actual_crc = Crc32(
            reinterpret_cast<const unsigned char*>(data.constData() + offset + 4),
            static_cast<std::size_t>(4u + length));
        if (expected_crc != actual_crc)
        {
            error = QStringLiteral("PNG 数据块校验和不匹配。");
            return false;
        }

        if (type == QByteArrayLiteral("IHDR"))
        {
            if (length != 13)
            {
                error = QStringLiteral("PNG IHDR 长度无效。");
                return false;
            }
            width = ReadU32(chunk, 0);
            height = ReadU32(chunk, 4);
            if (static_cast<unsigned char>(chunk.at(8)) != 8 ||
                static_cast<unsigned char>(chunk.at(9)) != 3)
            {
                error = QStringLiteral("PNG 位深或颜色类型不受支持。");
                return false;
            }
            saw_ihdr = true;
        }
        else if (type == QByteArrayLiteral("PLTE"))
        {
            if (length < 3)
            {
                error = QStringLiteral("PNG 调色板长度无效。");
                return false;
            }
            saw_plte = true;
        }
        else if (type == QByteArrayLiteral("IDAT"))
        {
            idat += chunk;
        }
        else if (type == QByteArrayLiteral("tEXt"))
        {
            const int separator = chunk.indexOf('\0');
            if (separator > 0 &&
                chunk.left(separator) == QByteArrayLiteral("slmslicer"))
                metadata = chunk.mid(separator + 1);
        }
        else if (type == QByteArrayLiteral("IEND"))
        {
            saw_iend = true;
            break;
        }
        offset += 12 + static_cast<int>(length);
    }

    if (!saw_ihdr || !saw_plte || !saw_iend || idat.isEmpty() ||
        metadata.isEmpty())
    {
        error = QStringLiteral("PNG 报表缺少必要的数据块。");
        return false;
    }

    QHash<QString, QString> values;
    if (!ParseReportMetadata(metadata, values))
    {
        error = QStringLiteral("PNG 报表元数据无效。");
        return false;
    }
    if (values.value(QStringLiteral("format")) != QStringLiteral("png"))
    {
        error = QStringLiteral("PNG 报表格式标识不匹配。");
        return false;
    }
    std::size_t metadata_width = 0;
    std::size_t metadata_height = 0;
    std::size_t raw_bytes = 0;
    if (!RequiredSize(values, QStringLiteral("layers"), summary.layers, error) ||
        !RequiredSize(values, QStringLiteral("contours"), summary.contours,
                      error) ||
        !RequiredSize(values, QStringLiteral("hatches"), summary.hatches,
                      error) ||
        !RequiredDouble(values, QStringLiteral("contour_length_mm"),
                        summary.contour_length_mm, error) ||
        !RequiredDouble(values, QStringLiteral("hatch_length_mm"),
                        summary.hatch_length_mm, error) ||
        !RequiredSize(values, QStringLiteral("width"), metadata_width, error) ||
        !RequiredSize(values, QStringLiteral("height"), metadata_height, error) ||
        !RequiredSize(values, QStringLiteral("raw_bytes"), raw_bytes, error))
        return false;

    if (metadata_width != width || metadata_height != height)
    {
        error = QStringLiteral("PNG 尺寸与元数据不一致。");
        return false;
    }
    const quint64 expected_raw =
        static_cast<quint64>(height) * (static_cast<quint64>(width) + 1u);
    if (raw_bytes != expected_raw)
    {
        error = QStringLiteral("PNG 原始数据长度与尺寸不一致。");
        return false;
    }

    QByteArray compressed_header;
    AppendU32(compressed_header, static_cast<quint32>(raw_bytes));
    const QByteArray raw = qUncompress(compressed_header + idat);
    if (static_cast<std::size_t>(raw.size()) != raw_bytes)
    {
        error = QStringLiteral("PNG 图像数据解压失败。");
        return false;
    }
    for (quint32 row = 0; row < height; ++row)
    {
        if (static_cast<unsigned char>(
                raw.at(static_cast<int>(row * (width + 1u)))) != 0)
        {
            error = QStringLiteral("PNG 行过滤方式无效。");
            return false;
        }
    }
    return true;
}

bool SummarizeCsvReport(const QString& path, SliceFileSummary& summary,
                        QString& error)
{
    summary = {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("无法读取 CSV 文件：%1").arg(file.errorString());
        return false;
    }
    const QByteArray data = file.readAll();
    const QByteArray expected_header =
        "grid_index,z_mm,empty,contours,hatches,"
        "contour_length_mm,hatch_length_mm,total_length_mm";
    const QList<QByteArray> lines = data.split('\n');
    bool saw_header = false;
    for (QByteArray line : lines)
    {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty())
            continue;
        if (!saw_header)
        {
            if (line != expected_header)
            {
                error = QStringLiteral("CSV 表头无效。");
                return false;
            }
            saw_header = true;
            continue;
        }
        const QList<QByteArray> fields = line.split(',');
        if (fields.size() != 8)
        {
            error = QStringLiteral("CSV 行列数无效。");
            return false;
        }
        bool ok = false;
        const qlonglong grid_index = fields[0].toLongLong(&ok);
        if (!ok)
        {
            error = QStringLiteral("CSV 层号无效。");
            return false;
        }
        (void)grid_index;
        fields[1].toDouble(&ok);
        if (!ok)
        {
            error = QStringLiteral("CSV Z 值无效。");
            return false;
        }
        const int empty = fields[2].toInt(&ok);
        if (!ok || (empty != 0 && empty != 1))
        {
            error = QStringLiteral("CSV 空层标记无效。");
            return false;
        }
        const qulonglong contours = fields[3].toULongLong(&ok);
        if (!ok)
        {
            error = QStringLiteral("CSV 轮廓数无效。");
            return false;
        }
        const qulonglong hatches = fields[4].toULongLong(&ok);
        if (!ok)
        {
            error = QStringLiteral("CSV 填充数无效。");
            return false;
        }
        const double contour_length = fields[5].toDouble(&ok);
        if (!ok)
        {
            error = QStringLiteral("CSV 轮廓长度无效。");
            return false;
        }
        const double hatch_length = fields[6].toDouble(&ok);
        if (!ok)
        {
            error = QStringLiteral("CSV 填充长度无效。");
            return false;
        }
        fields[7].toDouble(&ok);
        if (!ok)
        {
            error = QStringLiteral("CSV 总长度无效。");
            return false;
        }
        ++summary.layers;
        summary.contours += static_cast<std::size_t>(contours);
        summary.hatches += static_cast<std::size_t>(hatches);
        if (std::isfinite(contour_length))
            summary.contour_length_mm += contour_length;
        if (std::isfinite(hatch_length))
            summary.hatch_length_mm += hatch_length;
    }
    if (!saw_header)
    {
        error = QStringLiteral("CSV 报表缺少表头。");
        return false;
    }
    if (summary.layers == 0)
    {
        error = QStringLiteral("CSV 报表不包含数据行。");
        return false;
    }
    return true;
}
