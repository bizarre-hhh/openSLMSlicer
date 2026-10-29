# openSLMSlicer

openSLMSlicer 是一个面向 SLM（选区激光熔化）的 Windows 桌面切片软件原型。它把模型排版和切片预览放在同一个三维场景中：导入 STL 后在平台上摆放、变换和阵列，设置层厚、扫描间距等参数，逐层查看轮廓与填充，并导出切片文件。

## 功能

- **模型管理**：一次导入多个 STL，支持选择、隐藏、重命名，显示包围盒和网格信息
- **场景编辑**：平移 / 旋转 / 缩放参数面板与视口 gizmo 联动，复制、矩形阵列、贴合基板，操作可撤销 / 回撤
- **平台与视角**：可调平台尺寸，透视 / 正交投影切换，视角立方体和场景设置
- **切片预览**：基于 PySLM C++ 计算轮廓和扫描填充，逐层查看，可分别显隐轮廓、填充、方向箭头和跳转，并统计路径长度
- **工程文件**：`.slmproj` 单文件工程，内嵌 STL 数据，附带最近工程记录
- **切片导出**：`.slmslice` 内部容器（逐层 SHA-256）、CLI 2.0 ASCII、SVG/PNG 联系人表、CSV 逐层统计；导出后自动回读校验层数、轮廓/填充数与路径长度

## 技术栈

- C++20 + CMake
- Qt Widgets（Qt 5 / 6）：主窗口、菜单、工具栏、模型列表和停靠面板
- GLFW + OpenGL 3.3 + Dear ImGui + ImGuizmo：中央 3D 视口和变换 gizmo
- Assimp：模型导入；GLM：数学计算
- PySLM C++：切片轮廓与填充计算

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `openSLMSlicer/` | 程序源码，包括窗口、视口、切片引擎、导出与工程读写 |
| `openSLMSlicer/tests/` | 工程、界面流程、切片和导出测试 |
| `docs/` | `.slmslice` 规格、SVG/PNG/CSV 报告格式与切片输出设计文档 |
| `asset/` | 示例 STL、着色器、图标等资源 |
| `slmproj/` | 示例工程和切片文件 |
| `3rd_party/` | GLFW、Dear ImGui、GLAD、GLM、Assimp、ImGuizmo、PySLM C++ 等依赖 |

## 切片输出

切片预览有效时，可通过“切片 → 导出切片文件…”（或预览面板的“导出切片…”）导出当前快照，支持五种格式：

| 格式 | 用途 | 说明 |
| --- | --- | --- |
| `.slmslice` | 内部无损容器 | 保存逐层几何、空层、模型归属、内外轮廓和填充线，逐层 SHA-256；可重新打开预览或另存为其他格式 |
| `.cli` | 通用几何交换 | CLI 2.0 ASCII，只含层几何，可给支持 CLI 的第三方软件导入 |
| `.svg` | 预览/报告 | 联系人表：每个非空层一格，含层号与 Z 标签，矢量可缩放 |
| `.png` | 预览/报告 | 与 SVG 同版式的位图联系人表，直接编码生成，不依赖 Qt 图片插件 |
| `.csv` | 统计/CI | 每层一行：层号、Z、空层标记、轮廓/填充数量与路径长度 |

导出只读取当前预览快照，不会重新切片，也不修改工程；写入使用临时文件原子替换，支持取消。勾选“导出后校验”会回读文件，校验层数、轮廓数、填充数和路径长度（CLI 因坐标按 4 位小数写出，只校验数量）。

需要说明的是：CLI 输出只包含几何，SVG/PNG/CSV 是报告/统计产物，它们都不包含激光功率、扫描速度等工艺参数，不能直接用于设备加工；面向设备的 OVF、3MF 尚未实现。各报告格式的字段和校验规则见 [docs/slice-report-formats.md](docs/slice-report-formats.md)。

## 文档

- [docs/slmslice-v1.md](docs/slmslice-v1.md)：`.slmslice` 文件布局、字段和版本兼容规则
- [docs/slice-report-formats.md](docs/slice-report-formats.md)：SVG/PNG/CSV 报告格式、字段与校验规则
- [docs/slice-output-design.md](docs/slice-output-design.md)：切片输出格式的设计与演进计划

## 说明

项目目前面向 Windows x64，随仓库提供的 Assimp 和 PySLM C++ 均为 Windows x64 预编译库。
