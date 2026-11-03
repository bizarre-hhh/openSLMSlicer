#include "slice_file_info_dialog.h"

#include "fluent_ui.h"

#include "components/basicinput/Button.h"
#include "components/textfields/Label.h"
#include "design/Spacing.h"
#include "design/Typography.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
using fluent::textfields::Label;

QString CountText(std::size_t value)
{
    return QLocale().toString(static_cast<qulonglong>(value));
}

// 概览网格中的一格：上方是说明小字，下方是取值。
QWidget* StatCell(const QString& caption, const QString& value, QWidget* parent)
{
    auto* cell = new QWidget(parent);
    QVBoxLayout* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(Spacing::XSmall);
    layout->addWidget(FluentUi::MakeLabel(cell, caption,
                                          FluentUi::TextRole::Caption, true));
    layout->addWidget(FluentUi::MakeLabel(cell, value,
                                          FluentUi::TextRole::Subtitle));
    return cell;
}

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
    std::size_t support_part_count = 0;
    for (const SlicePartInfo& part : info.parts)
        if (part.kind == SlicePartKind::Support)
            ++support_part_count;
    text += QStringLiteral("Part：共 %1 个（支撑 %2 个）\n")
        .arg(static_cast<qulonglong>(info.parts.size()))
        .arg(static_cast<qulonglong>(support_part_count));
    if (!info.parts.empty())
    {
        text += QStringLiteral("\nid | 类型 | 三角面 | 名称 | 来源\n");
        for (const SlicePartInfo& part : info.parts)
        {
            text += QStringLiteral("%1 | %2 | %3 | %4 | %5\n")
                .arg(static_cast<qulonglong>(part.id))
                .arg(part.kind == SlicePartKind::Support
                         ? QStringLiteral("支撑")
                         : QStringLiteral("模型"))
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
    setMinimumSize(820, 620);
    // 对话框字体与 Fluent 排版、控件外观由集成层统一设置。
    FluentUi::PolishDialog(*this);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Spacing::Large, 20, Spacing::Large, 20);
    layout->setSpacing(Spacing::Gap::Loose);

    // ---------- 标题区：标题 + 文件路径 ----------
    const QFileInfo file(path);
    QLabel* title = FluentUi::MakeLabel(this, QStringLiteral("切片文件信息"),
                                        FluentUi::TextRole::Title);
    title->setObjectName(QStringLiteral("sliceInfoTitleLabel"));
    layout->addWidget(title);

    auto* path_label = new Label(this);
    path_label->setObjectName(QStringLiteral("sliceInfoPathLabel"));
    path_label->setText(QDir::toNativeSeparators(file.absoluteFilePath()));
    path_label->setFluentTypography(Typography::FontRole::Caption);
    path_label->setTextColorRole(Label::TextColorRole::Secondary);
    path_label->setTextElideMode(Qt::ElideMiddle);
    path_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    path_label->setToolTip(QDir::toNativeSeparators(file.absoluteFilePath()));
    layout->addWidget(path_label);

    // ---------- 概览卡片：用可读的标签行代替拥挤的表单 ----------
    std::size_t support_part_count = 0;
    for (const SlicePartInfo& part : info.parts)
        if (part.kind == SlicePartKind::Support)
            ++support_part_count;

    QVBoxLayout* stats_body = nullptr;
    QWidget* stats_card = FluentUi::MakeSection(
        this, QStringLiteral("概览"), &stats_body);
    stats_card->setObjectName(QStringLiteral("sliceInfoStatsCard"));

    QGridLayout* stats_grid = new QGridLayout;
    stats_grid->setContentsMargins(0, 0, 0, 0);
    stats_grid->setHorizontalSpacing(Spacing::Standard);
    stats_grid->setVerticalSpacing(Spacing::Medium);
    const QString stats[8][2] = {
        {QStringLiteral("层数"), CountText(info.layer_count)},
        {QStringLiteral("空层"), CountText(info.empty_layer_count)},
        {QStringLiteral("层厚"),
         QStringLiteral("%1 mm").arg(info.layer_thickness_mm, 0, 'f', 3)},
        {QStringLiteral("文件大小"), FormatBytes(file.size())},
        {QStringLiteral("轮廓"),
         QStringLiteral("%1 条").arg(CountText(info.contour_count))},
        {QStringLiteral("填充"),
         QStringLiteral("%1 条").arg(CountText(info.hatch_count))},
        {QStringLiteral("Part"), CountText(info.parts.size())},
        {QStringLiteral("支撑"), CountText(support_part_count)},
    };
    for (int i = 0; i < 8; ++i)
    {
        const int row = i / 4;
        const int column = i % 4;
        stats_grid->addWidget(StatCell(stats[i][0], stats[i][1], stats_card),
                              row, column);
        stats_grid->setColumnStretch(column, 1);
    }
    stats_body->addLayout(stats_grid);
    layout->addWidget(stats_card);

    // ---------- 明细标签页：保持原生 QTabWidget / QPlainTextEdit ----------
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

    // ---------- 底部操作按钮 ----------
    QWidget* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("sliceInfoButtons"));
    QHBoxLayout* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(0, 0, 0, 0);
    footer_layout->setSpacing(Spacing::Gap::Normal);
    footer_layout->addStretch(1);
    auto* close_button = FluentUi::MakeButton(
        footer, QStringLiteral("关闭"), FluentUi::ButtonKind::Standard);
    close_button->setObjectName(QStringLiteral("sliceInfoCloseButton"));
    close_button->setMinimumWidth(96);
    close_button->setMinimumHeight(Spacing::ControlHeight::Standard);
    footer_layout->addWidget(close_button);
    layout->addWidget(footer);
    connect(close_button, &QPushButton::clicked, this, &QDialog::reject);
}
