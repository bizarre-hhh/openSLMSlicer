# .slmslice v1 格式规格

> 适用：openSLMSlicer M1（应用版本见 `main.cpp` 的 `setApplicationVersion`）
> 实现：`openSLMSlicer/slmslice_format.cpp`（`WriteSlmSliceV1` / `ReadSlmSliceV1` / `WriteLayerBlob` / `ReadLayerBlob`），公开接口见 `openSLMSlicer/slice_export.h`
> 设计背景：`docs/slice-output-design.md` 第 6 节

## 1. 定位与边界

`.slmslice` 是 openSLMSlicer 的**内部无损切片容器**，只解决本程序自己的问题：

- 保存一次切片预览的完整几何快照（逐层浮点坐标、空层、模型归属、内外轮廓、填充线）；
- 作为回归测试的基线文件，便于逐字节/逐字段比对；
- 作为将来转换到 OVF、3MF Toolpath 等格式的中间层。

它**不是对外交换标准**，不要求其他软件支持，也不承诺任何设备能直接读取。

M1 的实际范围：

- **入口**：在切片预览有效时使用“切片 → 导出切片文件…”（或预览面板的“导出切片…”），格式可选 `.slmslice` 或 CLI；见 README 的“切片预览”章节。
- **writer + reader**：导出时写文件，随后可回读并校验层数、轮廓数、填充数和逐层 SHA-256；测试也直接使用 reader。
- **打开与查看**：可用“切片 → 打开切片文件…”把 `.slmslice` 载入切片预览（不需要 STL 模型）；空层保留在文件中，但预览滑块只显示有几何的层。“切片 → 切片信息…”显示 header JSON、part 列表、层数/空层/轮廓/填充统计和每层 SHA-256。
- **可另存为 CLI**：打开 `.slmslice` 后仍可通过导出功能选择 CLI，把当前快照写成 `.cli`；没有独立的批量转换命令。
- **对外格式是 CLI 与后续的 OVF/3MF**：M1 实现 CLI 2.0 ASCII（仅逐层几何，见第 8 节）；OVF、3MF Toolpath 属于后续阶段，当前未实现，不能声称可上机或已被设备验证。

## 2. 文件布局

`.slmslice` 是单文件、无压缩、无对齐填充的二进制容器，整体布局如下（所有多字节整数和浮点数均为 **little-endian**）：

| 偏移（字节） | 长度（字节） | 内容 |
| --- | --- | --- |
| 0 | 8 | magic：ASCII 字节 `SLMSLICE`（`53 4C 4D 53 4C 49 43 45`） |
| 8 | 4 | `uint32 version`，v1 固定为 `1` |
| 12 | 4 | `uint32 header_bytes`，JSON 头的字节长度 |
| 16 | `header_bytes` | UTF-8 JSON 对象（header，见第 3 节） |
| `16 + header_bytes` | `layers.byte_length` | 逐层二进制 payload（各层 blob 依次串联，见第 4 节） |

文件必须满足：

```text
文件大小 == 16 + header_bytes + layers.byte_length
```

- payload 之间**没有分隔符、没有对齐填充、没有压缩**；层边界只由 header 中 `layers.checksums` 里每层的 `bytes` 决定。
- 不支持 ZIP/容器封装，也没有缩略图或附加资源块。
- 读取端从 `16 + header_bytes` 顺序读取每层 blob；偏移计算错误或末尾有多余数据都会被 M1 reader 拒绝。

## 3. JSON 头（header）

### 3.1 编码与总体约定

- header 是 UTF-8 编码的 JSON 对象，`header_bytes` 计的是**字节数**（不是字符数）。
- 写入端输出带缩进的美化 JSON；字段顺序与空白对读取方无意义，但必须是合法 JSON 对象。
- 整数（`int64`/`uint64`）用 JSON number 表示。JSON 的精度上限意味着超过 2^53 的整数在通用 JSON 解析器中会丢失精度；M1 写入的 id、层号、字节数等远小于 2^53，不受影响。
- 除 `format`、`version`、`layers.checksums` 等语义必需的字段外，读取方应忽略不认识的 JSON 字段；向 header 增加可选字段不需要改变文件版本（见第 7 节）。

### 3.2 顶层字段

| 字段 | 类型 | M1 写入值 / 说明 |
| --- | --- | --- |
| `format` | string | 固定为 `"slmslice"`（小写；与 magic 的 `SLMSLICE` 不同） |
| `version` | number | 固定为 `1`，必须与文件头部的 `uint32 version` 一致 |
| `app` | object | 写入程序信息，见 3.3 |
| `created_utc` | string | 创建时间，ISO 8601 UTC，秒级，例如 `"2026-10-27T04:16:51Z"` |
| `coordinate_system` | object | 坐标约定，见 3.4；**语义声明，不能被读取方猜测** |
| `plate_mm` | number[3] | 平台尺寸 `[X, Y, Z]`，单位 mm；来自切片时的平台设置 |
| `slice` | object | 切片参数与实际写入范围，见 3.5 |
| `parts` | array | 参与切片的模型元数据，见 3.6；可为空数组 |
| `source_project_revision` | number | 切片生成时对应的工程修订号（内部单调计数器），仅用于追溯，不参与几何校验 |
| `layers` | object | payload 的编码声明、长度与逐层校验和，见 3.7 |

### 3.3 `app`

| 字段 | 类型 | M1 写入值 / 说明 |
| --- | --- | --- |
| `app.name` | string | 固定为 `"openSLMSlicer"` |
| `app.version` | string | `QCoreApplication::applicationVersion()` 的字符串（M1 为 `0.1.0`） |

### 3.4 `coordinate_system`

| 字段 | 类型 | M1 写入值 | 说明 |
| --- | --- | --- | --- |
| `unit` | string | `"mm"` | 坐标单位固定为毫米 |
| `origin` | string | `"plate_front_left_bottom"` | 坐标原点：基板前左下方；X/Y 沿基板两边，Z 垂直向上（右手系） |
| `up` | string | `"+Z"` | Z 轴向上 |
| `z_reference` | string | `"layer_midplane"` | 层块中的 `z_mm` 是层中面 |

### 3.5 `slice`

| 字段 | 类型 | M1 写入值 / 说明 |
| --- | --- | --- |
| `layer_thickness_mm` | number | 切片层厚（mm），与坐标、Z 值配套使用 |
| `z_reference` | string | `"layer_midplane"`，与 `coordinate_system.z_reference` 相同（重复声明便于局部读取） |
| `first_grid_index` | number | 实际写入的第一层的全局网格层号 |
| `last_grid_index` | number | 实际写入的最后一层的全局网格层号 |
| `layer_count` | number | 实际写入的层数，等于 `layers.checksums` 的长度 |
| `include_empty_layers` | boolean | 导出时是否保留空层；写入端按实际选项填写 |
| `settings.enabled` | boolean | 切片时是否启用填充 |
| `settings.spacing_mm` | number | 扫描间距（mm） |
| `settings.angle_deg` | number | 填充角度（度） |
| `settings.layer_angle_increment_deg` | number | 层间角度增量（度/层） |
| `settings.spot_compensation_mm` | number | 光斑补偿（mm） |

说明：

- `settings` 记录的是**生成这批填充线时使用的参数**，不代表文件里一定含有填充记录：若导出时关闭“包含填充”，各层 `hatch_count` 为 0，但 `settings.enabled` 仍可能为 `true`。v1 没有单独的 `include_hatches` 字段。
- `first_grid_index` / `last_grid_index` / `layer_count` 描述**过滤空层之后实际写入**的层范围；`layers.checksums` 才是权威索引。M1 reader 不校验这几个汇总字段与 blob 是否一致，写入端保证一致。

### 3.6 `parts`

按切片时的模型列表顺序写入；层块中的 `model_id` 引用 `parts[].id`。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `parts[].id` | number | 模型 id（`uint64` 语义），对应 `model_id` |
| `parts[].name` | string | 模型名称，UTF-8 |
| `parts[].source_path` | string | 来源 STL 路径（仅追溯用，可能是空字符串） |
| `parts[].triangle_count` | number | 模型三角面数 |

v1 不保存 part 的变换矩阵、网格哈希、材料或工艺参数：坐标已经是应用变换后的**平台绝对坐标**。

### 3.7 `layers`

| 字段 | 类型 | M1 写入值 / 说明 |
| --- | --- | --- |
| `layers.encoding` | string | 固定为 `"binary-le"` |
| `layers.point_type` | string | 固定为 `"float64"`（IEEE 754 binary64） |
| `layers.layer_header` | string | 供人阅读的层块头字段说明：`grid_index:int64,z_mm:float64,flags:uint32,ring_count:uint32,hatch_count:uint32`；**读取方不得解析它**，字段布局以本规格第 4 节为准 |
| `layers.byte_length` | number | 所有层块字节数之和；必须满足第 2 节的文件长度等式 |
| `layers.checksums` | array | 逐层索引与校验和，顺序与 payload 中 blob 的顺序一致 |

`layers.checksums[i]` 的字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `grid_index` | number | 该层的全局网格层号；仅便于定位，回读以 blob 内的 `grid_index` 为准 |
| `bytes` | number | 该层 blob 的字节长度，必须 > 0 |
| `sha256` | string | 该层 blob 原始字节的 SHA-256，**小写十六进制（64 字符）**；读取时大小写不敏感 |

层块按 `grid_index` 升序排列；省略空层时相邻层的层号可以跳号，但不得重复。校验和用于检测位翻转/截断，**不是签名**，不能防篡改。

### 3.8 header 示例

```json
{
  "format": "slmslice",
  "version": 1,
  "app": { "name": "openSLMSlicer", "version": "0.1.0" },
  "created_utc": "2026-10-27T04:16:51Z",
  "coordinate_system": {
    "unit": "mm",
    "origin": "plate_front_left_bottom",
    "up": "+Z",
    "z_reference": "layer_midplane"
  },
  "plate_mm": [100.0, 100.0, 100.0],
  "slice": {
    "layer_thickness_mm": 0.03,
    "z_reference": "layer_midplane",
    "first_grid_index": 0,
    "last_grid_index": 0,
    "layer_count": 1,
    "include_empty_layers": true,
    "settings": {
      "enabled": true,
      "spacing_mm": 0.08,
      "angle_deg": 45.0,
      "layer_angle_increment_deg": 67.0,
      "spot_compensation_mm": 0.08
    }
  },
  "parts": [
    {
      "id": 1,
      "name": "cube",
      "source_path": "D:/models/cube.stl",
      "triangle_count": 12
    }
  ],
  "source_project_revision": 7,
  "layers": {
    "encoding": "binary-le",
    "point_type": "float64",
    "layer_header": "grid_index:int64,z_mm:float64,flags:uint32,ring_count:uint32,hatch_count:uint32",
    "byte_length": 1234,
    "checksums": [
      { "grid_index": 0, "bytes": 1234, "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" }
    ]
  }
}
```

## 4. 逐层二进制 payload

每层一个连续 blob，结构固定为“层头 + 轮廓记录数组 + 填充记录数组”。所有整数与浮点数均为 **little-endian**，`float64` 为 **IEEE 754 binary64**；字段之间没有 padding，不依赖 C 结构体内存布局。

### 4.1 层头（28 字节）

| 偏移 | 长度 | 类型 | 字段 | 说明 |
| --- | --- | --- | --- | --- |
| 0 | 8 | `int64` | `grid_index` | 全局 Z 网格层号；`z_mm = (grid_index + 0.5) × layer_thickness_mm` |
| 8 | 8 | `float64` | `z_mm` | 层中面 Z（mm），显式保存，避免读取方自行推算 |
| 16 | 4 | `uint32` | `flags` | 位 0（`0x1`）= 空层；其余位 v1 保留，写入 0，读取方忽略 |
| 20 | 4 | `uint32` | `ring_count` | 后面的轮廓记录条数 |
| 24 | 4 | `uint32` | `hatch_count` | 后面的填充记录条数 |

层头之后紧跟 `ring_count` 条轮廓记录，再紧跟 `hatch_count` 条填充记录。

### 4.2 轮廓记录

| 偏移 | 长度 | 类型 | 字段 | 说明 |
| --- | --- | --- | --- | --- |
| 0 | 8 | `uint64` | `model_id` | 所属模型，对应 `header.parts[].id`；0 表示未知 |
| 8 | 1 | `uint8` | `role` | 环角色：`0` = 外轮廓，`1` = 内轮廓（孔） |
| 9 | 1 | `uint8` | `closed` | 是否闭合：`0`/`1` |
| 10 | 2 | `uint16` | `reserved` | v1 必须写 0，读取方忽略 |
| 12 | 4 | `uint32` | `point_count` | 点数，v1 要求 ≥ 2 |
| 16 | `16 × point_count` 字节 | `float64` × 2 | `x`, `y` | 按点序依次写 `x0,y0,x1,y1,…`，单位 mm |

单条轮廓记录占 `16 + 16 × point_count` 字节。

### 4.3 填充记录

| 偏移 | 长度 | 类型 | 字段 | 说明 |
| --- | --- | --- | --- | --- |
| 0 | 8 | `uint64` | `model_id` | 所属模型，对应 `header.parts[].id`；0 表示未知 |
| 8 | 8 | `float64` | `x1` | 起点 X |
| 16 | 8 | `float64` | `y1` | 起点 Y |
| 24 | 8 | `float64` | `x2` | 终点 X |
| 32 | 8 | `float64` | `y2` | 终点 Y |

单条填充记录固定 40 字节，表示一条直线段（起终点，mm）。填充线方向/顺序按切片内核输出原样保存，没有额外的扫描顺序或开关光信息。

### 4.4 空层

- 空层写 `flags = 1`、`ring_count = 0`、`hatch_count = 0`，blob 固定 28 字节；`grid_index` 和 `z_mm` 仍然有效。
- 空层用于保留“首末非空层之间”的完整 Z 网格（`BuildSliceBuildSnapshot` 补齐），便于按层号对齐和设备后处理。
- 关闭“包含空层”导出选项时不写空层，层号会跳号，`slice.include_empty_layers` 为 `false`。
- 非空层 `flags = 0`；M1 写入端保证 `empty == true` 时两个计数都为 0。读取方应以 `ring_count`/`hatch_count` 解析内容、以 `flags` 位 0 判断空层语义。

### 4.5 点序与闭合

- 点列按记录中的顺序连接；记录里**不存放**用于闭合的隐式标记之外的额外点。
- `closed = 1` 表示环闭合：若最后一个点与第一个点不重合，语义上仍由最后一段连接回起点。M1 切片内核的输出本身已经**在末尾重复首点**，所以 M1 写出的闭合轮廓通常满足 `首点 == 末点`；reader 按原样保留点列。
- `role` 只表达外/内轮廓语义，v1 不重新计算或重排点的绕向。CLI writer 会把 `role` 映射为方向字段（见第 8 节），但不改变点序。
- v1 reader 拒绝 `point_count < 2` 或超过 10,000,000 的轮廓。

### 4.6 尺寸计算

对一层：

```text
blob_bytes = 28
           + Σ_rings (16 + 16 × point_count_i)
           + hatch_count × 40
```

所有层 blob 之和必须等于 `layers.byte_length`。由于是无压缩原始数据，文件体积可以直接由几何量级估算（每点 16 字节、每填充线 40 字节）。

### 4.7 每层 SHA-256

`layers.checksums[i].sha256` 是第 `i` 层 **blob 原始字节**（从层头第一个字节到该层最后一条填充记录的最后一个字节）的 SHA-256：

- 输入不含前一层的字节、不含 header、不含任何长度前缀；
- 输出为小写十六进制字符串；
- M1 reader 逐层重算并比对（大小写不敏感），不一致直接报错。

## 5. 坐标、单位与 Z 约定

- **单位**：mm；所有坐标与长度都是毫米，没有 µm 整型化，没有量化。
- **坐标系**：原点 `plate_front_left_bottom`（基板前左下方），X/Y 沿基板两边，Z 向上，右手系。`plate_mm` 给出平台 X/Y/Z 尺寸。
- **part 归属**：坐标是平台绝对坐标；part 的平移/旋转/缩放在切片前已应用，文件里不再保存变换。
- **Z 参考面**：`z_reference = layer_midplane`，层块里的 `z_mm` 是层中面：

```text
z_mm = (grid_index + 0.5) × layer_thickness_mm
```

例如层厚 0.03 mm 时，`grid_index = 0` 对应 `z = 0.015` mm。

- **层号语义**：`grid_index` 是全局 Z 网格序号（从平台 Z=0 起算），不是“非空层序号”；后者不写入文件，M1 reader 在回读时按 payload 顺序重算为 1-based 的 `index`。
- **不猜约定**：v1 把单位、原点方向、Z 参考面都显式写进 header；任何工具都不得根据文件缺失字段或层号自行猜测 Z 参考面和单位。语义变化必须升级版本（第 7 节）。

## 6. 校验规则（M1 reader）

`ReadSlmSliceV1()` / `ReadSlmSliceFile()` 按以下顺序校验，任一步失败都返回 `false` 并给出错误文本，目标快照保持不变：

1. 文件可读、长度 ≥ 16；
2. 前 8 字节 magic 完全等于 `SLMSLICE`；
3. `version == 1`；`header_bytes ≤ 16 MiB`；文件长度 ≥ `16 + header_bytes`；
4. header 是合法 JSON 对象，`format == "slmslice"` 且 `header.version == 1`；
5. `layers.checksums` 非空且不超过 200,000 项；
6. `16 + header_bytes + layers.byte_length == 文件大小`；
7. 逐层：`bytes > 0` 且不越过文件末尾；读取 blob 后重算 SHA-256 并与 `sha256` 比对（大小写不敏感）；
8. 逐层解析 blob：字段读取完整；`ring_count ≤ 4,000,000`、`hatch_count ≤ 4,000,000`、`2 ≤ point_count ≤ 10,000,000`；解析结束后必须恰好到达 blob 末尾（有多余字节即判为“长度与内容不一致”）；
9. 所有层读完后偏移必须恰好等于文件大小，末尾不得有多余数据。

M1 reader 的硬限制（写入端受切片内核的层数/填充段上限约束，不额外做完整预检）：header ≤ 16 MiB、层数 ≤ 200,000、每层轮廓 ≤ 4,000,000、每层填充 ≤ 4,000,000、单轮廓点 ≤ 10,000,000。

M1 reader **不做跨字段语义交叉校验**：不检查层块 `model_id` 是否存在于 `parts[]`、不检查 `checksums[].grid_index` 是否等于 blob 内的 `grid_index`、也不检查 `slice.first_grid_index` / `last_grid_index` / `layer_count` 与 blob 是否一致；`coordinate_system` 只按 `version == 1` 的既定语义信任，不解析其字符串内容。上述一致性由写入端保证，需要额外核对时可使用测试或独立解析工具。

导出流程中勾选“导出后校验”时会调用 `SummarizeSliceFile()` 回读文件，并与快照比对层数、轮廓数、填充数；`.slmslice` 的回读本身还会校验逐层 SHA-256。CLI 没有校验和，CLI 回读只解析结构并统计数量。

## 7. 版本与兼容规则

1. **版本号只有一个** `uint32`（文件头部与 `header.version` 各写一次，必须一致）。v1 读取方只接受 `1`；其他版本直接拒绝，不做“尽力解析”。
2. **版本只增不减**，不实现向后写出旧版本。新版本文件不能被 v1 reader 打开，这是预期行为。
3. **header 可以增加可选字段**：旧 reader 忽略不认识的 JSON 成员，仍能正确读取核心数据。不能删除、改名或改变同版本内已有字段的类型/语义。
4. **payload 布局是版本锁定的一部分**：层块/轮廓/填充的字段顺序、类型、字节数固定，且 reader 要求每层 blob 被精确消费。因此**在 v1 内追加 payload 字段一定会破坏 v1 reader**；任何 payload 变更都必须递增版本号。
5. **语义变化必须升版本**：单位、坐标原点、Z 参考面（中面/顶面/底面）、层号基准、`flags` 位含义等任何改变解释结果的语义，都必须递增版本并在 header 显式声明；v1 只有一个整数版本号，可把它理解为 major——语义变化就升 major，不允许用“猜”来兼容。
6. 同一版本内写入端必须写全本规格要求的字段（至少 `format`、`version`、`layers.checksums` 及其长度/校验信息）；缺失必需字段视为损坏文件。
7. M1 不提供跨版本自动转换；将来若需要，由新工具显式转换，而不是让旧 reader 猜测。
8. **重命名兼容**：reader 同时接受项目重命名前的 magic `GLSLICE\0` 和 `format: "glslice"` 文件，写入端只写新标识；旧文件可打开后重新导出完成迁移。

## 8. 与 CLI 2.0 ASCII 导出的对照

`.slmslice` 是内部无损格式；对外部软件交换时 M1 使用 CLI 2.0 ASCII。两者由同一份快照导出，但语义与能力不同：

| 项目 | `.slmslice` v1 | CLI 2.0 ASCII（M1） |
| --- | --- | --- |
| 层平面 | 层中面 `z_mm = (grid_index + 0.5) × t`（header 显式声明） | 层顶面：`$$LAYER/z` 写 `z_mm + t/2 = (grid_index + 1) × t` |
| 空层 | 可选保留（默认保留），写 28 字节空层 blob | 始终跳过 |
| part 归属 | 每个轮廓/填充带 `model_id`，header 有 `parts[]` | 无；所有 `$$POLYLINE` / `$$HATCHES` 的 id 固定为 0，不写 `$$LABEL` |
| 内外轮廓 | `role`：0 外 / 1 内 | `dir`：外轮廓 1（CCW）、内轮廓 0（CW）；开放线 2（M1 切片数据为闭合环，不产生 2） |
| 坐标精度 | `float64` 原始值，无量化 | 文本固定 4 位小数（0.0001 mm）；非有限值写 0 |
| 填充 | 每层 `hatch_count` 条 40 字节线段记录 | 每层一条 `$$HATCHES/0,n,...`（启用填充且非空时） |
| 校验 | 每层 blob 的 SHA-256 | 无校验和；靠回读解析校验结构 |
| 工艺参数 | 无（只有切片时的填充设置） | 无；不含功率、速度、扫描顺序、开关光、激光分配 |

CLI 的通用边界（详见 `docs/slice-output-design.md` 4.1 与 README）：

- CLI 是**交换格式，不是设备控制格式**；M1 导出的是“几何集合”，不能直接上机，也没有经过任何设备/厂商软件验证。
- CLI 文件是 ASCII 文本；层块头 `$$LAYER/z` 的 Z 由 `.slmslice` 的层中面换算为层顶面，这是两种格式之间唯一的 Z 语义转换，换算只做一次。
- 当前写入的 header 顺序为 `$$HEADERSTART`、`$$ASCII`、`$$UNITS/1.0`、`$$VERSION/200`、`$$DATE/ddMMyy`、`$$LAYERS/非空层数`、`$$HEADEREND`、`$$GEOMETRYSTART`、逐层块、`$$GEOMETRYEND`。`$$UNITS/1.0` 表示坐标单位为 mm；`$$DATE` 为本地日期。

## 9. M1 限制与后续

M1 的限制（不要对外承诺超出这些范围的能力）：

- **不做场景导入/几何编辑**：提供 writer、回读校验 reader、UI 打开预览和信息对话框；不把 `.slmslice` 作为可编辑场景导入，也没有独立的批量格式转换器。打开后可通过正常导出流程另存为 CLI。
- **不是设备作业**：没有扫描顺序、跳转、开关光、延时、激光功率/速度、多激光分配、扫描场、machine/process profile、材料与操作者信息。
- **无损有前提**：在包含空层、包含填充并开启回读校验时，几何、空层、归属和精度是无损的；关闭“包含填充”会丢弃填充记录，且 v1 不记录该选项（只能由 `hatch_count` 全为 0 推断）。`include_empty_layers` 则记录在 header 中。
- **无压缩**：大作业的 `.slmslice` 可能达到数十到数百 MB；读取时按层顺序逐个处理，但 M1 writer 仍会在临时文件中暂存完整 payload 后再原子替换目标文件。
- **无缩略图/预览块**，文件管理器不能直接预览。
- OVF、3MF Toolpath 等面向设备的格式属于后续阶段，当前未实现；本格式也不承担“机器适配”职责。

后续演进建议：

- 需要新增 payload 字段（扫描顺序、激光参数、part 变换、网格哈希等）时，递增版本号并保留 v1 reader；
- 需要对外分发/可解压查看时，再评估 ZIP + manifest 的封装方案，而不是修改 v1 布局；
- 需要批量 `.slmslice → CLI/OVF` 转换、场景导入或编辑几何时，单独设计 UI/命令行与转换语义，不改变本规格的 v1 定义。

## 10. 参考

- 设计背景与选型：`docs/slice-output-design.md`
- 写入/读取实现：`openSLMSlicer/slmslice_format.cpp`、`openSLMSlicer/slmslice_format.h`
- 导出接口与选项：`openSLMSlicer/slice_export.h`、`openSLMSlicer/slice_export.cpp`
- 信息读取：`ReadSlmSliceFileInfo()`（`openSLMSlicer/slice_export.cpp` / `slmslice_format.cpp`）
- CLI 写入/解析：`openSLMSlicer/cli_format.cpp`、`openSLMSlicer/cli_format.h`
- 回归测试：`openSLMSlicer/tests/slice_export_tests.cpp`（roundtrip、空层、Unicode 名称、校验和损坏、取消保留旧文件）
- CLI 2.0 规范原文：<https://www.hmilch.net/downloads/cli_format.html>
