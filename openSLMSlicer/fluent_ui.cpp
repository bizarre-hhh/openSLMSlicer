#include "fluent_ui.h"

#include <FluentQt/FluentQt.h>

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPalette>
#include <QPushButton>
#include <QStatusBar>
#include <QStyleFactory>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
// 语义色 -> CSS 颜色。Fluent token 里有半透明色，统一用 rgba 输出。
QString Css(const QColor& color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF(), 'f', 3));
}

// 把 over 以 alpha 混合到 base 上，得到不透明的等效色。
QColor Blend(const QColor& over, const QColor& base, qreal alpha)
{
    const qreal a = std::clamp(alpha, 0.0, 1.0);
    return QColor::fromRgbF(over.redF() * a + base.redF() * (1.0 - a),
                            over.greenF() * a + base.greenF() * (1.0 - a),
                            over.blueF() * a + base.blueF() * (1.0 - a));
}

const fluent::FluentElement::Colors& Palette(fluent::FluentElement::Theme theme)
{
    return fluent::ThemeRegistry::instance().colors(theme);
}

const fluent::FluentElement::Colors& ActivePalette()
{
    return Palette(fluent::FluentElement::currentTheme());
}

FluentUi::Tokens BuildTokens()
{
    const auto& colors = ActivePalette();
    const bool dark = fluent::FluentElement::themeUsesDarkAppearance(
        fluent::FluentElement::currentTheme());

    FluentUi::Tokens tokens;
    tokens.canvas = colors.bgCanvas;
    tokens.layer = colors.bgLayer;
    tokens.layer_alt = colors.bgLayerAlt;
    tokens.solid = dark ? colors.bgLayerAlt : colors.bgSolid;
    tokens.text_primary = colors.textPrimary;
    tokens.text_secondary = colors.textSecondary;
    tokens.text_tertiary = colors.textTertiary;
    tokens.text_disabled = colors.textDisabled;
    tokens.text_on_accent = colors.textOnAccent;
    tokens.accent = colors.accentDefault;
    tokens.accent_hover = colors.accentSecondary;
    tokens.accent_subtle = Blend(colors.accentDefault, colors.bgCanvas, dark ? 0.30 : 0.14);
    tokens.control_default = colors.controlDefault;
    tokens.control_secondary = colors.controlSecondary;
    tokens.control_tertiary = colors.controlTertiary;
    tokens.stroke = colors.strokeDefault;
    tokens.stroke_divider = colors.strokeDivider;
    tokens.system_critical = colors.systemCritical;
    tokens.system_caution = colors.systemCaution;
    tokens.system_success = colors.systemSuccess;
    return tokens;
}

QPalette BuildPalette(const FluentUi::Tokens& t)
{
    QPalette palette;
    palette.setColor(QPalette::Window, t.canvas);
    palette.setColor(QPalette::WindowText, t.text_primary);
    palette.setColor(QPalette::Base, t.solid);
    palette.setColor(QPalette::AlternateBase, t.layer);
    palette.setColor(QPalette::ToolTipBase, t.layer_alt);
    palette.setColor(QPalette::ToolTipText, t.text_primary);
    palette.setColor(QPalette::Text, t.text_primary);
    palette.setColor(QPalette::PlaceholderText, t.text_tertiary);
    palette.setColor(QPalette::Button, t.control_default);
    palette.setColor(QPalette::ButtonText, t.text_primary);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Light, t.canvas);
    palette.setColor(QPalette::Midlight, t.control_secondary);
    palette.setColor(QPalette::Dark, t.control_tertiary);
    palette.setColor(QPalette::Mid, t.stroke);
    palette.setColor(QPalette::Shadow, t.stroke);
    palette.setColor(QPalette::Highlight, t.accent);
    palette.setColor(QPalette::HighlightedText, t.text_on_accent);
    palette.setColor(QPalette::Link, t.accent);
    palette.setColor(QPalette::LinkVisited, t.accent_hover);

    palette.setColor(QPalette::Disabled, QPalette::WindowText, t.text_disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, t.text_disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, t.text_disabled);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, t.control_secondary);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, t.text_disabled);
    palette.setColor(QPalette::Disabled, QPalette::Base, t.canvas);
    palette.setColor(QPalette::Disabled, QPalette::Button, t.control_default);
    return palette;
}

// 原生控件的样式表。只写 Fluent 组件无法覆盖到的 Qt 类：
// Fluent（QMenuBar/QMenu/QLabel/QPushButton/... 的子类）由组件自己绘制，
// 这里不写它们的裸类选择器，避免样式表覆盖组件绘制。
QString BuildStyleSheet()
{
    const FluentUi::Tokens t = BuildTokens();
    const int radius = fluent::ThemeRegistry::instance().radius().control;

    QString sheet = QStringLiteral(R"(
QMainWindow {
    background: @canvas@;
}
QMainWindow::separator {
    background: @divider@;
    width: 1px;
    height: 1px;
}
QDialog {
    background: @canvas@;
}
QMessageBox {
    background: @canvas@;
}

/* ---------- 主工具栏 ---------- */
QToolBar#mainToolBar {
    background: @layer@;
    border: none;
    border-bottom: 1px solid @divider@;
    spacing: 2px;
    padding: 6px 10px;
}
QToolBar#mainToolBar QToolButton {
    min-width: 62px;
    padding: 5px 8px;
    border: none;
    border-radius: @radius@px;
    background: transparent;
    color: @textPrimary@;
    font-size: 12px;
}
QToolBar#mainToolBar QToolButton:hover {
    background: @controlSecondary@;
}
QToolBar#mainToolBar QToolButton:pressed {
    background: @controlTertiary@;
}
QToolBar#mainToolBar QToolButton:checked {
    background: @controlSecondary@;
}
QToolBar#mainToolBar QToolButton:disabled {
    color: @textDisabled@;
}
QToolBar#mainToolBar::separator {
    background: @divider@;
    width: 1px;
    margin: 8px 6px;
}
QToolBar#mainToolBar::handle {
    image: none;
    width: 0px;
}

/* ---------- 停靠面板 ---------- */
QDockWidget {
    color: @textPrimary@;
    font-size: 13px;
}
QDockWidget::title {
    background: @layer@;
    border: none;
    border-bottom: 1px solid @divider@;
    padding: 7px 10px;
    text-align: left;
}
QDockWidget::close-button, QDockWidget::float-button {
    background: transparent;
    border: none;
    padding: 2px;
}
QDockWidget::close-button:hover, QDockWidget::float-button:hover {
    background: @controlSecondary@;
    border-radius: @radius@px;
}

/* ---------- 状态栏 ---------- */
QStatusBar {
    background: @layer@;
    border-top: 1px solid @divider@;
    color: @textSecondary@;
    min-height: 28px;
}
QStatusBar::item {
    border: none;
}
QStatusBar QLabel {
    color: @textSecondary@;
}

/* ---------- 输入控件（保持原生类型，供测试和既有代码使用） ---------- */
QLineEdit, QPlainTextEdit, QTextEdit, QComboBox,
QSpinBox, QDoubleSpinBox, QAbstractSpinBox {
    background: @solid@;
    color: @textPrimary@;
    border: 1px solid @stroke@;
    border-radius: @radius@px;
    padding: 2px 6px;
    min-height: 24px;
    selection-background-color: @accent@;
    selection-color: @textOnAccent@;
}
QLineEdit:hover, QPlainTextEdit:hover, QComboBox:hover,
QSpinBox:hover, QDoubleSpinBox:hover {
    border-color: @textTertiary@;
}
QLineEdit:focus, QPlainTextEdit:focus, QComboBox:focus,
QSpinBox:focus, QDoubleSpinBox:focus {
    border-color: @accent@;
}
QLineEdit:read-only, QLineEdit:disabled, QPlainTextEdit:disabled,
QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    background: @layer@;
    color: @textDisabled@;
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QComboBox QAbstractItemView {
    background: @layerAlt@;
    color: @textPrimary@;
    border: 1px solid @stroke@;
    selection-background-color: @accentSubtle@;
    selection-color: @textPrimary@;
    outline: none;
}
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {
    width: 18px;
    border: none;
    background: transparent;
}
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover {
    background: @controlSecondary@;
}

/* ---------- 列表 / 树 ---------- */
QTreeWidget, QTreeView, QListWidget, QListView, QTableWidget, QTableView {
    background: @solid@;
    color: @textPrimary@;
    border: 1px solid @divider@;
    border-radius: @radius@px;
    outline: none;
    selection-background-color: @accentSubtle@;
    selection-color: @textPrimary@;
}
QTreeWidget::item, QTreeView::item, QListWidget::item, QListView::item {
    border: none;
    color: @textPrimary@;
}
QTreeWidget::item:hover, QTreeView::item:hover,
QListWidget::item:hover, QListView::item:hover {
    background: @controlSecondary@;
}
QTreeWidget::item:selected, QTreeView::item:selected,
QListWidget::item:selected, QListView::item:selected {
    background: @accentSubtle@;
    color: @textPrimary@;
}
QTreeWidget::item:disabled, QListWidget::item:disabled {
    color: @textDisabled@;
}
QHeaderView::section {
    background: @layerAlt@;
    color: @textSecondary@;
    border: none;
    border-bottom: 1px solid @divider@;
    padding: 5px 8px;
    font-size: 12px;
}
QTreeView::branch {
    background: transparent;
}

/* ---------- 滚动条 ---------- */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 2px;
}
QScrollBar::handle:vertical {
    background: @controlTertiary@;
    border-radius: 3px;
    min-height: 28px;
}
QScrollBar::handle:vertical:hover {
    background: @textTertiary@;
}
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 2px;
}
QScrollBar::handle:horizontal {
    background: @controlTertiary@;
    border-radius: 3px;
    min-width: 28px;
}
QScrollBar::handle:horizontal:hover {
    background: @textTertiary@;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0px;
    height: 0px;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}

/* ---------- 分组框 / 选项卡 / 分割条 ---------- */
QGroupBox {
    border: 1px solid @divider@;
    border-radius: @radius@px;
    margin-top: 14px;
    padding-top: 8px;
    color: @textPrimary@;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 10px;
    padding: 0px 4px;
    color: @textSecondary@;
    font-size: 12px;
}
QTabWidget::pane {
    border: 1px solid @divider@;
    border-radius: @radius@px;
    top: -1px;
}
QTabBar::tab {
    background: transparent;
    color: @textSecondary@;
    padding: 6px 14px;
    border: none;
    border-bottom: 2px solid transparent;
}
QTabBar::tab:hover {
    color: @textPrimary@;
    background: @controlSecondary@;
}
QTabBar::tab:selected {
    color: @textPrimary@;
    border-bottom: 2px solid @accent@;
}
QSplitter::handle {
    background: @divider@;
}
QScrollArea {
    background: transparent;
    border: none;
}

/* ---------- 行内小按钮（模型列表 / 面板） ---------- */
QToolButton {
    background: transparent;
    border: none;
    border-radius: @radius@px;
    padding: 3px;
    color: @textPrimary@;
}
/* 模型列表行里的按钮在窄面板中需要更紧凑，给模型名留出宽度。 */
QTreeWidget QToolButton {
    padding: 2px 3px;
    border-radius: 3px;
    font-size: 12px;
}
QToolButton:hover {
    background: @controlSecondary@;
}
QToolButton:pressed {
    background: @controlTertiary@;
}
QToolButton:checked {
    background: @controlSecondary@;
}
QToolButton:disabled {
    color: @textDisabled@;
}

/* ---------- 对话框按钮 --- 只影响 Qt 自带的按钮盒，不影响 Fluent Button --- */
QDialogButtonBox QPushButton {
    min-width: 84px;
    min-height: 26px;
    padding: 2px 14px;
    border: 1px solid @stroke@;
    border-radius: @radius@px;
    background: @controlDefault@;
    color: @textPrimary@;
}
QDialogButtonBox QPushButton:hover {
    background: @controlSecondary@;
}
QDialogButtonBox QPushButton:pressed {
    background: @controlTertiary@;
}
QDialogButtonBox QPushButton:default {
    background: @accent@;
    border-color: @accent@;
    color: @textOnAccent@;
}
QDialogButtonBox QPushButton:disabled {
    background: @controlDefault@;
    color: @textDisabled@;
}

/* ---------- 标签页内的表格/统计文字 ---------- */
QPlainTextEdit#modelPathEdit {
    background: transparent;
    border: none;
    color: @textSecondary@;
}
)");

    sheet.replace(QStringLiteral("@canvas@"), Css(t.canvas));
    sheet.replace(QStringLiteral("@layer@"), Css(t.layer));
    sheet.replace(QStringLiteral("@layerAlt@"), Css(t.layer_alt));
    sheet.replace(QStringLiteral("@solid@"), Css(t.solid));
    sheet.replace(QStringLiteral("@textPrimary@"), Css(t.text_primary));
    sheet.replace(QStringLiteral("@textSecondary@"), Css(t.text_secondary));
    sheet.replace(QStringLiteral("@textTertiary@"), Css(t.text_tertiary));
    sheet.replace(QStringLiteral("@textDisabled@"), Css(t.text_disabled));
    sheet.replace(QStringLiteral("@textOnAccent@"), Css(t.text_on_accent));
    sheet.replace(QStringLiteral("@accent@"), Css(t.accent));
    sheet.replace(QStringLiteral("@accentHover@"), Css(t.accent_hover));
    sheet.replace(QStringLiteral("@accentSubtle@"), Css(t.accent_subtle));
    sheet.replace(QStringLiteral("@controlDefault@"), Css(t.control_default));
    sheet.replace(QStringLiteral("@controlSecondary@"), Css(t.control_secondary));
    sheet.replace(QStringLiteral("@controlTertiary@"), Css(t.control_tertiary));
    sheet.replace(QStringLiteral("@stroke@"), Css(t.stroke));
    sheet.replace(QStringLiteral("@divider@"), Css(t.stroke_divider));
    sheet.replace(QStringLiteral("@radius@"), QString::number(radius));
    return sheet;
}

void ApplyThemeToApplication()
{
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
    {
        app->setPalette(BuildPalette(BuildTokens()));
        app->setStyleSheet(BuildStyleSheet());
    }
}
} // namespace

namespace
{
// Fluent 内置的 Inter 派生字体只有拉丁字形；Qt 5.14 对汉字会回退到宋体(SimSun)，
// 中文界面会变成衬线字体。这里选一个系统自带的中文无衬线字体作为 CJK 回退：
// 原生控件用 QFont::setFamilies 做「Inter + 中文字体」字形回退；Fluent 组件内部
// 只支持单一家族，通过 ThemeRegistry 的字族覆盖统一到中文字体（与原程序
// 之前显式使用 Microsoft YaHei 的外观一致）。
QString ResolveCjkFallbackFamily()
{
    static const QString resolved = []()
    {
        static const QStringList candidates{
            QStringLiteral("Microsoft YaHei UI"),
            QStringLiteral("Microsoft YaHei"),
            QStringLiteral("Microsoft JhengHei UI"),
            QStringLiteral("Noto Sans SC"),
            QStringLiteral("SimHei"),
        };
        const QStringList installed = QFontDatabase().families();
        for (const QString& candidate : candidates)
        {
            for (const QString& family : installed)
            {
                if (family.compare(candidate, Qt::CaseInsensitive) == 0)
                    return family;
            }
        }
        return QString();
    }();
    return resolved;
}

// 组合字号/字重与「拉丁 + 中文」字形回退，给原生控件和手写标签使用。
QFont ThemedFont(Typography::FontRole role)
{
    QFont font = Typography::fontStyle(role).toQFont();
    const QString cjk_family = ResolveCjkFallbackFamily();
    if (!cjk_family.isEmpty())
    {
        QStringList families{font.family()};
        if (families.front().compare(cjk_family, Qt::CaseInsensitive) != 0)
            families.push_back(cjk_family);
        font.setFamilies(families);
    }
    return font;
}
} // namespace

namespace FluentUi
{
void PrepareApplication()
{
    // 必须在 QApplication 构造前设置 High-DPI 属性。
    fluent::prepareHighDpiApplication();
}

void Initialize(QApplication& app)
{
    // 注册 Fluent 内置字体与图标资源（静态库里的 qrc）。
    fluent::initializeResources();

    const QString cjk_family = ResolveCjkFallbackFamily();
    app.setFont(ThemedFont(Typography::FontRole::Body));
    if (!cjk_family.isEmpty())
    {
        // Fluent 组件用单一的 token 字族，覆盖成中文字体，保证控件里的汉字无衬线。
        fluent::ThemeRegistry::instance().setFontFamilyOverride(cjk_family);
    }

    // Fusion 是唯一完整遵循调色板的 Qt 内置样式；Windows 的 vista 样式会忽略
    // 大部分 palette 设置，原生控件无法与 Fluent 组件统一外观。
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);

    fluent::FluentElement::setTheme(fluent::FluentElement::Light);
    ApplyThemeToApplication();
}

bool IsDark()
{
    return fluent::FluentElement::themeUsesDarkAppearance(fluent::FluentElement::currentTheme());
}

void SetDark(bool dark)
{
    const auto theme = dark ? fluent::FluentElement::Dark : fluent::FluentElement::Light;
    if (fluent::FluentElement::currentTheme() == theme)
        return;
    fluent::FluentElement::setTheme(theme);
    ApplyThemeToApplication();
}

Tokens CurrentTokens()
{
    return BuildTokens();
}

QMenuBar* InstallMenuBar(QMainWindow& window)
{
    auto* menu_bar = new fluent::menus_toolbars::FluentMenuBar(&window);
    window.setMenuBar(menu_bar);
    return menu_bar;
}

QMenu* CreateMenu(QMenuBar& menu_bar, const QString& title)
{
    auto* menu = new fluent::menus_toolbars::FluentMenu(title, &menu_bar);
    menu_bar.addMenu(menu);
    return menu;
}

void PolishMainWindow(QMainWindow& window)
{
    if (QStatusBar* bar = window.statusBar())
    {
        bar->setSizeGripEnabled(false);
        bar->setFont(ThemedFont(Typography::FontRole::Caption));
    }
    // 停靠面板保持 Qt 原生的拖拽/浮动/关闭行为，只由样式表统一标题栏外观。
    window.setDockNestingEnabled(false);
}

void PolishDialog(QDialog& dialog)
{
    dialog.setFont(ThemedFont(Typography::FontRole::Body));
}

QString StyleSheet()
{
    return BuildStyleSheet();
}

namespace
{
Typography::FontRole ToFontRole(FluentUi::TextRole role)
{
    switch (role)
    {
    case FluentUi::TextRole::Caption:
        return Typography::FontRole::Caption;
    case FluentUi::TextRole::Strong:
        return Typography::FontRole::BodyStrong;
    case FluentUi::TextRole::Subtitle:
        return Typography::FontRole::Subtitle;
    case FluentUi::TextRole::Title:
        return Typography::FontRole::Title;
    case FluentUi::TextRole::Body:
        break;
    }
    return Typography::FontRole::Body;
}
} // namespace

fluent::textfields::Label* MakeLabel(QWidget* parent, const QString& text, TextRole role,
                                     bool secondary)
{
    auto* label = new fluent::textfields::Label(text, parent);
    label->setFluentTypography(ToFontRole(role));
    label->setTextColorRole(secondary ? fluent::textfields::Label::TextColorRole::Secondary
                                      : fluent::textfields::Label::TextColorRole::Primary);
    return label;
}

QLabel* MakeValueLabel(QWidget* parent, const QString& text, TextRole role, bool secondary)
{
    auto* label = new QLabel(text, parent);
    label->setFont(ThemedFont(ToFontRole(role)));
    const auto& colors = ActivePalette();
    const QColor color = secondary ? colors.textSecondary : colors.textPrimary;
    // 显式颜色必须写在标签自身的样式表里：祖先样式表会让 Qt 忽略子控件调色板。
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(Css(color)));
    return label;
}

fluent::basicinput::Button* MakeButton(QWidget* parent, const QString& text, ButtonKind kind,
                                       ButtonScale scale)
{    auto* button = new fluent::basicinput::Button(text, parent);
    switch (kind)
    {
    case ButtonKind::Accent:
        button->setFluentStyle(fluent::basicinput::Button::Accent);
        break;
    case ButtonKind::Subtle:
        button->setFluentStyle(fluent::basicinput::Button::Subtle);
        break;
    case ButtonKind::Standard:
        break;
    }
    switch (scale)
    {
    case ButtonScale::Small:
        button->setFluentSize(fluent::basicinput::Button::Small);
        break;
    case ButtonScale::Large:
        button->setFluentSize(fluent::basicinput::Button::Large);
        break;
    case ButtonScale::Standard:
        break;
    }
    return button;
}

QWidget* MakeSection(QWidget* parent, const QString& title, QVBoxLayout** body)
{
    auto* card = new fluent::layout::Card(parent);
    card->setAppearance(fluent::layout::Card::Layer);
    card->setBorderVisible(true);

    auto* outer = new QVBoxLayout(card);
    outer->setContentsMargins(14, 12, 14, 14);
    outer->setSpacing(8);
    if (!title.isEmpty())
    {
        outer->addWidget(MakeLabel(card, title, TextRole::Strong));
        outer->addWidget(new fluent::layout::Divider(Qt::Horizontal, card));
    }
    auto* content = new QVBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(8);
    outer->addLayout(content);
    if (body)
        *body = content;
    return card;
}

DialogParts BuildDialog(QDialog& dialog, const QString& title, const QString& description,
                        const QString& primary_text, const QString& secondary_text)
{
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog.setFont(ThemedFont(Typography::FontRole::Body));

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);
    if (!title.isEmpty())
        root->addWidget(MakeLabel(&dialog, title, TextRole::Title));
    if (!description.isEmpty())
    {
        QLabel* description_label = MakeLabel(&dialog, description, TextRole::Caption, true);
        description_label->setWordWrap(true);
        root->addWidget(description_label);
    }

    DialogParts parts;
    parts.body = new QVBoxLayout;
    parts.body->setContentsMargins(0, 0, 0, 0);
    parts.body->setSpacing(12);
    root->addLayout(parts.body, 1);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 4, 0, 0);
    footer->setSpacing(8);
    footer->addStretch(1);
    if (!secondary_text.isEmpty())
    {
        parts.secondary = MakeButton(&dialog, secondary_text, ButtonKind::Standard);
        parts.secondary->setAutoDefault(true);
        footer->addWidget(parts.secondary);
    }
    if (!primary_text.isEmpty())
    {
        parts.primary = MakeButton(&dialog, primary_text, ButtonKind::Accent);
        parts.primary->setAutoDefault(true);
        parts.primary->setDefault(true);
        footer->addWidget(parts.primary);
    }
    root->addLayout(footer);
    return parts;
}
} // namespace FluentUi
