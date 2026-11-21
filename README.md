## 掌小读

> FOLOTOY 上的txt电子书阅读系统.

## 截图

![首页](photo/index.png)
![封面](photo/cover.png)
![书籍](photo/book.png)
![书签](photo/mark.png)
![设置](photo/setting.png)
![储存](photo/space.png)
![暗色](photo/black.png)

## 特性

- 热点传书
- 书签, 目录跳转
- UI 简洁美观
- 实时储存查看
- ...

## 书格式（.zdx）

内置书和上传书统一存成 `.zdx`：UTF-8 文本按 4 KiB 分块，每块独立 raw-deflate
压缩。`reader_store_read()` 对外仍按**原始**字节偏移读，所以分页、书签、目录跳转和
阅读进度与未压缩时完全一致；中文小说一般压到原大小的 55%，内置的两本书从 84 KiB
降到约 45 KiB Flash。

二进制布局（小端；表中偏移是绝对文件偏移）：

| 范围 | 字段 | 含义 |
| --- | --- | --- |
| 0..3 | magic | `ZDX1` |
| 4 | version | `1` |
| 5 | flags | bit0 = raw deflate |
| 8..11 | chunk size | 4096（每块原始字节数） |
| 12..15 | chunk count | deflate 流数量 |
| 16..19 | original size | 未压缩 UTF-8 字节数 |
| 20..23 | text chars | 非空白码点数（UI 提示用） |
| 32.. | chunk table | `count` 个 `{u32 offset, u32 length}` |
| ... | chunk data | 依次拼接的 raw-deflate 流 |

- **转换在浏览器里完成**：热点没有外网，`assets/web/transfer.html` 用内联 pako
  （`raw: true, windowBits: 12, level: 9`）分块压缩后上传。网页显示书库当前可上传
  的压缩包大小，设备在写入前重新检查剩余空间并预留文件系统开销。原始 `.txt` 可超过
  2 MB，转换速度和可处理的大小取决于浏览器及设备内存。
- **打开大书**：首次扫描显示目录进度。上传书的目录与最近翻页记录会写入书库缓存，
  再打开时可复用；替换或删除书籍会清除相关缓存。
- **补字库**：保留原有九套主字形，共用文泉驿点阵宋体的 16 像素 GB2312 二级汉字补字库。
  缺字时按当前字号渲染，补字数据存于应用分区。上传仍缺字的书时，网页会列出部分字符及
  Unicode 码点。用 `tools/gen_reader_fallback.py` 和文泉驿的 `wenquanyi_12pt.bdf` 可重新生成。
- **内置书**：`assets/books/*.txt` 是唯一原文，`tools/gen_builtin_books.py` 生成
  `main/builtin_books_data.c`；设备用 `reader_zdx_open_mem()` 直接读 Flash 镜像，
  不占额外 RAM。字体生成器也扫描这些 `.txt`，保证内置书用到的字都有字形。
- **解码**：`components/uzlib/`（uzlib tinflate，zlib 许可）。无 PSRAM，每个解码器
  用固定缓冲（4 KiB 字典 / 4 KiB 输出 / 8 KiB 压缩输入）；块之间不共享字典，所以
  书签与目录跳转是 O(1) 的块加载。
- **参考实现与测试**：`tools/zdx_codec.py` 是字节布局的参考编解码器；
  `tests/test_reader_zdx.py` 用固件同一份 `main/reader_zdx.c` + uzlib 跑往返、跳转、
  头部和截断校验。

## 使用

从 Release 中下载烧录即可。合并镜像 `FoloToy-AI-Passport-full.bin` 从 `0x0` 烧录，
浏览器烧录工具：<https://ai-passport.folotoy.cn/tools/web-flasher/>。

## 开发

板卡 ESP32-C3 / 8 MB Flash / ESP-IDF 5.5.3。构建与验证：

```bash
./tools/validate.sh --static    # 仓库检查 + host tests
./tools/validate.sh --firmware  # ESP-IDF 构建 + 合并镜像校验
./tools/validate.sh             # 完整门禁
```

工程规则（BSP 边界、LVGL 锁、中文字体、真机交付格式）见 `AGENTS.md`；AI 技能在
`skills/`。PR 和 `main` 推送只跑静态检查与主机测试。推送 `v*` 标签运行完整验证并
发布固件。在 Actions 手动运行 Release 工作流可构建未发版的固件包，上游同步也仅手动触发。
