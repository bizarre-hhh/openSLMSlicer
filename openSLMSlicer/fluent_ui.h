#ifndef FLUENT_UI_H
#define FLUENT_UI_H

#include <QColor>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QWidget>

class QApplication;
class QDialog;
class QLabel;
class QMainWindow;
class QMenu;
class QMenuBar;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace fluent::textfields
{
class Label;
}
namespace fluent::basicinput
{
class Button;
}

// Fluent-Qt 集成层：只负责 Qt 界面与控件，不涉及 GLFW/OpenGL/ImGui 的 3D 视口。
//
// 设计原则（与 tests/project_tests.cpp 的控件查找方式保持一致）：
//  * 测试依赖的 Qt 基类保持不动：QTreeWidget、QListWidget、QSpinBox、
//    QDoubleSpinBox、QToolBar、QToolButton、QStatusBar、QDialog 的对象名和类型
//    都保留，这些原生控件用语义化调色板 + 生成的样式表做主题化；
//  * 基类兼容的控件换成 Fluent 组件：QMenuBar -> FluentMenuBar、QMenu ->
//    FluentMenu、QPushButton -> fluent::basicinput::Button、QLabel ->
//    fluent::textfields::Label、QCheckBox -> fluent::basicinput::CheckBox、
//    QSlider -> fluent::basicinput::Slider。
namespace FluentUi
{
// 主题的语义色，供需要手写绘制的少数位置使用（行背景、状态色等）。
struct Tokens
{
    QColor canvas;         // 窗口/内容画布
    QColor layer;          // 面板层（停靠面板、状态栏）
    QColor layer_alt;      // 交替层（标题行、表头）
    QColor solid;          // 列表/输入框底
    QColor text_primary;
    QColor text_secondary;
    QColor text_tertiary;
    QColor text_disabled;
    QColor text_on_accent;
    QColor accent;
    QColor accent_hover;
    QColor accent_subtle;  // 选中行背景等低饱和强调色
    QColor control_default;
    QColor control_secondary;
    QColor control_tertiary;
    QColor stroke;
    QColor stroke_divider;
    QColor system_critical;
    QColor system_caution;
    QColor system_success;
};

// QApplication 创建前调用：开启 Fluent 需要的 High-DPI 行为。
void PrepareApplication();

// QApplication 创建后、创建任何窗口前调用一次：
// 注册内置字体/资源、设置正文字体、安装 Fusion 基础样式、语义化调色板与样式表。
void Initialize(QApplication& app);

bool IsDark();
// 切换明暗主题：Fluent 组件、原生控件调色板和样式表一起更新。
void SetDark(bool dark);

Tokens CurrentTokens();

// 把窗口的菜单栏换成 Fluent 菜单栏，返回新的菜单栏。
QMenuBar* InstallMenuBar(QMainWindow& window);
// 在菜单栏上添加一个 Fluent 菜单（QMenu 的 Fluent 子类）。
QMenu* CreateMenu(QMenuBar& menu_bar, const QString& title);

// 主窗口的非菜单装饰：状态栏、停靠面板等。
void PolishMainWindow(QMainWindow& window);
// 对话框的公共设置：字体、图标、内容边距。
void PolishDialog(QDialog& dialog);

// 生成的样式表（原生控件部分）。测试或文档需要时可以单独取用。
QString StyleSheet();

// ---- 组件工厂：返回值保持 Qt 基类，既有代码和测试的类型查找继续有效 ----

// Fluent 排版角色，对应 Typography::FontRole 的常用子集。
enum class TextRole
{
    Caption,
    Body,
    Strong,
    Subtitle,
    Title,
};

// 文字标签：Fluent Label（QLabel 子类），可指定排版角色与主/次文本色。
fluent::textfields::Label* MakeLabel(QWidget* parent, const QString& text = QString(),
                                     TextRole role = TextRole::Body, bool secondary = false);

// 运行时反复 setText 的取值标签：原生 QLabel + 主题字体/颜色。
// Fluent Label 的 setText 不是虚函数，通过 QLabel* 赋值会在其内部重排时被清空，
// 因此动态数值用这个原生版本（字体和颜色仍来自 Fluent token）。
QLabel* MakeValueLabel(QWidget* parent, const QString& text = QString(),
                       TextRole role = TextRole::Body, bool secondary = false);

// 按钮：Fluent Button（QPushButton 子类）。Accent 用于每个区域唯一的主操作。
enum class ButtonKind
{
    Standard,
    Accent,
    Subtle,
};

enum class ButtonScale
{
    Small,
    Standard,
    Large,
};

fluent::basicinput::Button* MakeButton(QWidget* parent, const QString& text,
                                       ButtonKind kind = ButtonKind::Standard,
                                       ButtonScale scale = ButtonScale::Standard);

// 卡片式分区：标题 + 分隔线 + 内容布局，返回的控件已带好边框和圆角。
// body 返回内容布局，调用方把控件加进去即可。
QWidget* MakeSection(QWidget* parent, const QString& title, QVBoxLayout** body);

// 对话框骨架：标题、说明、内容区和底部按钮行（Fluent 按钮，主操作强调色）。
// 调用方负责连接 primary/secondary 到 accept/reject 以及填充 body。
struct DialogParts
{
    QVBoxLayout* body = nullptr;
    fluent::basicinput::Button* primary = nullptr;
    fluent::basicinput::Button* secondary = nullptr;
};
DialogParts BuildDialog(QDialog& dialog, const QString& title, const QString& description,
                        const QString& primary_text, const QString& secondary_text);

} // namespace FluentUi

#endif // FLUENT_UI_H
