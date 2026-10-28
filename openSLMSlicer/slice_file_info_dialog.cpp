#include "slice_file_info_dialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QVBoxLayout>

namespace
{
QString FormatBytes(qint64 bytes)
{
    const QLocale locale;
    if (bytes < 1024)
        return QStringLiteral("%1 字节").arg(locale.toString(bytes));
    double value = static_cast<double>(bytes) / 1024.0;
    const char* units[] = {"KB", "MB", "GB", "TB"};
    int unit = 0;
    while (value >= 1024.0 && unit < 3)
    {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2")
        .arg(locale.toString(value, 'f', 2), QString::fromLatin1(units[unit]));
}

QString BuildSummary(const QString& path, const SliceFileInfo& info)
{
    const QFileInfo file(path);
    QString text;
    text += QStringLiteral("文件：%1\n").arg(file.absoluteFilePath());
    text += QStringLiteral("大小：%1\n").arg(FormatBytes(file.size()));
    text += QStringLiteral("格式：slmslice v1\n");
    text += QStringLiteral("应用版本：%1\n")
        .arg(info.app_version.isEmpty() ? QStringLiteral("（未记录）")
                                        : info.app_version);
    text += QStringLiteral("创建时间：%1\n")
        .arg(info.created_utc.isEmpty() ? QStringLiteral("（未记录）")
                                        : info.created_utc);
    text += QStringLiteral("平台：%1 × %2 × %3 mm\n")
        .arg(info.plate_size_mm[0], 0, 'f', 2)
        .arg(info.plate_size_mm[1], 0, 'f', 2)
        .arg(info.plate_size_mm[2], 0, 'f', 2);
    text += QStringLiteral("层厚：%1 mm\n").arg(info.layer_thickness_mm, 0, 'f', 4);
    text += QStringLiteral("层数：%1（其中空层 %2）\n")
        .arg(static_cast<qulonglong>(info.layer_count))
        .arg(static_cast<qulonglong>(info.empty_layer_count));
    text += QStringLiteral("写入时包含空层：%1\n")
        .arg(info.include_empty_layers ? QStringLiteral("是") : QStringLiteral("否"));
    text += QStringLiteral("轮廓：%1 条\n")
        .arg(static_cast<qulonglong>(info.contour_count));
    text += QStringLiteral("填充：%1 条\n")
        .arg(static_cast<qulonglong>(info.hatch_count));
    text += QStringLiteral("Part：%1 个\n")
        .arg(static_cast<qulonglong>(info.parts.size()));
    if (!info.parts.empty())
    {
        text += QStringLiteral("\nid | 三角面 | 名称 | 来源\n");
        for (const SlicePartInfo& part : info.parts)
        {
            text += QStringLiteral("%1 | %2 | %3 | %4\n")
                .arg(static_cast<qulonglong>(part.id))
                .arg(static_cast<qulonglong>(part.triangle_count))
                .arg(part.name.isEmpty() ? QStringLiteral("（未命名）") : part.name)
                .arg(part.source_path);
        }
    }
    return text;
}

QString BuildChecksums(const SliceFileInfo& info)
{
    QString text;
    for (const SliceLayerChecksum& checksum : info.checksums)
    {
        text += QStringLiteral("%1  %2  %3\n")
            .arg(checksum.grid_index)
            .arg(static_cast<qulonglong>(checksum.bytes))
            .arg(checksum.sha256);
    }
    return text;
}
} // namespace

SliceFileInfoDialog::SliceFileInfoDialog(const QString& path,
                                         const SliceFileInfo& info,
                                         QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("sliceFileInfoDialog"));
    setWindowTitle(QStringLiteral("切片文件信息"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumSize(760, 600);
    setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("sliceInfoTabs"));
    QFont mono(QStringLiteral("Consolas"), 9);

    summary_ = new QPlainTextEdit(this);
    summary_->setObjectName(QStringLiteral("sliceInfoSummary"));
    summary_->setReadOnly(true);
    summary_->setLineWrapMode(QPlainTextEdit::NoWrap);
    summary_->setFont(mono);
    summary_->setPlainText(BuildSummary(path, info));
    tabs_->addTab(summary_, QStringLiteral("摘要"));

    header_ = new QPlainTextEdit(this);
    header_->setObjectName(QStringLiteral("sliceInfoHeader"));
    header_->setReadOnly(true);
    header_->setLineWrapMode(QPlainTextEdit::NoWrap);
    header_->setFont(mono);
    header_->setPlainText(info.header_json);
    tabs_->addTab(header_, QStringLiteral("Header JSON"));

    checksums_ = new QPlainTextEdit(this);
    checksums_->setObjectName(QStringLiteral("sliceInfoChecksums"));
    checksums_->setReadOnly(true);
    checksums_->setLineWrapMode(QPlainTextEdit::NoWrap);
    checksums_->setFont(mono);
    checksums_->setPlainText(BuildChecksums(info));
    tabs_->addTab(checksums_, QStringLiteral("层校验和"));

    layout->addWidget(tabs_, 1);

    QDialogButtonBox* buttons =
        new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setObjectName(QStringLiteral("sliceInfoButtons"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
