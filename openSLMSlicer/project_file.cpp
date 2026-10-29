#include "project_file.h"
#include "model_names.h"

#include <QDataStream>
#include <QFile>
#include <QSaveFile>

#include <cmath>
#include <utility>

namespace
{
constexpr quint32 kMagic = 0x534C4D50; // SLMP
constexpr quint32 kLegacyMagic = 0x474C5052; // GLPR from the pre-rename format
constexpr quint32 kVersion = 6;
constexpr quint32 kMaxModels = 10000;
constexpr quint32 kMaxSupportGroups = 10000;
constexpr quint32 kMaxPathBytes = 1024 * 1024;
// Qt 5 QByteArray is int-sized. Bound allocations before reading file data.
constexpr quint32 kMaxModelBytes = 512 * 1024 * 1024;

void Configure(QDataStream& stream)
{
    stream.setVersion(QDataStream::Qt_5_12);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
}

bool ReadBytes(QDataStream& stream, QByteArray& bytes, quint32 limit)
{
    quint32 size = 0;
    stream >> size;
    if (stream.status() != QDataStream::Ok || size > limit ||
        size > static_cast<quint64>(stream.device()->bytesAvailable()))
        return false;
    bytes.resize(static_cast<int>(size));
    return stream.readRawData(bytes.data(), bytes.size()) == bytes.size();
}

void WriteBytes(QDataStream& stream, const QByteArray& bytes)
{
    stream << static_cast<quint32>(bytes.size());
    stream.writeRawData(bytes.constData(), static_cast<int>(bytes.size()));
}

bool ReadBool(QDataStream& stream, bool& value)
{
    quint8 byte = 0;
    stream >> byte;
    value = byte != 0;
    return stream.status() == QDataStream::Ok && byte <= 1;
}

void ReadVector(QDataStream& stream, glm::vec3& value)
{
    stream >> value.x >> value.y >> value.z;
}

void WriteVector(QDataStream& stream, const glm::vec3& value)
{
    stream << value.x << value.y << value.z;
}

bool FiniteVector(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool ValidSupportState(SupportState state)
{
    return state == SupportState::Generated || state == SupportState::NotNeeded;
}

bool FiniteTriangle(const SliceTriangle& triangle)
{
    for (const auto& vertex : triangle.vertices)
        for (double value : vertex)
            if (!std::isfinite(value))
                return false;
    return true;
}

bool ValidateSupports(const ProjectData& project, QString& error)
{
    if (project.supports.size() > kMaxSupportGroups)
    {
        error = QStringLiteral("工程中的支撑数量超过上限。");
        return false;
    }
    for (const auto& support : project.supports)
    {
        if (support.owner_model_index >= project.models.size())
        {
            error = QStringLiteral("工程中的支撑关联到不存在的模型。");
            return false;
        }
        if (!ValidSupportState(support.state))
        {
            error = QStringLiteral("工程中的支撑状态无效。");
            return false;
        }
        if (!std::isfinite(support.overhang_angle_deg) ||
            support.overhang_angle_deg < 0.0 || support.overhang_angle_deg > 89.0 ||
            !std::isfinite(support.volume_mm3) || support.volume_mm3 < 0.0)
        {
            error = QStringLiteral("工程中的支撑参数无效。");
            return false;
        }
        if (support.local_triangles.size() > kMaxSupportTrianglesPerModel)
        {
            error = QStringLiteral("工程中的支撑三角面数量超过上限。");
            return false;
        }
        if (support.state == SupportState::Generated && support.local_triangles.empty())
        {
            error = QStringLiteral("工程中的支撑几何缺失。");
            return false;
        }
        if (support.state == SupportState::NotNeeded && !support.local_triangles.empty())
        {
            error = QStringLiteral("工程中的支撑数据不一致。");
            return false;
        }
        for (const auto& triangle : support.local_triangles)
            if (!FiniteTriangle(triangle))
            {
                error = QStringLiteral("工程中的支撑几何包含非有限坐标。");
                return false;
            }
    }
    return true;
}
} // namespace

QString ProjectFile::Filter()
{
    return QStringLiteral("openSLMSlicer 工程 (*.slmproj)");
}

QString ProjectFile::DefaultOpenDirectory()
{
    return QStringLiteral("D:/vs_cmake_proj/openSLMSlicer/slmproj");
}

bool ProjectFile::Validate(const ProjectData& project, QString& error)
{
    error.clear();
    if (!PlateDimensions::IsValid(project.plate_size_mm))
    {
        error = QStringLiteral("平台长度、宽度和高度必须在 1 至 10000 mm 之间。");
        return false;
    }
    bool valid = project.models.size() <= kMaxModels;
    QSet<QString> names;
    for (const auto& model : project.models)
    {
        // Empty names are allowed for programmatic/legacy documents and derived on load/save.
        if (!model.name.isEmpty())
        {
            if (!ModelNames::IsValid(model.name) || names.contains(model.name.toCaseFolded()))
            {
                error = QStringLiteral("工程中的模型名称无效或重复。");
                return false;
            }
            names.insert(model.name.toCaseFolded());
        }
        valid = valid && !model.source_path.isEmpty() &&
            model.source_path.toUtf8().size() <= kMaxPathBytes &&
            !model.stl_data.isEmpty() && model.stl_data.size() <= kMaxModelBytes &&
            FiniteVector(model.translation_mm) && FiniteVector(model.scale) &&
            FiniteVector(model.rotation_degrees) &&
            std::isfinite(model.placement_center_mm.x) && std::isfinite(model.placement_center_mm.y) &&
            model.scale.x > 0.0f && model.scale.y > 0.0f && model.scale.z > 0.0f;
    }
    if (!valid)
    {
        error = QStringLiteral("工程包含无效的模型数据或变换参数。");
        return false;
    }
    return ValidateSupports(project, error);
}

bool ProjectFile::Read(const QString& path, ProjectData& project, QString& error)
{
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("无法读取工程：%1").arg(file.errorString());
        return false;
    }
    QDataStream stream(&file);
    Configure(stream);
    quint32 magic = 0, version = 0, count = 0;
    stream >> magic >> version;
    if (stream.status() != QDataStream::Ok || (magic != kMagic && magic != kLegacyMagic))
    {
        error = QStringLiteral("这不是有效的 openSLMSlicer 工程文件。");
        return false;
    }
    if (version < 1 || version > kVersion)
    {
        error = QStringLiteral("不支持工程版本 %1，请使用兼容的软件版本。").arg(version);
        return false;
    }

    // Parse into a temporary document; failures never replace the caller's data.
    ProjectData loaded;
    QSet<QString> names;
    bool valid = true;
    if (version == 1)
    {
        // Consume the legacy camera/display fields; only geometry is restored.
        float ignored = 0.0f;
        for (int i = 0; i < 12; ++i)
            stream >> ignored;
        bool flag = false;
        valid = ReadBool(stream, flag) && ReadBool(stream, flag);
    }
    if (version >= 3)
    {
        stream >> loaded.plate_size_mm.x >> loaded.plate_size_mm.y;
        if (version >= 5)
            stream >> loaded.plate_size_mm.z;
    }
    stream >> count;
    valid = valid && stream.status() == QDataStream::Ok && count <= kMaxModels;
    for (quint32 i = 0; valid && i < count; ++i)
    {
        ProjectModel model;
        QByteArray utf8_path;
        valid = ReadBytes(stream, utf8_path, kMaxPathBytes) &&
                ReadBytes(stream, model.stl_data, kMaxModelBytes);
        if (!valid)
            break;
        model.source_path = QString::fromUtf8(utf8_path);
        ReadVector(stream, model.translation_mm);
        ReadVector(stream, model.scale);
        ReadVector(stream, model.rotation_degrees);
        if (version >= 3)
            stream >> model.placement_center_mm.x >> model.placement_center_mm.y;
        if (version >= 4)
        {
            QByteArray utf8_name;
            valid = ReadBytes(stream, utf8_name, ModelNames::kMaxUtf8Bytes);
            model.name = QString::fromUtf8(utf8_name);
            valid = valid && ModelNames::IsValid(model.name) && model.name.toUtf8() == utf8_name;
        }
        else
            model.name = ModelNames::Unique(ModelNames::FromPath(model.source_path), names);
        if (version == 1)
        {
            bool flag = false;
            valid = ReadBool(stream, flag) && ReadBool(stream, flag) && ReadBool(stream, flag);
            quint64 ignored_order = 0;
            stream >> ignored_order;
        }
        valid = valid && stream.status() == QDataStream::Ok;
        loaded.models.push_back(std::move(model));
    }

    // Version 6 adds support groups after the model list. Older versions simply
    // have none, which keeps them fully readable.
    if (valid && version >= 6)
    {
        quint32 support_count = 0;
        stream >> support_count;
        valid = stream.status() == QDataStream::Ok && support_count <= kMaxSupportGroups;
        for (quint32 i = 0; valid && i < support_count; ++i)
        {
            ProjectSupportRecord support;
            quint32 owner_index = 0;
            quint8 state = 0;
            float overhang = 0.0f;
            float volume = 0.0f;
            quint32 triangle_count = 0;
            stream >> owner_index >> state >> overhang >> volume >> triangle_count;
            valid = stream.status() == QDataStream::Ok &&
                    owner_index < loaded.models.size() && state <= 2 &&
                    triangle_count <= kMaxSupportTrianglesPerModel;
            if (!valid)
                break;
            support.owner_model_index = owner_index;
            support.state = static_cast<SupportState>(state);
            support.overhang_angle_deg = overhang;
            support.volume_mm3 = volume;
            support.local_triangles.resize(triangle_count);
            for (auto& triangle : support.local_triangles)
            {
                for (auto& vertex : triangle.vertices)
                    for (double& value : vertex)
                    {
                        float component = 0.0f;
                        stream >> component;
                        value = component;
                    }
            }
            valid = valid && stream.status() == QDataStream::Ok;
            loaded.supports.push_back(std::move(support));
        }
    }

    if (!valid || stream.status() != QDataStream::Ok || !file.atEnd())
    {
        error = QStringLiteral("工程文件已损坏、数据不完整或超过支持的大小。");
        return false;
    }
    if (!Validate(loaded, error))
        return false;
    project = std::move(loaded);
    return true;
}

bool ProjectFile::Write(const QString& path, const ProjectData& project, QString& error)
{
    if (!Validate(project, error))
        return false;
    // Atomic replacement: a failed write leaves an existing project intact.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        error = QStringLiteral("无法保存工程：%1").arg(file.errorString());
        return false;
    }
    QDataStream stream(&file);
    Configure(stream);
    stream << kMagic << kVersion;
    stream << project.plate_size_mm.x << project.plate_size_mm.y
           << project.plate_size_mm.z;
    stream << static_cast<quint32>(project.models.size());
    QSet<QString> names;
    for (const auto& model : project.models)
        if (!model.name.isEmpty()) names.insert(model.name.toCaseFolded());
    for (const auto& model : project.models)
    {
        WriteBytes(stream, model.source_path.toUtf8());
        WriteBytes(stream, model.stl_data);
        WriteVector(stream, model.translation_mm);
        WriteVector(stream, model.scale);
        WriteVector(stream, model.rotation_degrees);
        stream << model.placement_center_mm.x << model.placement_center_mm.y;
        WriteBytes(stream, (model.name.isEmpty()
            ? ModelNames::Unique(ModelNames::FromPath(model.source_path), names) : model.name).toUtf8());
    }
    stream << static_cast<quint32>(project.supports.size());
    for (const auto& support : project.supports)
    {
        stream << static_cast<quint32>(support.owner_model_index)
               << static_cast<quint8>(support.state)
               << static_cast<float>(support.overhang_angle_deg)
               << static_cast<float>(support.volume_mm3)
               << static_cast<quint32>(support.local_triangles.size());
        for (const auto& triangle : support.local_triangles)
            for (const auto& vertex : triangle.vertices)
                for (double value : vertex)
                    stream << static_cast<float>(value);
    }
    if (stream.status() != QDataStream::Ok || !file.commit())
    {
        error = QStringLiteral("保存工程失败：%1").arg(file.errorString());
        return false;
    }
    return true;
}
