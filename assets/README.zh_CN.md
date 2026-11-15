<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐、电池参数等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### 掌小读阅读器字库（`fonts/lv_font_reader_12.c` … `_20.c`）

九档 2bpp PLAIN 位图字体，共 3987 个码位（ASCII + GB2312 一级汉字 + 标点/UI 符号）。
**所有九档汉字都来自点阵**，仅 ASCII/标点/UI 符号用 Noto；**推进宽度一律取自 Noto**，分页与书签按字节偏移计算，所以切换字号不会改变分页、书签偏移和布局：

| 范围 | 来源 | 许可 |
| --- | --- | --- |
| 12–15 px 汉字（冻结） | 文泉驿点阵宋体 **原生点阵**（12×12 / 13×13 / 14×14；15 px 直接复用 14×14，不缩放、不加权） | GPL-2-with-font-embedding-exception，全文见 [`fonts/LICENSE.wqy-bitmapfont.txt`](fonts/LICENSE.wqy-bitmapfont.txt) |
| 16 px 汉字 | 原生 16×16 点阵 + **内侧拐角补全**（墨量 +8%，横竖粗细比 1.89→0.85） | 同上 |
| 17–20 px 汉字 | 16×16 点阵**确定性最近邻放大**到目标字号 + 内侧拐角补全 | 同上 |
| ASCII、标点、UI 符号（全档） | Noto Sans CJK SC 的 FreeType hinted 单色骨架 + 笔画核心 | SIL OFL 1.1（源 TTC 不随仓库分发） |

汉字一律使用点阵的**原生方框**（或等比放大后的方框）：每个汉字共用同一方框与同一基线（`ofs_y = baseline_row - 边长`），笔画 1 像素、横平竖直，稀疏字（如「一」）也保留完整横线。早期把方形点阵压缩进 Noto 的紧致字框，各字压缩比不同会导致字歪、断笔；现已由 `tests/test_reader_font_coverage.py` 钉死。

**为什么要动 17–20 px**：旧方案用 Noto 矢量骨架，hinting 把汉字横笔画压成 2 px、竖笔画留 1 px，再叠加按单点灰度随机补的 2 px 笔画核心，同一行里粗细混杂、2×2 黑块随字号从 11 个/字涨到 26 个/字（即「有时粗有时浅」）。改为点阵放大后额外行列按固定间隔落位，横竖粗细比收敛到约 1.0，黑块减少 37–83%。同时旧方案 **16px→17px 墨量突跳 +28%**，反衬得 16 px 偏浅；现在跳变降到 +4.3%，16 px 再补 8% 墨量，跨档观感连续。

12–15 px 被称为「完美」，其 `glyph_bitmap` / `glyph_dsc` / `cmaps` 由门禁用 SHA-256 冻结，任何像素或几何改动都会报错；16–20 px 另由采样契约限制 2×2 黑块密度与横竖 2px 笔画比。方形点阵框比 Noto 的高窄框更小，九档 bitmap 合计约 2.19 MiB。

- 点阵提取表：[`fonts/cjk_strikes.txt`](fonts/cjk_strikes.txt)（纯文本，只含汉字位图，约 0.95 MB）。原 PCF/BDF 不提交。
- 再生成步骤与依赖见 [`tools/extract_cjk_strikes.py`](../tools/extract_cjk_strikes.py) 顶部说明；字库由 `python3 tools/gen_reader_font.py` 生成并逐像素自校验。
- 覆盖率与清晰度门禁：`python3 tools/gen_reader_font.py --check`、`tests/test_reader_font_coverage.py`。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

## 电池参数（battery）

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
保存万锂新能源 500 mAh 电芯的 CW2017 参数（profile2、XIL40X、2026-10-20）。
该文件是项目提供的 `喜梨_智能工卡_万锂新能源500mAh_profile2_XIL40X_20261020.txt`
的原样副本，按维护者要求保留用于本次固件更新；原文件没有附带单独的许可说明。

格式为 80 行两位十六进制数字，每行一个字节，按寄存器顺序排列。
BSP 在 `components/bsp/src/bsp_battery.c` 中嵌入这 80 字节，主机检查会验证
源文件及数组内容完全一致。文本文件 SHA-256：
`7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`。
解码后 80 字节 SHA-256：
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`。
这是电量计参数，不是充电配置，也不能通用于所有 500 mAh 电芯；详见
[硬件指南](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md#9-cw2017-电池计)。
