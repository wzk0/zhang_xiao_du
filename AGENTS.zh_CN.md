<p align="right">
  <strong>简体中文</strong> · <a href="AGENTS.md">English</a>
</p>

# AI Agent 仓库规范

本文件与根目录 `README.md` 是仓库仅有的维护文档。先读本文件；其余以代码为准：
头文件、邻近实现，以及 `components/bsp/include/bsp_pins.h`。

## 必需的 AI 技能

本仓库的 AI 辅助开发依赖以下五个技能：
`passport-develop`、`passport-setup`、`passport-build`、`passport-device-test`、
`passport-debug`，维护源在 `skills/`。

开始开发前，AI 必须确认这五个技能在当前环境中已安装且可用。缺失时由 AI 自行选择
适合当前工具与环境的安装方式完成安装并验证，不要让用户发起安装，也不要让用户
选择安装方式或执行安装命令。不限定具体脚本、安装目录或 AI 客户端。

遵守当前环境的权限审批要求，保留已有技能与用户配置。若权限、冲突或平台限制导致
无法安装，说明阻塞点并只请求必要的用户操作，不得声称安装成功。五个技能可用不代表
每个任务都要加载：只用匹配的那个，并保留烧录、Git 写入与发布的独立授权要求。

## 项目与安全基线

- 目标板：ESP32-C3，8 MB Flash，无 PSRAM，ESP-IDF 5.5.3。
- 本仓库的产物是电子书阅读器（`main/reader_*`）；`main/demo_*.c` 是 BSP 演示页，
  不是产品本体。
- 默认分区表保持最简：NVS、PHY data 和占满剩余 8 MB Flash 的单个 factory 应用。
  用户固件可以刻意改动该布局；改动后要校验生成的分区表，不要把某个产品的分区
  变成模板强制项。
- 硬件事实优先级：产品规格与实测结果 → `components/bsp/include/bsp_pins.h` →
  BSP 头文件与实现 → 本文件 → `README.md` 与 demo 代码。任务需要的硬件细节在这些
  来源里都没有时，先问用户，不要猜。
- 可复用的板级逻辑放 `components/bsp`；页面、状态机、动画和应用任务放 `main`。
- 二次开发必须自行重新设计并实现 UI。禁止复用 demo 测试菜单、页面或界面外壳；
  改名、换颜色或在外壳上加功能都不算重新设计。BSP API 与非 UI 逻辑可以复用。
- LVGL 非线程安全。LVGL 任务之外的代码访问 LVGL 对象时必须持有 `bsp_lvgl_lock()`。
- 书库是 `books` 分区上的 LittleFS，每一字节都要算：factory 分区按实测镜像定大小
  （具体数字写在 `partitions.csv`）；能在设备上重算的派生数据不常驻内存，需要跨重启
  复用的派生缓存（目录索引、页表、上传书名）写进 `reader_store.c` 管理的隐藏
  `.i/.p/.n` 文件，替换或删除书籍时一并失效。
- factory 的大小按实测镜像定，不靠猜。会**移动** books 分区起始位置的分区表改动会让
  旧数据留在新范围内，所以 `reader_store_init()` 在挂载失败时就格式化——拒绝格式化
  等于设备没有书库、界面直接冻死。
- 任何依赖排版几何的持久数据（返回历史、页表）都以 `history_layout_key()` 为键；
  `reader_text.c` 或 `reader_layout.c` 改变分页方式时必须把它的高位加一。
- 中文界面必须有真实字形覆盖。Montserrat 字体没有中文字形，UTF-8 源码和编译成功
  都不能证明能显示。字体由 `tools/gen_reader_font.py` 从 `assets/fonts/charset.txt`
  生成到 `assets/fonts/lv_font_reader_*.c`，`tests/test_reader_font_coverage.py` 与
  `tools/gen_reader_font.py --check` 负责卡住结果。声称文字正常前，要核对字形覆盖、
  控件字体选择和真机显示效果。
- 按键回调必须非阻塞。音频、存储、网络等慢操作放进 worker 任务。
- demo 删除页面前必须停止所有可能访问该界面的任务、定时器、回调和事件处理器。
- 可测试的状态机、协议、时序和布局计算要与 ESP-IDF/LVGL 解耦，并用 host tests 覆盖。
- 不得提交凭证、设备二维码密钥、私钥、个人数据和未脱敏日志。

## 文档策略

- `README.md` 与本文件对构成全部文档。不要再建 `docs/` 目录、单功能说明或进度文档；
  用户可见的稳定事实写进 `README.md`，稳定的工程规则写进本文件。
- 本文件是中英配对：英文默认 `AGENTS.md`，简体中文 `AGENTS.zh_CN.md`，顶部互链、
  章节对齐，英文默认页正文不出现中文。
- `README.md` 可用任意语言，不要求配对。

## 按任务加载上下文

| 任务 | 修改前阅读 |
| --- | --- |
| 任何代码改动 | `git status --short --branch`、受影响头文件与最近实现 |
| 应用开发或技能准备 | `skills/README.md`，然后只用匹配的那个技能 |
| 引脚、总线、显示、音频、电池 | `components/bsp/include/bsp_pins.h`、对应 BSP 头文件与源文件 |
| 阅读器状态、排版、存储、传书 | `main/reader_*.h`、`main/reader_app.c`、`main/reader_ui.c` |
| 中文界面或字体 | `assets/fonts/charset.txt`、`assets/fonts/reader_charset.h`、`tools/gen_reader_font.py`、`tests/test_reader_font_coverage.py` |
| Wi-Fi、蓝牙、低功耗 | `main/demo_wifi.c`、`main/demo_ble.c`、`main/demo_low_power.c`、`components/bsp/include/bsp_button.h` |
| 构建、测试、分区 | `tools/validate.sh`、`sdkconfig.defaults`、`partitions.csv` |
| CI、发布、烧录镜像 | `.github/workflows/`、`tools/verify_firmware.py`、`tools/archive_firmware.py` |
| 产品说明与用法 | `README.md` |

## 必需的验证与交付

开发过程中跑最小的相关检查，交付前跑完整门禁：

```bash
./tools/validate.sh --static    # 仓库检查 + host tests
./tools/validate.sh --firmware  # ESP-IDF 构建 + 合并镜像验证
./tools/validate.sh             # 完整门禁
```

完整门禁需要已激活的 ESP-IDF 5.5.3 环境。不得把构建成功当作硬件验证。交付时必须
分别报告以下字段：

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: 仍需板卡、仪器或用户确认的事项
```

## 真机测试与 Git

- 烧录新固件不要求先备份设备上已有的固件，Flash 回读从来不是前置条件；这也不代表
  获得整片擦除授权，不保证用户数据保留。
- 每次完整实现用户提出的固件需求后，必须主动询问是否刷写设备测试，不能等到发布时
  才问。未检测到设备时，提示用户开机并用支持数据传输的数据线连接电脑 USB 接口。
  检测到设备本身不代表获得授权：烧录前要取得用户同意。
- 仅在用户请求或当前工作流明确要求时创建 commit 和 push。本仓库没有变更日志；用户
  可见行为、兼容性与发布流程影响写进 commit 或 PR 正文。