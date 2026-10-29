#include "slmslice_format.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr char kMagic[8] = {'S', 'L', 'M', 'S', 'L', 'I', 'C', 'E'};
constexpr char kLegacyMagic[8] = {'G', 'L', 'S', 'L', 'I', 'C', 'E', '\0'};
constexpr quint32 kVersion = 1;
constexpr quint32 kMaxHeaderBytes = 16 * 1024 * 1024;
constexpr quint32 kMaxLayerCount = 200000;
constexpr quint32 kMaxRingCount = 4000000;
constexpr quint32 kMaxHatchCount = 4000000;
constexpr quint32 kMaxPointCount = 10000000;

bool MatchesMagic(const char* magic)
{
    return std::memcmp(magic, kMagic, sizeof(kMagic)) == 0 ||
           std::memcmp(magic, kLegacyMagic, sizeof(kLegacyMagic)) == 0;
}

bool IsSlmSliceFormatName(const QString& format)
{
    return format == QStringLiteral("slmslice") ||
           format == QStringLiteral("glslice");
}

void ConfigureStream(QDataStream& stream)
{
    stream.setVersion(QDataStream::Qt_5_12);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}

QJsonObject HatchSettingsJson(const HatchSettings& hatch)
{
    QJsonObject object;
    object.insert(QStringLiteral("enabled"), hatch.enabled);
    object.insert(QStringLiteral("spacing_mm"), hatch.spacing_mm);
    object.insert(QStringLiteral("angle_deg"), hatch.angle_deg);
    object.insert(QStringLiteral("layer_angle_increment_deg"),
                  hatch.layer_angle_increment_deg);
    object.insert(QStringLiteral("spot_compensation_mm"),
                  hatch.spot_compensation_mm);
    return object;
}

// "kind" is an optional v1 header field (see docs/slmslice-v1.md 3.6 / 7):
// writers only emit it for support parts, so files written before automatic
// support existed keep their exact header shape. Readers treat a missing or
// unknown value as Model instead of rejecting the file.
SlicePartKind ReadPartKindJson(const QJsonObject& object)
{
    if (object.value(QStringLiteral("kind")).toString() ==
        QStringLiteral("support"))
    {
        return SlicePartKind::Support;
    }
    return SlicePartKind::Model;
}

void ReadHatchSettingsJson(const QJsonObject& object, HatchSettings& hatch)
{
    hatch.enabled = object.value(QStringLiteral("enabled")).toBool(hatch.enabled);
    hatch.spacing_mm = object.value(QStringLiteral("spacing_mm"))
        .toDouble(hatch.spacing_mm);
    hatch.angle_deg = object.value(QStringLiteral("angle_deg"))
        .toDouble(hatch.angle_deg);
    hatch.layer_angle_increment_deg =
        object.value(QStringLiteral("layer_angle_increment_deg"))
            .toDouble(hatch.layer_angle_increment_deg);
    hatch.spot_compensation_mm =
        object.value(QStringLiteral("spot_compensation_mm"))
            .toDouble(hatch.spot_compensation_mm);
}

void WriteLayerBlob(const SliceLayer& layer, bool include_hatches,
                    QByteArray& blob, double& contour_length,
                    double& hatch_length)
{
    blob.clear();
    QDataStream out(&blob, QIODevice::WriteOnly);
    ConfigureStream(out);

    const quint32 flags = layer.empty ? 1u : 0u;
    const quint32 ring_count = static_cast<quint32>(layer.contours.size());
    const quint32 hatch_count = include_hatches
        ? static_cast<quint32>(layer.hatches.size()) : 0u;
    out << static_cast<qint64>(layer.grid_index);
    out << static_cast<double>(layer.z_mm);
    out << flags << ring_count << hatch_count;

    contour_length = 0.0;
    for (const SlicePath& path : layer.contours)
    {
        out << static_cast<quint64>(path.model_id);
        out << static_cast<quint8>(path.role == SliceRingRole::Inner ? 1u : 0u);
        out << static_cast<quint8>(path.closed ? 1u : 0u);
        out << static_cast<quint16>(0);
        out << static_cast<quint32>(path.points.size());
        for (const auto& point : path.points)
            out << static_cast<double>(point[0]) << static_cast<double>(point[1]);
        for (std::size_t i = 1; i < path.points.size(); ++i)
        {
            const double dx = path.points[i][0] - path.points[i - 1][0];
            const double dy = path.points[i][1] - path.points[i - 1][1];
            contour_length += std::hypot(dx, dy);
        }
    }

    hatch_length = 0.0;
    if (include_hatches)
    {
        for (const SliceHatchLine& line : layer.hatches)
        {
            out << static_cast<quint64>(line.model_id);
            out << static_cast<double>(line.start[0])
                << static_cast<double>(line.start[1])
                << static_cast<double>(line.end[0])
                << static_cast<double>(line.end[1]);
            hatch_length += std::hypot(line.end[0] - line.start[0],
                                       line.end[1] - line.start[1]);
        }
    }
}

bool ReadLayerBlob(const QByteArray& blob, SliceLayer& layer,
                   SliceFileSummary& summary, QString& error)
{
    QDataStream in(blob);
    ConfigureStream(in);

    qint64 grid_index = 0;
    double z_mm = 0.0;
    quint32 flags = 0;
    quint32 ring_count = 0;
    quint32 hatch_count = 0;
    in >> grid_index >> z_mm >> flags >> ring_count >> hatch_count;
    if (in.status() != QDataStream::Ok || ring_count > kMaxRingCount ||
        hatch_count > kMaxHatchCount)
    {
        error = QStringLiteral("层数据头无效或数量超限。");
        return false;
    }

    layer = SliceLayer();
    layer.grid_index = grid_index;
    layer.z_mm = z_mm;
    layer.empty = (flags & 1u) != 0u;
    layer.contours.reserve(ring_count);
    layer.hatches.reserve(hatch_count);

    for (quint32 ring = 0; ring < ring_count; ++ring)
    {
        quint64 model_id = 0;
        quint8 role = 0;
        quint8 closed = 0;
        quint16 reserved = 0;
        quint32 point_count = 0;
        in >> model_id >> role >> closed >> reserved >> point_count;
        if (in.status() != QDataStream::Ok || point_count > kMaxPointCount ||
            point_count < 2)
        {
            error = QStringLiteral("轮廓点数量无效。");
            return false;
        }

        SlicePath path;
        path.model_id = model_id;
        path.role = role == 1u ? SliceRingRole::Inner : SliceRingRole::Outer;
        path.closed = closed != 0u;
        path.points.resize(point_count);
        for (quint32 point = 0; point < point_count; ++point)
        {
            double x = 0.0, y = 0.0;
            in >> x >> y;
            path.points[point] = {x, y};
        }
        if (in.status() != QDataStream::Ok)
        {
            error = QStringLiteral("轮廓坐标数据不完整。");
            return false;
        }
        layer.contour_path_length_mm += SlicePathLengthMm(path);
        layer.contours.push_back(std::move(path));
    }

    for (quint32 hatch = 0; hatch < hatch_count; ++hatch)
    {
        quint64 model_id = 0;
        SliceHatchLine line;
        in >> model_id;
        in >> line.start[0] >> line.start[1] >> line.end[0] >> line.end[1];
        line.model_id = model_id;
        if (in.status() != QDataStream::Ok)
        {
            error = QStringLiteral("填充数据不完整。");
            return false;
        }
        layer.hatch_path_length_mm +=
            std::hypot(line.end[0] - line.start[0],
                       line.end[1] - line.start[1]);
        layer.hatches.push_back(line);
    }

    if (in.status() != QDataStream::Ok || !in.atEnd())
    {
        error = QStringLiteral("层数据长度与内容不一致。");
        return false;
    }

    layer.path_length_mm =
        layer.contour_path_length_mm + layer.hatch_path_length_mm;
    summary.contour_length_mm += layer.contour_path_length_mm;
    summary.hatch_length_mm += layer.hatch_path_length_mm;
    summary.contours += layer.contours.size();
    summary.hatches += layer.hatches.size();
    return true;
}
} // namespace

bool WriteSlmSliceV1(const SliceBuildSnapshot& snapshot,
                    const SliceExportOptions& options,
                    const SliceExportProgress& progress,
                    SliceExportResult& result)
{
    const QFileInfo output_info(options.output_path);
    const QDir directory = output_info.absoluteDir();
    if (!directory.exists())
    {
        result.error = QStringLiteral("输出目录不存在。");
        return false;
    }

    std::vector<const SliceLayer*> selected;
    selected.reserve(snapshot.layers.size());
    for (const SliceLayer& layer : snapshot.layers)
        if (options.include_empty_layers || !layer.empty)
            selected.push_back(&layer);

    if (selected.empty())
    {
        result.error = QStringLiteral("没有可导出的层。");
        return false;
    }

    const double thickness = snapshot.settings.layer_thickness_mm;

    QTemporaryFile payload(directory.filePath(QStringLiteral(".slmslice-payload-XXXXXX.tmp")));
    payload.setAutoRemove(true);
    if (!payload.open())
    {
        result.error = QStringLiteral("无法创建临时文件：%1").arg(payload.errorString());
        return false;
    }

    QJsonArray checksums;
    quint64 payload_bytes = 0;
    for (std::size_t i = 0; i < selected.size(); ++i)
    {
        const SliceLayer& layer = *selected[i];
        QByteArray blob;
        double contour_length = 0.0;
        double hatch_length = 0.0;
        WriteLayerBlob(layer, options.include_hatches, blob,
                       contour_length, hatch_length);
        const QByteArray digest =
            QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toHex();
        QJsonObject entry;
        entry.insert(QStringLiteral("grid_index"),
                     static_cast<qint64>(layer.grid_index));
        entry.insert(QStringLiteral("bytes"), static_cast<qint64>(blob.size()));
        entry.insert(QStringLiteral("sha256"), QString::fromLatin1(digest));
        checksums.append(entry);

        if (payload.write(blob) != blob.size())
        {
            result.error = QStringLiteral("写入临时文件失败：%1")
                .arg(payload.errorString());
            return false;
        }
        payload_bytes += static_cast<quint64>(blob.size());
        result.layers_written = i + 1;
        result.contours_written += layer.contours.size();
        if (options.include_hatches)
            result.hatches_written += layer.hatches.size();
        if (progress && !progress(i + 1, selected.size()))
        {
            result.cancelled = true;
            result.error = QStringLiteral("已取消导出。");
            return false;
        }
    }

    QJsonObject slice;
    slice.insert(QStringLiteral("layer_thickness_mm"), thickness);
    slice.insert(QStringLiteral("z_reference"), QStringLiteral("layer_midplane"));
    slice.insert(QStringLiteral("first_grid_index"),
                 static_cast<qint64>(selected.front()->grid_index));
    slice.insert(QStringLiteral("last_grid_index"),
                 static_cast<qint64>(selected.back()->grid_index));
    slice.insert(QStringLiteral("layer_count"),
                 static_cast<qint64>(selected.size()));
    slice.insert(QStringLiteral("include_empty_layers"),
                 options.include_empty_layers);
    slice.insert(QStringLiteral("settings"), HatchSettingsJson(snapshot.settings.hatch));

    QJsonArray parts;
    for (const SlicePartInfo& part : snapshot.parts)
    {
        QJsonObject object;
        object.insert(QStringLiteral("id"), static_cast<qint64>(part.id));
        object.insert(QStringLiteral("name"), part.name);
        object.insert(QStringLiteral("source_path"), part.source_path);
        object.insert(QStringLiteral("triangle_count"),
                      static_cast<qint64>(part.triangle_count));
        // Optional field: only support parts are tagged, so a snapshot without
        // supports writes a byte-identical v1 header. Model parts omit "kind"
        // and are read back as SlicePartKind::Model (the default).
        if (part.kind == SlicePartKind::Support)
            object.insert(QStringLiteral("kind"), QStringLiteral("support"));
        parts.append(object);
    }

    QJsonObject coordinate_system;
    coordinate_system.insert(QStringLiteral("unit"), QStringLiteral("mm"));
    coordinate_system.insert(QStringLiteral("origin"),
                             QStringLiteral("plate_front_left_bottom"));
    coordinate_system.insert(QStringLiteral("up"), QStringLiteral("+Z"));
    coordinate_system.insert(QStringLiteral("z_reference"),
                             QStringLiteral("layer_midplane"));

    QJsonObject layers_info;
    layers_info.insert(QStringLiteral("encoding"), QStringLiteral("binary-le"));
    layers_info.insert(QStringLiteral("point_type"), QStringLiteral("float64"));
    layers_info.insert(QStringLiteral("layer_header"),
                       QStringLiteral("grid_index:int64,z_mm:float64,"
                                      "flags:uint32,ring_count:uint32,"
                                      "hatch_count:uint32"));
    layers_info.insert(QStringLiteral("byte_length"),
                       static_cast<qint64>(payload_bytes));
    layers_info.insert(QStringLiteral("checksums"), checksums);

    QJsonObject app;
    app.insert(QStringLiteral("name"), QStringLiteral("openSLMSlicer"));
    app.insert(QStringLiteral("version"), snapshot.app_version);

    QJsonObject header;
    header.insert(QStringLiteral("format"), QStringLiteral("slmslice"));
    header.insert(QStringLiteral("version"), static_cast<int>(kVersion));
    header.insert(QStringLiteral("app"), app);
    header.insert(QStringLiteral("created_utc"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    header.insert(QStringLiteral("coordinate_system"), coordinate_system);
    QJsonArray plate;
    for (double value : snapshot.plate_size_mm)
        plate.append(value);
    header.insert(QStringLiteral("plate_mm"), plate);
    header.insert(QStringLiteral("slice"), slice);
    header.insert(QStringLiteral("parts"), parts);
    header.insert(QStringLiteral("source_project_revision"),
                  static_cast<qint64>(snapshot.source_project_revision));
    header.insert(QStringLiteral("layers"), layers_info);
    const QByteArray header_bytes =
        QJsonDocument(header).toJson(QJsonDocument::Indented);
    if (header_bytes.size() > static_cast<int>(kMaxHeaderBytes))
    {
        result.error = QStringLiteral("切片头信息过大。");
        return false;
    }

    QSaveFile output(options.output_path);
    if (!output.open(QIODevice::WriteOnly))
    {
        result.error = QStringLiteral("无法写入导出文件：%1")
            .arg(output.errorString());
        return false;
    }
    QDataStream out(&output);
    ConfigureStream(out);
    out.writeRawData(kMagic, sizeof(kMagic));
    out << kVersion << static_cast<quint32>(header_bytes.size());
    out.writeRawData(header_bytes.constData(), header_bytes.size());
    if (out.status() != QDataStream::Ok)
    {
        result.error = QStringLiteral("写入切片文件头失败。");
        output.cancelWriting();
        return false;
    }

    payload.seek(0);
    QByteArray buffer(1024 * 1024, '\0');
    while (!payload.atEnd())
    {
        const qint64 read = payload.read(buffer.data(), buffer.size());
        if (read <= 0)
            break;
        if (out.writeRawData(buffer.constData(), static_cast<int>(read)) != read)
        {
            result.error = QStringLiteral("写入切片数据失败。");
            output.cancelWriting();
            return false;
        }
    }
    if (out.status() != QDataStream::Ok || !output.commit())
    {
        result.error = QStringLiteral("保存切片文件失败：%1")
            .arg(output.errorString());
        return false;
    }

    result.bytes_written = static_cast<std::uint64_t>(QFileInfo(options.output_path).size());
    return true;
}

bool ReadSlmSliceV1(const QString& path, SliceBuildSnapshot& snapshot,
                   SliceFileSummary& summary, QString& error)
{
    summary = {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("无法读取切片文件：%1").arg(file.errorString());
        return false;
    }
    const qint64 file_size = file.size();
    if (file_size < 16)
    {
        error = QStringLiteral("切片文件不完整。");
        return false;
    }

    QDataStream in(&file);
    ConfigureStream(in);
    char magic[sizeof(kMagic)]{};
    if (in.readRawData(magic, sizeof(magic)) != sizeof(magic) ||
        !MatchesMagic(magic))
    {
        error = QStringLiteral("不是有效的 .slmslice 文件。");
        return false;
    }
    quint32 version = 0;
    quint32 header_bytes = 0;
    in >> version >> header_bytes;
    if (in.status() != QDataStream::Ok || version != kVersion ||
        header_bytes > kMaxHeaderBytes || file_size < 16 + header_bytes)
    {
        error = QStringLiteral("切片文件版本或头信息无效。");
        return false;
    }

    QByteArray header_raw(static_cast<int>(header_bytes), Qt::Uninitialized);
    if (in.readRawData(header_raw.data(), header_raw.size()) != header_raw.size())
    {
        error = QStringLiteral("读取切片文件头失败。");
        return false;
    }

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(header_raw, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("切片文件头不是有效 JSON。");
        return false;
    }
    const QJsonObject header = document.object();
    if (!IsSlmSliceFormatName(header.value(QStringLiteral("format")).toString()) ||
        header.value(QStringLiteral("version")).toInt() != static_cast<int>(kVersion))
    {
        error = QStringLiteral("不支持的 .slmslice 文件版本。");
        return false;
    }

    SliceBuildSnapshot loaded;
    const QJsonObject slice = header.value(QStringLiteral("slice")).toObject();
    loaded.settings.layer_thickness_mm =
        slice.value(QStringLiteral("layer_thickness_mm"))
            .toDouble(loaded.settings.layer_thickness_mm);
    ReadHatchSettingsJson(slice.value(QStringLiteral("settings")).toObject(),
                          loaded.settings.hatch);
    loaded.source_project_revision = static_cast<std::uint64_t>(
        header.value(QStringLiteral("source_project_revision")).toDouble(0.0));
    loaded.app_version = header.value(QStringLiteral("app")).toObject()
        .value(QStringLiteral("version")).toString();

    const QJsonArray plate = header.value(QStringLiteral("plate_mm")).toArray();
    for (int i = 0; i < 3 && i < plate.size(); ++i)
        loaded.plate_size_mm[static_cast<std::size_t>(i)] = plate.at(i).toDouble();

    const QJsonArray parts = header.value(QStringLiteral("parts")).toArray();
    loaded.parts.reserve(static_cast<std::size_t>(parts.size()));
    for (const QJsonValue& value : parts)
    {
        const QJsonObject object = value.toObject();
        SlicePartInfo part;
        part.id = static_cast<std::uint64_t>(object.value(QStringLiteral("id")).toDouble());
        part.name = object.value(QStringLiteral("name")).toString();
        part.source_path = object.value(QStringLiteral("source_path")).toString();
        part.triangle_count = static_cast<std::size_t>(
            object.value(QStringLiteral("triangle_count")).toDouble());
        part.kind = ReadPartKindJson(object);
        loaded.parts.push_back(std::move(part));
    }

    const QJsonObject layers_info = header.value(QStringLiteral("layers")).toObject();
    const QJsonArray checksums = layers_info.value(QStringLiteral("checksums")).toArray();
    if (checksums.isEmpty() || checksums.size() > static_cast<int>(kMaxLayerCount))
    {
        error = QStringLiteral("切片文件没有有效的层索引。");
        return false;
    }
    const qint64 payload_bytes =
        layers_info.value(QStringLiteral("byte_length")).toDouble(0.0);
    if (payload_bytes < 0 || 16 + static_cast<qint64>(header_bytes) + payload_bytes != file_size)
    {
        error = QStringLiteral("切片文件长度与头信息不一致。");
        return false;
    }

    qint64 offset = 16 + static_cast<qint64>(header_bytes);
    loaded.layers.reserve(static_cast<std::size_t>(checksums.size()));
    for (int i = 0; i < checksums.size(); ++i)
    {
        const QJsonObject entry = checksums.at(i).toObject();
        const qint64 bytes = entry.value(QStringLiteral("bytes")).toDouble(0.0);
        const QString expected = entry.value(QStringLiteral("sha256")).toString();
        if (bytes <= 0 || bytes > file_size - offset)
        {
            error = QStringLiteral("切片层长度无效。");
            return false;
        }
        QByteArray blob(static_cast<int>(bytes), '\0');
        if (in.readRawData(blob.data(), blob.size()) != blob.size())
        {
            error = QStringLiteral("读取切片层数据失败。");
            return false;
        }
        const QByteArray digest =
            QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toHex();
        if (QString::fromLatin1(digest).compare(expected, Qt::CaseInsensitive) != 0)
        {
            error = QStringLiteral("切片层校验和不匹配（第 %1 层）。").arg(i + 1);
            return false;
        }
        SliceLayer layer;
        if (!ReadLayerBlob(blob, layer, summary, error))
            return false;
        layer.index = static_cast<std::size_t>(i) + 1;
        loaded.layers.push_back(std::move(layer));
        offset += bytes;
    }
    if (offset != file_size)
    {
        error = QStringLiteral("切片文件末尾存在多余数据。");
        return false;
    }

    summary.layers = loaded.layers.size();
    snapshot = std::move(loaded);
    return true;
}

bool ReadSlmSliceV1Info(const QString& path, SliceFileInfo& info,
                       QString& error)
{
    info = {};

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("无法读取切片文件：%1").arg(file.errorString());
        return false;
    }
    const qint64 file_size = file.size();
    if (file_size < 16)
    {
        error = QStringLiteral("切片文件不完整。");
        return false;
    }

    QDataStream in(&file);
    ConfigureStream(in);
    char magic[sizeof(kMagic)]{};
    if (in.readRawData(magic, sizeof(magic)) != sizeof(magic) ||
        !MatchesMagic(magic))
    {
        error = QStringLiteral("不是有效的 .slmslice 文件。");
        return false;
    }
    quint32 version = 0;
    quint32 header_bytes = 0;
    in >> version >> header_bytes;
    if (in.status() != QDataStream::Ok || version != kVersion ||
        header_bytes > kMaxHeaderBytes || file_size < 16 + header_bytes)
    {
        error = QStringLiteral("切片文件版本或头信息无效。");
        return false;
    }

    QByteArray header_raw(static_cast<int>(header_bytes), '\0');
    if (in.readRawData(header_raw.data(), header_raw.size()) != header_raw.size())
    {
        error = QStringLiteral("读取切片文件头失败。");
        return false;
    }
    info.header_json = QString::fromUtf8(header_raw);

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(header_raw, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("切片文件头不是有效 JSON。");
        return false;
    }
    const QJsonObject header = document.object();
    if (!IsSlmSliceFormatName(header.value(QStringLiteral("format")).toString()) ||
        header.value(QStringLiteral("version")).toInt() != static_cast<int>(kVersion))
    {
        error = QStringLiteral("不支持的 .slmslice 文件版本。");
        return false;
    }

    info.app_version = header.value(QStringLiteral("app")).toObject()
        .value(QStringLiteral("version")).toString();
    info.created_utc = header.value(QStringLiteral("created_utc")).toString();
    const QJsonArray plate = header.value(QStringLiteral("plate_mm")).toArray();
    for (int i = 0; i < 3 && i < plate.size(); ++i)
        info.plate_size_mm[static_cast<std::size_t>(i)] = plate.at(i).toDouble();

    const QJsonObject slice = header.value(QStringLiteral("slice")).toObject();
    info.layer_thickness_mm = slice.value(QStringLiteral("layer_thickness_mm")).toDouble();
    info.include_empty_layers =
        slice.value(QStringLiteral("include_empty_layers")).toBool(true);

    const QJsonArray parts = header.value(QStringLiteral("parts")).toArray();
    info.parts.reserve(static_cast<std::size_t>(parts.size()));
    for (const QJsonValue& value : parts)
    {
        const QJsonObject object = value.toObject();
        SlicePartInfo part;
        part.id = static_cast<std::uint64_t>(
            object.value(QStringLiteral("id")).toDouble());
        part.name = object.value(QStringLiteral("name")).toString();
        part.source_path = object.value(QStringLiteral("source_path")).toString();
        part.triangle_count = static_cast<std::size_t>(
            object.value(QStringLiteral("triangle_count")).toDouble());
        part.kind = ReadPartKindJson(object);
        info.parts.push_back(std::move(part));
    }

    const QJsonObject layers_info = header.value(QStringLiteral("layers")).toObject();
    const QJsonArray checksums = layers_info.value(QStringLiteral("checksums")).toArray();
    if (checksums.isEmpty() || checksums.size() > static_cast<int>(kMaxLayerCount))
    {
        error = QStringLiteral("切片文件没有有效的层索引。");
        return false;
    }
    const qint64 payload_bytes =
        static_cast<qint64>(layers_info.value(QStringLiteral("byte_length")).toDouble(0.0));
    if (payload_bytes < 0 ||
        16 + static_cast<qint64>(header_bytes) + payload_bytes != file_size)
    {
        error = QStringLiteral("切片文件长度与头信息不一致。");
        return false;
    }

    qint64 offset = 16 + static_cast<qint64>(header_bytes);
    info.checksums.reserve(static_cast<std::size_t>(checksums.size()));
    for (const QJsonValue& value : checksums)
    {
        const QJsonObject entry = value.toObject();
        SliceLayerChecksum checksum;
        checksum.grid_index = static_cast<std::int64_t>(
            entry.value(QStringLiteral("grid_index")).toDouble());
        checksum.bytes = static_cast<std::uint64_t>(
            entry.value(QStringLiteral("bytes")).toDouble());
        checksum.sha256 = entry.value(QStringLiteral("sha256")).toString();
        if (checksum.bytes < 28 || checksum.bytes > static_cast<std::uint64_t>(file_size - offset))
        {
            error = QStringLiteral("切片层长度无效。");
            return false;
        }

        QByteArray layer_header(28, '\0');
        if (!file.seek(offset) ||
            file.read(layer_header.data(), layer_header.size()) != layer_header.size())
        {
            error = QStringLiteral("读取切片层头失败。");
            return false;
        }
        QDataStream layer_stream(layer_header);
        ConfigureStream(layer_stream);
        qint64 grid_index = 0;
        double z_mm = 0.0;
        quint32 flags = 0;
        quint32 ring_count = 0;
        quint32 hatch_count = 0;
        layer_stream >> grid_index >> z_mm >> flags >> ring_count >> hatch_count;
        if (layer_stream.status() != QDataStream::Ok ||
            ring_count > kMaxRingCount || hatch_count > kMaxHatchCount)
        {
            error = QStringLiteral("切片层头数据无效。");
            return false;
        }
        if ((flags & 1u) != 0u)
            ++info.empty_layer_count;
        info.contour_count += ring_count;
        info.hatch_count += hatch_count;
        info.checksums.push_back(std::move(checksum));
        offset += static_cast<qint64>(info.checksums.back().bytes);
    }

    if (offset != file_size)
    {
        error = QStringLiteral("切片文件末尾存在多余数据。");
        return false;
    }

    info.layer_count = info.checksums.size();
    return true;
}
