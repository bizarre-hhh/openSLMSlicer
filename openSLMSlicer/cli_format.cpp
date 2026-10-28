#include "cli_format.h"

#include <QByteArray>
#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSaveFile>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr int kDecimals = 4;

QByteArray FormatReal(double value)
{
    if (!std::isfinite(value))
        value = 0.0;
    return QString::number(value, 'f', kDecimals).toLatin1();
}

void AppendLine(QByteArray& target, const QByteArray& line)
{
    target += line;
    target += '\n';
}

QByteArray PolylineCommand(const SlicePath& path)
{
    std::vector<std::array<double, 2>> points = path.points;
    if (points.size() < 2)
        return {};
    if (path.closed && (points.front()[0] != points.back()[0] ||
                        points.front()[1] != points.back()[1]))
        points.push_back(points.front());

    const int direction = path.closed
        ? (path.role == SliceRingRole::Inner ? 0 : 1)
        : 2;
    QByteArray line = "$$POLYLINE/0,";
    line += QByteArray::number(direction);
    line += ',';
    line += QByteArray::number(static_cast<int>(points.size()));
    for (const auto& point : points)
    {
        line += ',';
        line += FormatReal(point[0]);
        line += ',';
        line += FormatReal(point[1]);
    }
    return line;
}

QByteArray HatchCommand(const std::vector<SliceHatchLine>& hatches)
{
    QByteArray line = "$$HATCHES/0,";
    line += QByteArray::number(static_cast<int>(hatches.size()));
    for (const SliceHatchLine& hatch : hatches)
    {
        line += ',';
        line += FormatReal(hatch.start[0]);
        line += ',';
        line += FormatReal(hatch.start[1]);
        line += ',';
        line += FormatReal(hatch.end[0]);
        line += ',';
        line += FormatReal(hatch.end[1]);
    }
    return line;
}

bool ParseDouble(const QByteArray& field, double& value)
{
    bool ok = false;
    value = field.trimmed().toDouble(&ok);
    return ok && std::isfinite(value);
}

bool ParseInteger(const QByteArray& field, int& value)
{
    bool ok = false;
    value = field.trimmed().toInt(&ok);
    return ok;
}
} // namespace

bool WriteCliAscii(const SliceBuildSnapshot& snapshot,
                   const SliceExportOptions& options,
                   const SliceExportProgress& progress,
                   SliceExportResult& result)
{
    std::vector<const SliceLayer*> layers;
    layers.reserve(snapshot.layers.size());
    for (const SliceLayer& layer : snapshot.layers)
        if (!layer.empty)
            layers.push_back(&layer);
    if (layers.empty())
    {
        result.error = QStringLiteral("没有可导出的非空层。");
        return false;
    }

    const double thickness = snapshot.settings.layer_thickness_mm;

    QSaveFile output(options.output_path);
    if (!output.open(QIODevice::WriteOnly))
    {
        result.error = QStringLiteral("无法写入导出文件：%1")
            .arg(output.errorString());
        return false;
    }

    QByteArray header;
    AppendLine(header, "$$HEADERSTART");
    AppendLine(header, "$$ASCII");
    AppendLine(header, "$$UNITS/1.0");
    AppendLine(header, "$$VERSION/200");
    AppendLine(header, QStringLiteral("$$DATE/%1")
        .arg(QDate::currentDate().toString(QStringLiteral("ddMMyy")))
        .toLatin1());
    AppendLine(header, QStringLiteral("$$LAYERS/%1")
        .arg(static_cast<qulonglong>(layers.size())).toLatin1());
    AppendLine(header, "$$HEADEREND");
    AppendLine(header, "$$GEOMETRYSTART");
    if (output.write(header) != header.size())
    {
        result.error = QStringLiteral("写入 CLI 文件头失败。");
        output.cancelWriting();
        return false;
    }

    for (std::size_t index = 0; index < layers.size(); ++index)
    {
        const SliceLayer& layer = *layers[index];
        QByteArray block;
        // CLI defines the layer plane by its upper surface; the slicer keeps
        // the mid plane internally.
        const double z_top = layer.z_mm + thickness * 0.5;
        AppendLine(block, QStringLiteral("$$LAYER/%1")
            .arg(QString::number(z_top, 'f', kDecimals)).toLatin1());
        for (const SlicePath& path : layer.contours)
        {
            const QByteArray line = PolylineCommand(path);
            if (!line.isEmpty())
                AppendLine(block, line);
            ++result.contours_written;
        }
        if (options.include_hatches && !layer.hatches.empty())
        {
            AppendLine(block, HatchCommand(layer.hatches));
            result.hatches_written += layer.hatches.size();
        }
        if (output.write(block) != block.size())
        {
            result.error = QStringLiteral("写入 CLI 层数据失败。");
            output.cancelWriting();
            return false;
        }
        result.layers_written = index + 1;
        if (progress && !progress(index + 1, layers.size()))
        {
            result.cancelled = true;
            result.error = QStringLiteral("已取消导出。");
            output.cancelWriting();
            return false;
        }
    }

    QByteArray footer("$$GEOMETRYEND\n");
    if (output.write(footer) != footer.size() || !output.commit())
    {
        result.error = QStringLiteral("保存 CLI 文件失败：%1")
            .arg(output.errorString());
        return false;
    }
    result.bytes_written =
        static_cast<std::uint64_t>(QFileInfo(options.output_path).size());
    return true;
}

bool ParseCliAscii(const QString& path, SliceFileSummary& summary,
                   QString& error)
{
    summary = {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("无法读取 CLI 文件：%1").arg(file.errorString());
        return false;
    }

    bool in_geometry = false;
    bool saw_header = false;
    bool saw_ascii = false;
    std::size_t line_number = 0;
    while (!file.atEnd())
    {
        const QByteArray raw = file.readLine();
        ++line_number;
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith("//"))
            continue;
        if (line == "$$HEADERSTART")
        {
            saw_header = true;
            continue;
        }
        if (line == "$$HEADEREND")
            continue;
        if (line == "$$ASCII")
        {
            saw_ascii = true;
            continue;
        }
        if (line == "$$BINARY")
        {
            error = QStringLiteral("CLI 校验器只支持 ASCII 文件。");
            return false;
        }
        if (line == "$$GEOMETRYSTART")
        {
            in_geometry = true;
            continue;
        }
        if (line == "$$GEOMETRYEND")
            break;
        if (!in_geometry)
            continue;

        const int slash = line.indexOf('/');
        if (slash <= 0)
        {
            error = QStringLiteral("CLI 第 %1 行缺少命令参数。")
                .arg(static_cast<qulonglong>(line_number));
            return false;
        }
        const QByteArray command = line.left(slash);
        const QByteArray payload = line.mid(slash + 1);
        const QList<QByteArray> fields = payload.split(',');
        if (command == "$$LAYER")
        {
            if (fields.size() != 1)
            {
                error = QStringLiteral("CLI 第 %1 行层命令参数无效。")
                    .arg(static_cast<qulonglong>(line_number));
                return false;
            }
            ++summary.layers;
            continue;
        }
        if (command == "$$POLYLINE")
        {
            if (fields.size() < 3)
            {
                error = QStringLiteral("CLI 第 %1 行轮廓命令参数无效。")
                    .arg(static_cast<qulonglong>(line_number));
                return false;
            }
            int id = 0, direction = 0, count = 0;
            if (!ParseInteger(fields[0], id) ||
                !ParseInteger(fields[1], direction) ||
                !ParseInteger(fields[2], count) ||
                direction < 0 || direction > 2 || count < 2 ||
                fields.size() != 3 + 2 * count)
            {
                error = QStringLiteral("CLI 第 %1 行轮廓点数量无效。")
                    .arg(static_cast<qulonglong>(line_number));
                return false;
            }
            Q_UNUSED(id);
            double previous_x = 0.0;
            double previous_y = 0.0;
            for (int point = 0; point < count; ++point)
            {
                double x = 0.0, y = 0.0;
                if (!ParseDouble(fields[3 + point * 2], x) ||
                    !ParseDouble(fields[4 + point * 2], y))
                {
                    error = QStringLiteral("CLI 第 %1 行轮廓坐标无效。")
                        .arg(static_cast<qulonglong>(line_number));
                    return false;
                }
                if (point > 0)
                    summary.contour_length_mm += std::hypot(
                        x - previous_x, y - previous_y);
                previous_x = x;
                previous_y = y;
            }
            ++summary.contours;
            continue;
        }
        if (command == "$$HATCHES")
        {
            if (fields.size() < 2)
            {
                error = QStringLiteral("CLI 第 %1 行填充命令参数无效。")
                    .arg(static_cast<qulonglong>(line_number));
                return false;
            }
            int id = 0, count = 0;
            if (!ParseInteger(fields[0], id) ||
                !ParseInteger(fields[1], count) || count < 0 ||
                fields.size() != 2 + 4 * count)
            {
                error = QStringLiteral("CLI 第 %1 行填充数量无效。")
                    .arg(static_cast<qulonglong>(line_number));
                return false;
            }
            Q_UNUSED(id);
            for (int hatch = 0; hatch < count; ++hatch)
            {
                double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
                if (!ParseDouble(fields[2 + hatch * 4], x1) ||
                    !ParseDouble(fields[3 + hatch * 4], y1) ||
                    !ParseDouble(fields[4 + hatch * 4], x2) ||
                    !ParseDouble(fields[5 + hatch * 4], y2))
                {
                    error = QStringLiteral("CLI 第 %1 行填充坐标无效。")
                        .arg(static_cast<qulonglong>(line_number));
                    return false;
                }
                summary.hatch_length_mm += std::hypot(x2 - x1, y2 - y1);
            }
            summary.hatches += static_cast<std::size_t>(count);
            continue;
        }
    }

    if (!saw_header || !saw_ascii || !in_geometry || summary.layers == 0)
    {
        error = QStringLiteral("CLI 文件头或几何段不完整。");
        return false;
    }
    return true;
}
