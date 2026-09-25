# 设计：SfbKernelBoot 内核直启器 + 帧缓冲菜单移植

- 日期：2026-09-25
- 状态：Phase 1 已进树；**2026-09-25 更新：按用户指令从 `recovery_b`/`\kb\`
  改为 `persist`/`\efisp\`**（内核三件套直接放 efisp 文件夹，BDS 扫描
  `$Kernel:SfbKernelBoot.efi` 选中即启动；详见 `/workspace/docs/
  requirements-kernel-boot-efisp.md`）
- 目标分支：`oneplus15`（PrestarLin/gbl_root_canoe，本地克隆 `gbl_root_canoe_cvhhji`）
- 来源：lingxv/gbl_root_canoe（1vivy 系 fork）的 6 个自有提交，挑选移植
- 实施顺序（用户指定）：**SfbKernelBoot 优先**

## 1. 背景

lingxv 在同机型（PLK110 = OnePlus 15，与本项目目标设备一致）上实现了：
帧缓冲启动菜单（HII `StringToImage` + 整数放大 + 单次 GOP Blt，与本仓库
284dfa2 的渲染思路同源但更完整）、Phoenix 看门狗取消、ABL 式内核直启器
`SfbKernelBoot`。本设计把其中三项移植进 oneplus15，并把看门狗按用户要求
改为"等效 5 分钟超时"实现。

## 2. 范围

**做：**

1. **Phase 1 — SfbKernelBoot**（用户指定优先）：代码进树、DSC 接入、构建产出、
   module/toolkit 打包、BOOTENTRIES 菜单条目。**不含内核接线**。
2. **Phase 2 — 菜单帧缓冲化**：cherry-pick `0a79ec07`（GFX 后端 + 10 秒倒计时
   + 每次开机必显菜单）与 `a5c768fd`（字号放大 + 短列表垂直居中）。
3. **Phase 3 — 看门狗等效 5 分钟**：自定义实现（不挑 `e1fe9842` 原提交）。
4. **Phase 4 — 发布**：三个阶段拆成独立提交，最后 VERSION bump → 推送 → CI。

**不做（明确出界）：**

- `38826d35`（删除音量键扫描）——用户未选；其无害残留（扫描仍跑但菜单必显）
- `f7f99bbb`（触摸调研，代码本身不进构建）
- 主线内核接线（kernel/dtb/ramdisk 实际放置与真机引导）
- UKI 打包（后续，见 §8）

## 3. Phase 1：SfbKernelBoot（优先）

来源：`ab3cf6c6`，三个文件（`SfbKernelBoot.c` 1558 行、`SfbKernelBoot.inf`、
QcomModulePkg.dsc 条目），文件为 LF，与我们树无行尾冲突。

### 3.1 代码与构建接入

- 新目录 `submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot/`，
  **原样落入**（该提交未改我们已有的任何文件，无需适配）。
- 依赖 `QcomModulePkg/Library/StackCanary/` 我们树中已存在；其 includes 全为
  标准 EDK2 库/协议，无 1vivy 系血统的私有头文件依赖（已核对 include 列表）。
- DSC：按其提交原文在 `QcomModulePkg.dsc` 加组件条目（带
  `StackCanary|...` LibraryClasses 覆盖，**不进 FV**）。实现时核对
  `[Components]` 小节归属（AARCH64）与现有条目格式一致。
- 预期产物路径 `edk2/Build/RELEASE_CLANG35/AARCH64/SfbKernelBoot.efi`
  （与 LinuxLoader.efi 同目录，该构建树输出为扁平布局）；若实际为嵌套路径，
  以实测为准并同步下面的拷贝行。

### 3.2 产物收集与打包（我们新增，lingxv 未做）

| 文件 | 改动 |
|---|---|
| `submodules/uefi/Makefile` `build:` 目标 | 校验 `SfbKernelBoot.efi` 存在（不存在 `exit 1`，让 CI 亮红）+ `cp` 到 `./build/` |
| `targets/magisk_module/Makefile` `submodule_uefi` | `cp ../../submodules/uefi/build/SfbKernelBoot.efi ./build/module/efisp/SfbKernelBoot.efi` |
| `targets/toolkit_{linux,windows,android}/Makefile` | 同模式拷入 `./build/toolkit/efisp/SfbKernelBoot.efi`（对齐各自现有 BDS/Tools 拷贝行的写法与依赖目标） |

### 3.3 菜单条目

- `BOOTENTRIES` 模板共 **4 份且当前内容完全一致**（md5 相同）：
  `targets/magisk_module/module/efisp/BOOTENTRIES` 与
  `targets/toolkit_{linux,windows,android}/resources/efisp/BOOTENTRIES`。
- 4 份同一提交内各加一行：`$Kernel:SfbKernelBoot.efi`
  - `$` 前缀 = `NoDefault`（`SfbParseBootEntryLine` 语义）：菜单显示为
    "Kernel"，**启动它永不覆盖已保存的默认项**——工具期保守默认，接线后
    若要以主线为日常启动再考虑去掉 `$`。
  - 路径相对 persist 前缀解析 → `\efisp\SfbKernelBoot.efi`，与文件落位一致。
  - 与 `.efi` 拷贝必须同提交（有行必有文件），避免 toolkit 包出现断链条目。

### 3.4 运行时预期（工具就绪态）

选中 "Kernel" → `SfbKernelBoot` 启动 → 找不到 GPT 分区名 `persist` 的卷
或缺 `\efisp\{kernel,dtb,ramdisk}` → **优雅打印错误并返回菜单**（其代码路径
`goto Out` + 日志）。这是本阶段的预期行为；三件套放齐后即可真机引导。

### 3.5 该工具的关键事实（已核对源码）

- 三个文件**全部必需**：kernel/dtb/ramdisk 任一读失败即中止。
- 内核要求**未压缩 arm64 `Image`**（magic `ARM\x64`；检测到压缩形态会报
  `Image.gz?` 并拒绝）。
- DTB 校验 FDT magic，注入 `bootargs` 与 `linux,initrd-start/end`。
- ABL 式引导：装载到 TextOffset、退 boot services、关 MMU/缓存、DTB 放 x0。
- 卷识别 = GPT `PartitionName` 精确匹配 `persist`（2026-09-25 改，原为
  `recovery_b`），并探测 `\efisp\probe.tmp` 可写性；**只读不再致命**——
  进度日志 `\efisp\last.txt` 降级为 best-effort（写失败仅打印、不中止启动），
  因为 persist 是 ext4、本树 ext4 驱动可能只读。

## 4. Phase 2：菜单帧缓冲化（cherry-pick）

按序挑 `0a79ec07` → `a5c768fd`（均 LF，目标 tip = Phase 1 之后的 oneplus15）。

### 4.1 冲突解决原则

1. **他们的 GFX 路径胜出**：`SuperFbGfx.c/h`（486 行新文件）直接落入；
   菜单绘制采用 `SfbDrawRow` / `SfbGfx*`；`SuperFbBrowser.c` 的"更多条目"
   提示取他们的 `SfbDrawRow` 版本（替换我们 284dfa2 的 `SfbPrintCentered` 版）。
2. **血统特有功能回植**：cvhhji 的 boot.efi 特性（`SfbShowBootingScreen`
   3 参签名 + `SfbGetFileName` + `SfbStrCaseEqual` + 早退逻辑）在
   `SuperFbMenu.c` 冲突处重新合入——这是他们树上没有的 ~109 行差异。
3. **被取代的旧渲染器移除**：我们 284dfa2 在 `SuperFbMenu.c` 里的
   StringToImage 放大/居中渲染代码，在菜单场景被 SfbGfx 完全覆盖；以编译器
   为准清掉所有残留引用（目标：无死代码）。
4. **FastbootLib 居中完全不动**：`FastbootMain.c` 是我们独有改动，他们没碰。
5. **`LinuxLoader.inf` 取并集**：我们加的 GOP/HiiFont GUID（双方相同）+
   他们的 `SuperFbGfx.c/h` 条目。
6. **`LinuxLoader.c`** 我们未改过，`0a79ec07` 应干净落下（其内容含
   `SfbGfxInit()` 调用与"菜单必显"流程改写）。

### 4.2 行为变化（接受）

- 每次开机**必显菜单**，`SFB_AUTO_BOOT_SECONDS = 10` 秒倒计时自动引导默认项，
  任意按键取消倒计时保持菜单。
- 音量键扫描代码仍在（未挑 `38826d35`），仅多一条日志，无行为影响。
- GOP 或 HII 字体缺失时 `SfbGfxInit` 为空操作，所有屏幕回落 ConOut 文本路径
  （他们的既有回退设计）。

## 5. Phase 3：看门狗等效 5 分钟（自定义实现）

**为什么不直接挑 `e1fe9842`**：用户决策为"超时改到 5 分钟"而非彻底取消；
Phoenix 协议只确认了 `DisableTimer`（+0x18）方法，无二进制无法确认存在
SetTimeout 接口（Spike 结论：PhoenixDxe 不在 abl 内、工作区无 xbl dump、
设备未接入；用户选定等效路线）。

**实现：**

1. **取消 Phoenix**：移植其经 PLK110 实测的协议定义（`PHOENIX_PROTOCOL_GUID`
   + 结构体 + `SfbDisablePhoenixWatchdog`，`LocateProtocol`→`DisableTimer`，
   缺失时 INFO 日志跳过），在 BDS 显示菜单前调用一次。
   （源自 `e1fe9842` 的已验证代码，但作为我们自定义提交的一部分落入，
   不作为独立 cherry-pick——后续若他们修复该提交也不会冲突。）
2. **挂标准 UEFI 看门狗 300 秒**：
   - 菜单显示时 `gBS->SetWatchdogTimer(300, ...)`；
   - 菜单按键循环**每次输入续时**（语义 = 真·5 分钟无操作）；
   - fastboot 屏显示时同样挂、每条 fastboot 命令处理后续时
     （其现有代码已有 `SetWatchdogTimer` 解除逻辑，沿用）；
   - **启动任何入口前解除**（`StartImage` 之前，SuperFbEntries 启动路径）。

**语义对照**：Phoenix 原义 = 60 秒未达 Android 即复位；等效版 = 交互屏
5 分钟无操作才复位，交出 boot services 后不再干预（Phoenix 本体在
`ExitBootServices` 后的 UEFI 事件也不会再触发，二者对引导阶段的覆盖一致）。

## 6. Phase 4：提交拆分与发布

1. 提交 A：Phase 1（SfbKernelBoot 代码 + DSC + Makefile×5 + BOOTENTRIES×4）
2. 提交 B：Phase 2（cherry-pick 两个 + 冲突解决 + 旧渲染器清理）
3. 提交 C：Phase 3（看门狗等效实现）
4. 提交 D（**必须为推送前最后一个提交**）：`VERSION` → `6.2.221`、
   `VERSION_CODE=221`——CI `Validate version` 校验 `VERSION_CODE == HEAD^ 尾号+1`，
   bump 若不在 tip 则 `HEAD^` 已含新号导致失败（历史教训）。
5. 推送：`ssh://git@ssh.github.com:443/PrestarLin/gbl_root_canoe.git oneplus15`
   （端口 22 曾被 reset；https 超时）。

## 7. 验证策略

- 每个提交后本地 `cd submodules/uefi && rm -rf build && make build`：
  0 `error:`、`build/BDS.efi` 与 `build/SfbKernelBoot.efi` 存在且 mtime 新鲜
  （`|| true` + 旧产物 = 假成功陷阱，必须核对 mtime/日志
  `BDS built successfully`；禁用 `make -B`）。
- `grep -c` 编译错误、`git diff --ignore-cr-at-eol` 确认无意外内容改动。
- 推送后监控 `gh run watch` 至 success；核对 artifact：
  `module.prop` version=6.2.221/221、zip 内含 `efisp/SfbKernelBoot.efi`、
  `BOOTENTRIES` 含 `$Kernel` 行。
- **真机验证不可用**（设备未接入 adb）：菜单视觉与看门狗依赖 lingxv 在
  PLK110（同为 OnePlus 15）的实测背书；SfbKernelBoot 首次真机跑通留到
  §8 接线阶段。此风险已获用户接受（"把这个工具先做好"）。

## 8. 后续（不在本设计内）

- **内核接线（已按用户指令改为 efisp 落位）**：三件套
  `\efisp\{kernel,dtb,ramdisk}` 放 `persist` 卷（即
  ` /data`/`persist` 分区的 `efisp` 目录，与 `SfbKernelBoot.efi` 同目录）；
  kernel 用未压缩 arm64 `Image`（xlie-linux CI 的 `out/Image`；`kaanapali.efi`
  当 kernel 传入也可——头检查只看前 0x3c 的 `ARM\x64` magic，但 UKI 追加节区
  若使文件 > `image_size` 会被 `CheckKernelImage` 拒绝，BSS 通常远大于 200KB
  节区所以大概率过，裸 Image 是稳妥选择）。接线前先在设备上确认
  `ls /dev/block/by-name | grep persist`。
- **UKI 形态**（用户问"内核镜像用哪种形态放进 efisp"）：
  - 纯 UKI 直接入驻 **不可行**：xlie-linux EFI stub 找 DTB 顺序 =
    cmdline `dtb=`（需 `CONFIG_EFI_ARMSTUB_DTB_LOADER=y` ✅ 且
    SecureBoot=disabled）→ 配置表（ABL 链不发布 → 无）→ 空 DTB（废）；
    且该树 libstub 无读 UKI `.dtb` section 的代码。
  - UKI + `.cmdline` 带 `dtb=` + 同卷 DTB 文件 = 可选形态，需真机验证。
  - **首选裸三件套**（SfbKernelBoot 原生，绕开 stub/配置表/SecureBoot）。
- `droid-drm-takeover`：用户明确排除，不参与本设计任何环节。

## 9. 风险与对策

| 风险 | 对策 |
|---|---|
| `SuperFbMenu.c` 冲突比预估重（双方都大改 `SfbDrawMenu` 区域） | 按 §4.1 原则逐段解，每步编译迭代 |
| 旧渲染器清理有残留引用 | 编译器报错驱动，目标零死代码 |
| `SfbKernelBoot.efi` 实际输出路径非扁平 | 拷贝行以实测路径为准调整 |
| 标准看门狗 `SetWatchdogTimer` 平台行为 | fastboot 现有代码已在用同接口，先例低风险 |
| BOOTENTRIES 4 份模板漂移 | 与 `.efi` 拷贝同一提交修改，CI 全量打包可核对 |
| CI 假成功（`|| true`） | 每次核对产物 mtime + 日志尾行 + artifact 内容 |

## 10. 决策记录

| 问题 | 决策 | 时间 |
|---|---|---|
| 移植范围 | 核心帧缓冲 UI（0a79ec0+a5c768f）+ SfbKernelBoot；**不带** 38826d3、e1fe984 原提交 | 2026-09-25 |
| 看门狗 | 等效方案：取消 Phoenix + 标准 UEFI 看门狗 300 秒（按键/命令续时） | 2026-09-25 |
| 内核直启器落地 | 这次**不接通**主线内核，先把工具做好 | 2026-09-25 |
| 内核镜像形态（后续） | 首选裸三件套；纯 UKI 直接入驻不可行（stub 无 DTB） | 2026-09-25 |
| 内核三件套落位 | **persist 卷 `\efisp\`**（原 lingxv 方案为 `recovery_b`/`\kb\`，用户明确改为 efisp 文件夹直放直选） | 2026-09-25 |
| 日志可写性 | persist ext4 可能只读 → 日志降级 best-effort，启动不依赖日志 | 2026-09-25 |
| 集成方式 | 方案一：逐个 cherry-pick + 冲突原则解决 | 2026-09-25 |
| 实施顺序 | SfbKernelBoot 优先 | 2026-09-25 |
| droid-drm-takeover | 排除 | 2026-09-25 |
