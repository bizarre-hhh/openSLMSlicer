# SVG / PNG / CSV 切片报告格式（M1）

> 适用：openSLMSlicer M1 切片导出
> 实现：`openSLMSlicer/slice_report_export.cpp`、`openSLMSlicer/slice_export.cpp`
> 定位：这三种是**预览、报告、统计**产物，不是设备作业格式；都不携带激光功率、扫描速度、扫描顺序、开关光或多激光信息。

## 1. 共同约定

- 导出只序列化**当前预览快照**（`SliceBuildSnapshot`），不会重新切片，也不读取导出时的 UI 参数。
- 写文件使用 `QSaveFile` 临时文件 + 原子替换；取消或失败时保持旧文件不变。
- 三种格式都**不写入时间戳**，相同快照重复导出得到完全相同的字节（可用于回归/CI；`.slmslice` 头里的 `created_utc` 和 CLI 头里的 `$$DATE` 是例外）。
- 数据来自切片快照：坐标单位 mm，Z 为层中面 `z_mm = (grid_index + 0.5) × 层厚`。
- 选择“导出后校验”时，导出器会回读文件并对照快照校验：层数、轮廓数、填充数、路径长度（CLI 只校验数量，因其坐标按 4 位小数写出）。

## 2. 联系人表版式（SVG / PNG 共用）

- 只绘制**非空层**（`empty=true` 的 Z 网格层跳过），按快照顺序（`grid_index` 升序）**逐行**排列。
- 单元格数量 = 非空层数量；列数取 `ceil(sqrt(层数))` 并限制在 1～8 列，行数随之确定。
- 单元格尺寸随层数自适应：层数越多，格子越小；同时保证图像任一边不超过 30000 px、总像素不超过约 1600 万，避免超大位图和 OOM。
- 每个格子把该层几何按包围盒等比缩放并居中；轮廓先画、填充后画（实际顺序：先填充，再外轮廓，最后内轮廓），内轮廓覆盖外轮廓。
- SVG 在格子左上角写 `L<grid_index> z=<mm>` 标签；格子太小时（边长 < 32 px）省略标签。PNG 不写文字，靠元数据 `columns/rows/cell` 和行优先顺序定位。
- 颜色：外轮廓 `#153e75`（PNG 调色板索引 2），内轮廓 `#b02020`（索引 3），填充 `#9a9a9a`（索引 1），背景白色，格子边框 `#d8d8d8`（索引 4）。
- 联系人表是**缩略总览**，不表示真实尺寸比例；需要真实几何请用 `.slmslice` / CLI。

## 3. SVG

- 标准 SVG 1.1 文档，根元素带 `width` / `height` / `viewBox`；文本为 UTF-8，标签使用 `Segoe UI, Arial, sans-serif`。
- 结构：

```xml
<svg ...>
  <title>openSLMSlicer slice report</title>
  <desc>version=1;format=svg;layers=2;contours=3;hatches=3;...</desc>
  <rect .../>            <!-- 背景 -->
  <g id="layers">
    <g class="layer" data-grid-index="0" data-z-mm="0.015" transform="translate(0,0)">
      <rect class="cell" .../>
      <g class="hatches"><line class="hatch" .../></g>
      <g class="contours">
        <polyline class="contour outer" stroke="#153e75" ... points="..."/>
        <polyline class="contour inner" stroke="#b02020" ... points="..."/>
      </g>
      <text ...>L0 z=0.015</text>
    </g>
    ...
  </g>
</svg>
```

- `<desc>` 是机器可读的元数据，形如 `key=value;key=value`，至少包含：
  `version`、`format=svg`、`layers`、`contours`、`hatches`、`contour_length_mm`、`hatch_length_mm`、`columns`、`rows`、`cell`、`width`、`height`、`first_grid`、`last_grid`、`include_hatches`。
- 回读校验（`SummarizeSvgReport`）：
  1. 文档包含 `<svg>`，且有 `<desc>` 元数据；
  2. `format` 必须为 `svg`，必需字段齐全；
  3. 扫描全文统计 `<g class="layer"`、`class="contour outer"`、`class="contour inner"`、`class="hatch"` 的出现次数，必须分别与元数据的层数、外/内轮廓总数、填充数一致。
- 因此手工删改几何或元数据会校验失败；新增可选字段不影响旧读取方。

## 4. PNG

- 直接由导出器编码的调色板 PNG（color type 3、8 bit），**不依赖 Qt 的 qpng 图片插件**，在只有 QtCore 的环境也能生成。
- 关键数据块：`IHDR`（尺寸/8 bit/调色板）、`PLTE`（5 色）、`tEXt`（关键字 `slmslicer`）、`IDAT`（zlib 压缩的扫描行）、`IEND`；每个块带 CRC-32。
- `tEXt` 文本与 SVG `<desc>` 同格式，额外包含 `format=png`、`width`、`height`、`raw_bytes`；`raw_bytes = height × (width + 1)`，用于回读时校验解压后的扫描行长度。
- 几何栅格化：按格子局部坐标做包围盒缩放，逐段用 Bresenham 画线；轮廓宽度固定 1 px（缩略图不追求线宽精确）。
- 回读校验（`SummarizePngReport`）：
  1. PNG 签名、逐块 CRC-32；
  2. `IHDR` 必须是 8 bit 调色板，`PLTE`/`IDAT`/`IEND` 齐全；
  3. `tEXt` 中 `slmslicer` 元数据完整，且宽高与 IHDR 一致；
  4. `raw_bytes` 与尺寸一致，IDAT 拼接后能解压出恰好 `raw_bytes` 字节，且每行过滤字节为 0。
- 限制：为了控制内存，联系人表尺寸受 30000 px 边长、约 1600 万像素约束；层数特别多时格子会很小。定位具体层请配合 SVG 标签或 CSV。
- 已用 Pillow 独立解码验证过导出文件（调色板模式、尺寸、像素索引均正确）。

## 5. CSV

- 纯文本 UTF-8，无 BOM、无注释行，`\n` 换行；表头固定为：

```text
grid_index,z_mm,empty,contours,hatches,contour_length_mm,hatch_length_mm,total_length_mm
```

- 每行一层，按 `grid_index` 升序：
  - `grid_index`：全局 Z 网格层号（从 0 开始）；
  - `z_mm`：层中面 Z，6 位小数；
  - `empty`：`1` = 空层，`0` = 非空层；
  - `contours` / `hatches`：该层轮廓/填充数量（未勾选“包含填充”时 `hatches` 为 0）；
  - `contour_length_mm` / `hatch_length_mm` / `total_length_mm`：由几何点列计算的长度，6 位小数；未包含填充时 `hatch_length_mm` 为 0。
- 空层行为由导出选项决定：勾选“包含空层”（默认）时写入空层行（例如 `1,0.045000,1,0,0,0.000000,0.000000,0.000000`），否则只写非空层，层号可跳号。
- 回读校验（`SummarizeCsvReport`）：表头必须精确匹配；每行必须 8 列且数值合法（`empty` 只能是 0/1）；行数即层数，数量/长度按行求和。
- 适合放进 Git 做 golden diff、在 Excel/ pandas 里统计，或作为仿真输入的逐层统计表。

## 6. 导出选项与限制

| 选项 | `.slmslice` | CLI | SVG | PNG | CSV |
| --- | --- | --- | --- | --- | --- |
| 包含空层 | 可选（默认是） | 始终跳过 | 始终跳过 | 始终跳过 | 可选（默认是） |
| 包含填充 | 可选 | 可选 | 可选 | 可选 | 可选（影响数量与长度） |
| 回读校验 | 层数/轮廓/填充/长度 + 逐层 SHA-256 | 层数/轮廓/填充（不比长度） | 元数据 + 元素计数 | CRC + zlib + 元数据 | 表头/行解析 + 合计 |
| 确定性 | 否（`created_utc`） | 否（`$$DATE`，同一天内一致） | 是 | 是 | 是 |

- 三种报告格式**不是设备作业**：没有扫描顺序、跳转、开关光、延时、功率/速度、多激光或扫描场；面向设备的 OVF / 3MF Toolpath 仍属于 M2/M3。
- 改变 SVG/PNG/CSV 的字段或语义时应同步更新本文件和 `slice_export_tests.cpp` 的用例；`<desc>` / `tEXt` 元数据是读取方依赖的稳定接口。

## 7. 独立回读验收脚本

`openSLMSlicer/tests/verify_exports.py` 是不复用项目代码的独立解析器（Python 标准库；Pillow 可选），用于验收五种导出：

```text
set SLMSLICER_EXPORT_TEST_ARTIFACTS=D:\temp\slice-artifacts
slice_export_tests.exe
python openSLMSlicer/tests/verify_exports.py D:\temp\slice-artifacts
```

- 设置 `SLMSLICER_EXPORT_TEST_ARTIFACTS` 后，`slice_export_tests` 会在该目录写出 `sample.slmslice/.cli/.svg/.png/.csv` 各一份；
- 脚本独立解析：`.slmslice` 的 magic/JSON/逐层 SHA-256/blob 布局，CLI 的头部/折线/填充与顶面 Z，SVG 的 `<desc>` 与元素计数，PNG 的分块 CRC/zlib/扫描行（有 Pillow 时再解码一次），CSV 的表头与合计；
- 任何一项不符都会以非零退出并打印失败项。该脚本只依赖 Python 3，适合作为发布前的人工验收步骤。

## 8. 与设备作业格式的边界

这三种报告格式只解决"看"和"对账"：没有扫描顺序、跳转、开关光、延时、功率/速度、多激光或扫描场。面向开放设备/研究机的 OVF 和 3MF Toolpath 仍属于后续阶段（见 `docs/slice-output-design.md` 的 M2/M3）。

