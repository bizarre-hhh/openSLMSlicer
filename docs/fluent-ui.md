# Qt 界面 Fluent 化

本工程的 Qt Widgets 界面（菜单、工具栏、停靠面板、对话框、状态栏）使用仓库内的
[Fluent-Qt](https://github.com/calvinhxx/Fluent-Qt) 组件库做 Fluent 风格美化；
**GLFW/OpenGL/ImGui 的 3D 视口不参与**，它是 `QWidget::createWindowContainer` 包住的
原生子窗口，绘制与交互完全由视口自己负责。

## 分层

| 位置 | 职责 |
| --- | --- |
| `openSLMSlicer/fluent_ui.h/.cpp` | 启动初始化、语义调色板、原生控件样式表、Fluent 控件工厂（`MakeLabel`/`MakeValueLabel`/`MakeButton`/`MakeSection`/`BuildDialog`） |
| `openSLMSlicer/main.cpp` | `FluentUi::PrepareApplication()`（QApplication 前）+ `FluentUi::Initialize()`（之后） |
| `3rd_party/fluent_qt/` | Fluent-Qt 1.11.0 裁剪副本 + Qt 5.14 兼容补丁，见 `FLUENTQT_VENDOR.md` |
| `openSLMSlicer/scene_editor_window.cpp` | 主窗口外壳：Fluent 菜单栏/菜单、工具栏、两个停靠面板、内联对话框 |
| `openSLMSlicer/welcome_dialog.cpp` 等 | 欢迎页、导出、切片信息对话框 |

## 组件替换规则

Fluent 组件的 Qt 基类与原生控件一致，所以替换只改类型、不改调用代码：

| 原生 | Fluent | 说明 |
| --- | --- | --- |
| `QMenuBar` / `QMenu` | `fluent::menus_toolbars::FluentMenuBar` / `FluentMenu` | 仍由 `QAction` 驱动，快捷键、状态和菜单项都不用改 |
| `QPushButton` | `fluent::basicinput::Button` | `Standard` / `Accent` / `Subtle` 三级 |
| `QCheckBox` | `fluent::basicinput::CheckBox` | |
| `QSlider` | `fluent::basicinput::Slider` | |
| `QLabel` | `fluent::textfields::Label` | 有排版角色与主/次文字色 |
| `QComboBox` | `fluent::basicinput::ComboBox` | |
| `QLineEdit` | `fluent::textfields::LineEdit` | |
| `QDialog` | 保留 `QDialog` | 原生窗口边框保留；内容用 `FluentUi::BuildDialog()` 组织 |
| `QGroupBox` | `fluent::layout::Card` 或保留 + 样式表 | |
| `QTreeWidget`/`QListWidget`/`QSpinBox`/`QDoubleSpinBox`/`QToolBar`/`QDockWidget`/`QStatusBar`/`QPlainTextEdit` | 保留原生 | 用语义调色板 + 生成的样式表统一外观；也保证测试里的 `findChild<类型>(对象名)` 不受影响 |

### 动态文字用 `MakeValueLabel`

`fluent::textfields::Label::setText()` 不是虚函数：通过 `QLabel*` 赋值后，Fluent 内部下一次
重排（省略号计算、主题更新）会把文字清空。凡是运行时会反复 `setText` 的数值/统计标签，
用 `FluentUi::MakeValueLabel()`（原生 `QLabel` + Fluent 字体和颜色）。

## 主题

- 目前固定 Light 主题；`FluentUi::SetDark(bool)` 已经能同时切换 Fluent token、原生控件
  调色板和样式表，接入“跟随系统/深色模式”只需在设置里调用它。
- 中文排版：Fluent 内置字体只有拉丁字形，Qt 5.14 对汉字会回退到宋体。启动时
  `FluentUi::Initialize()` 会挑选系统中文字体（Microsoft YaHei UI 优先），
  一部分通过 `ThemeRegistry::setFontFamilyOverride()` 影响 Fluent 组件，
  一部分通过 `QFont::setFamilies()` 作为原生控件的字形回退。
- 原生控件的颜色只写在本工程生成的样式表里（`FluentUi::StyleSheet()`），不散落在业务代码中；
  业务代码需要颜色时使用 `FluentUi::CurrentTokens()`。

## 验收

界面测试与截图模式在 `openSLMSlicer/tests/project_tests.cpp`：

```powershell
project_tests.exe                      # 全部界面/工程/切片回归
project_tests.exe --capture-window  out.png   # 主窗口（菜单栏/工具栏/停靠面板/状态栏）
project_tests.exe --capture-welcome out.png   # 欢迎页
project_tests.exe --capture-slice   out.png   # 切片面板与视口叠层
project_tests.exe --capture-model-transform out.png  # 模型变换对话框
```

运行截图模式时注意：如果进程被限制只能写工作区目录，请把 `TEMP`/`TMP` 指到工作区内可写目录
并把输出路径也放在那里，否则 `QTemporaryDir`/`QWidget::grab().save()` 会失败。

## 待办与限制

- 主窗口标题栏仍是 Windows 原生边框：Qt 5.14 没有 `QWindow::startSystemMove/Resize`，
  `fluent::windowing::Window` 的自绘标题栏只能走平台回退路径，风险较高；如果需要完整
  WinUI 外壳（Mica 背景），建议先升级到 Qt 5.15+/6 再评估。
- Windows 10 没有 Mica/Acrylic 系统背景，窗口材料只能使用纯色回退。
- 原生 `QGroupBox` 仍是 Qt 自绘边框，与 Fluent `Card` 的圆角/阴影略有差异；后续可以
  逐步替换成 `FluentUi::MakeSection()`。
