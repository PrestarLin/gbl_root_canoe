# SfbKernelBoot + 帧缓冲菜单移植 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 lingxv/gbl_root_canoe 的 SfbKernelBoot 内核直启器、帧缓冲菜单与 5 分钟看门狗等效实现移植到本地 oneplus15 分支并交付 CI 验证的产物。

**Architecture:** 先落 SfbKernelBoot 并接通打包链（Makefile cp + BOOTENTRIES），再用 `git cherry-pick -x --no-commit` 逐个搬入两个 UI 提交（保原作者信息与可回滚粒度），最后以自定义提交补看门狗等效实现，VERSION bump 作为推送前最后一个提交。

**Tech Stack:** EDK2/UEFI C（QcomModulePkg）、GNU Make、git cherry-pick、GitHub Actions CI。

**Spec:** `docs/superpowers/specs/2026-09-25-sfckernelboot-framebuffer-menu-design.md`（本计划论据皆出自该 spec；执行者需同时阅读）

## Global Constraints

- 仓库当前 tip `9e5db9db`，分支 `oneplus15`，工作区必须保持全 LF：新文件/编辑后 `grep -c $'\r' <file>` 必须为 0。
- 构建唯一合法方式：`cd submodules/uefi && rm -rf build && make build`；禁止 `make -B`；**build 目录存在 = make 认为目标最新（假成功）**，每次验证前必删。成功断言三件套：日志含 `BDS built successfully`、产物 mtime 晚于构建起始、`build/` 下产物存在。
- 每完成一个任务提交一次（用户指令），提交落在最后一步；**VERSION bump 必须是推送前最后一个提交**（CI 校验 `HEAD^` 的 VERSION，现值 `6.2.220` → 目标 `6.2.221`）。
- 提交信息格式沿用本仓风格（小写 `bds:`/`docs:`/`build:` 前缀）；cherry-pick 使用 `-x` 保留来源引用。
- 不改 `droid-drm-takeover/`；不改 FastbootLib 既有 watchdog disarm 行为（`FastbootCmds.c:2182` 保持原样）。
- 推送只走 SSH 443：`git push ssh://git@ssh.github.com:443/PrestarLin/gbl_root_canoe.git oneplus15`。
- BOOTENTRIES 共 4 份，任何时候 `md5sum` 必须一致（现值 `cef76466363eda89c6023bbd489adafa` 为改动前基线）。

## Review Focus

1. **Phoenix 协议缺失**（非 PLK110 或无 PhoenixDxe）时 `LocateProtocol` 失败 → 必须只记 INFO 日志并返回，不得空指针/挂死。→ Task 6 步骤 6 核对 error 分支代码 + 构建通过（代码来自 PLK110 实测补丁 e1fe9842）。
2. **看门狗 rearm 覆盖漏洞**：子菜单停留、工具返回、fastboot 返回后若无人 arm，300s 后误重置。→ Task 6 步骤 5 的 grep 断言：所有等待循环 + fastboot 循环均含 arm 行。
3. **SfbKernelBoot 失败路径**（缺 kernel/dtb/ramdisk、magic 不符）必须 EFI_ERROR 返回 → 菜单 `SfbReportStatus` 报错并回菜单，不得黑屏挂死。→ Task 1 步骤 5 审查所有 return 路径 + 构建通过。
4. **BOOTENTRIES 四份漂移**（只改一份导致某平台菜单缺项）。→ Task 2 步骤 3 四份 md5 一致断言。
5. **陈旧 build 目录假成功**（改了源码但产物是旧的，任务间验证全部失效）。→ 每个构建步骤执行 Global 的"删 build + 三件套断言"。

---

### Task 1: SfbKernelBoot 源码落树 + DSC 接入 + 构建验证

**Files:**
- Create: `submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot/SfbKernelBoot.c`（1558 行，来自 ab3cf6c6）
- Create: `submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot/SfbKernelBoot.inf`（60 行，同上）
- Modify: `submodules/uefi/edk2/QcomModulePkg/QcomModulePkg.dsc`（文件末尾追加组件块，+7 行）

**Interfaces:**
- Consumes: 既有 `QcomModulePkg/Library/StackCanary/`（已确认存在 `StackCanary.c/.inf`）；构建系统 `submodules/uefi/Makefile` 的 `build` 目标。
- Produces: 产物 `submodules/uefi/edk2/Build/RELEASE_CLANG35/AARCH64/SfbKernelBoot.efi`（Task 2 的 cp 源路径）；DSC 组件块（后续任务不再改动 DSC）。

- [ ] **Step 1: cherry-pick --no-commit 取文件（不带提交）**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git cherry-pick -x --no-commit ab3cf6c6
git status --short
```

预期：3 个变更文件（SfbKernelBoot.c/h 新增、dsc 修改）。DSC 冲突预期为**无**——该 hunk 上下文为末三行 `StackCanary|QcomModulePkg/Library/StackCanary/StackCanary.inf` / `FastbootLib|...FastbootLib.inf` / `}`，与本仓 dsc 尾部逐字相同。

- [ ] **Step 2: 若 dsc 冲突则手工解决（无冲突则跳过）**

若 `git status` 显示 `UU .../QcomModulePkg.dsc`，打开冲突文件，将冲突标记整段替换为以下内容（即在文件最后一个组件块的 `}` 之后追加）：

```
	# Also not placed in the FV. Starts a Linux kernel the way ABL does, which
	# needs the cache and MMU primitives LinuxLoader does not use.
	QcomModulePkg/Application/SfbKernelBoot/SfbKernelBoot.inf {
		<LibraryClasses>
			StackCanary|QcomModulePkg/Library/StackCanary/StackCanary.inf
	}
```

然后 `git add submodules/uefi/edk2/QcomModulePkg/QcomModulePkg.dsc`。

- [ ] **Step 3: LF 检查**

```bash
grep -c $'\r' submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot/SfbKernelBoot.c \
  submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot/SfbKernelBoot.inf \
  submodules/uefi/edk2/QcomModulePkg/QcomModulePkg.dsc
```

预期：三行全为 `0`。若非 0：`sed -i 's/\r$//' <file>` 后复查。

- [ ] **Step 4: 构建验证**

```bash
cd submodules/uefi
rm -rf build
BEFORE=$(date +%s)
make build 2>&1 | tee /tmp/opencode/task1-build.log | tail -20
grep -q "BDS built successfully" /tmp/opencode/task1-build.log && echo LOG_OK
find edk2/Build -name SfbKernelBoot.efi -newer /tmp/opencode/task1-build.log -o -name SfbKernelBoot.efi | head
stat -c '%Y %n' build/BDS.efi
```

预期：`LOG_OK`；`find` 命中 `edk2/Build/RELEASE_CLANG35/AARCH64/SfbKernelBoot.efi`（以实际路径为准，记录下来给 Task 2）；`build/BDS.efi` mtime > `BEFORE`。
若 `find` 未命中：检查 `grep -i sfckernelboot /tmp/opencode/task1-build.log` 是否有编译报错，修到通过为止（常见：inf 引用缺失 → 核对 `git show ab3cf6c6 --stat` 的 3 文件是否齐全）。

- [ ] **Step 5: 失败路径审查（Review Focus #3）**

```bash
grep -n "return EFI_\|return Status" submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot/SfbKernelBoot.c | head -30
grep -n "GetFile\|LoadFile\|ARM\x64\|ARMd" submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot/SfbKernelBoot.c | head -20
```

核对：每个缺失文件/坏 magic 分支都 `return EFI_NOT_FOUND / EFI_UNSUPPORTED / EFI_INVALID_PARAMETER` 等错误码（不 `while(1)` 不死循环）。入口是菜单项经 `SfbLaunchEntry → StartImage`，错误返回后由既有 `SfbReportStatus` 显示并回菜单（`SuperFbEntries.c:1093` 返回路径已存在）。发现 `while (TRUE)` 卡死分支 → 改为 return 错误码再构建一次。

- [ ] **Step 6: 提交**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git add submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot/ submodules/uefi/edk2/QcomModulePkg/QcomModulePkg.dsc
if [ -f .git/CHERRY_PICK_MSG ]; then git commit -F .git/CHERRY_PICK_MSG; else
  git commit -m "bds: add SfbKernelBoot, an ABL-style kernel launcher

Cherry-picked without source changes from lingxv/gbl_root_canoe ab3cf6c6 (-x)."
fi
git log --oneline -1
```

---

### Task 2: 打包接线（4 个 Makefile + 4 份 BOOTENTRIES）

**Files:**
- Modify: `targets/magisk_module/Makefile`（`submodule_uefi` 块内 BDS cp 行之后加 1 行）
- Modify: `targets/toolkit_linux/Makefile`、`targets/toolkit_windows/Makefile`、`targets/toolkit_android/Makefile`（各自 `submodule_uefi` 块 BDS cp 行之后加 1 行）
- Modify: `targets/magisk_module/module/efisp/BOOTENTRIES`、`targets/toolkit_{linux,windows,android}/resources/efisp/BOOTENTRIES`（4 份同内容，插入 1 行）

**Interfaces:**
- Consumes: Task 1 产物 `submodules/uefi/build/SfbKernelBoot.efi`（Makefile 从 `build/` 取，Makefile 的 `make build` 会把产物复制到该处；若 Task 1 实际落在别处，先补 `submodules/uefi/Makefile` 一行 cp——见 Step 1b）。
- Produces: 安装包内 `SfbKernelBoot.efi`（module 根 / toolkit 根，与 BDS.efi、boot.efi 同目录）；BOOTENTRIES 新行 `$Kernel:SfbKernelBoot.efi`（`$` = 不作默认项，`SfbParseBootEntryLine` 语法）。

- [ ] **Step 1: 确认产物进入 build/ 目录**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
grep -n "SfbKernelBoot\|LinuxLoader.efi" submodules/uefi/Makefile
ls submodules/uefi/build/
```

预期：`build/` 下已有 `BDS.efi`（及工具）；`Makefile` 的 `build` 目标是 `cp edk2/Build/RELEASE_CLANG35/AARCH64/LinuxLoader.efi ./build/BDS.efi` 这类模式。
**1b（仅当 `build/SfbKernelBoot.efi` 不存在时执行）**：在 `submodules/uefi/Makefile` 的 build 目标 BDS cp 行之后加：

```make
	cp edk2/Build/RELEASE_CLANG35/AARCH64/SfbKernelBoot.efi ./build/SfbKernelBoot.efi
```

（路径以 Task 1 Step 4 实际命中为准。）

- [ ] **Step 2: 8 处编辑**

`targets/magisk_module/Makefile`，在 `cp ../../submodules/uefi/build/BDS.efi ./build/module/BDS.efi` 行之后插入：

```make
	cp ../../submodules/uefi/build/SfbKernelBoot.efi ./build/module/SfbKernelBoot.efi
```

`targets/toolkit_linux/Makefile`、`targets/toolkit_windows/Makefile`、`targets/toolkit_android/Makefile`，各自在 `cp ../../submodules/uefi/build/BDS.efi ./build/toolkit/BDS.efi` 行之后插入：

```make
	cp ../../submodules/uefi/build/SfbKernelBoot.efi ./build/toolkit/SfbKernelBoot.efi
```

4 份 BOOTENTRIES 全部改为（在 `$Android backup:boot_backup.efi` 行后插入新行，**文件末尾保持无换行符**）：

```
Android:boot.efi
$Android backup:boot_backup.efi
$Kernel:SfbKernelBoot.efi
%Android Tools:tools\ENTRIES
```

编辑方式示例（保持无尾换行）：

```bash
for f in targets/magisk_module/module/efisp/BOOTENTRIES \
         targets/toolkit_linux/resources/efisp/BOOTENTRIES \
         targets/toolkit_windows/resources/efisp/BOOTENTRIES \
         targets/toolkit_android/resources/efisp/BOOTENTRIES; do
  printf '%s\n%s\n%s\n%s' 'Android:boot.efi' \
    '$Android backup:boot_backup.efi' \
    '$Kernel:SfbKernelBoot.efi' \
    '%Android Tools:tools\ENTRIES' > "$f"
done
```

- [ ] **Step 3: 断言（Review Focus #4）**

```bash
md5sum targets/magisk_module/module/efisp/BOOTENTRIES \
  targets/toolkit_{linux,windows,android}/resources/efisp/BOOTENTRIES
grep -c $'\r' targets/magisk_module/module/efisp/BOOTENTRIES || true
tail -c 60 targets/magisk_module/module/efisp/BOOTENTRIES | xxd | tail -2
```

预期：4 个 md5 完全相同；`grep -c $'\r'` 输出 `0`；xxd 显示文件以 `ENTRIES` 结尾无 `0a`。

- [ ] **Step 4: 打包子目标验证**

```bash
cd targets/magisk_module && make submodule_uefi 2>&1 | tail -5
ls -la build/module/SfbKernelBoot.efi build/module/BDS.efi
cd ../toolkit_linux && make submodule_uefi 2>&1 | tail -5
ls -la build/toolkit/SfbKernelBoot.efi build/toolkit/BDS.efi
```

预期：4 个文件全部存在。`make submodule_uefi` 会进入 `submodules/uefi` 再次 `make build`——build 目录已存在则为快速 no-op（这正是想要的：验证 cp 链，不重编）。若 cp 报 No such file → 回 Step 1b 检查 build/ 落点。

- [ ] **Step 5: 提交**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git add targets/magisk_module/Makefile targets/toolkit_linux/Makefile \
  targets/toolkit_windows/Makefile targets/toolkit_android/Makefile \
  targets/magisk_module/module/efisp/BOOTENTRIES \
  targets/toolkit_linux/resources/efisp/BOOTENTRIES \
  targets/toolkit_windows/resources/efisp/BOOTENTRIES \
  targets/toolkit_android/resources/efisp/BOOTENTRIES
git commit -m "build: package SfbKernelBoot.efi and list it in every BOOTENTRIES"
git log --oneline -1
```

---

### Task 3: cherry-pick 0a79ec07（帧缓冲菜单 + 倒计时）

**Files:**
- Modify: `LinuxLoader.c`（+28）、`LinuxLoader.inf`（+4，SOURCES 加 SuperFbGfx.c/h）、`SuperFbBrowser.c`（±14）、`SuperFbMenu.c`（+403）
- Create: `SuperFbGfx.c`（414 行）、`SuperFbGfx.h`（72 行）（cherry-pick 自动带入）

**Interfaces:**
- Consumes: Task 1/2 已完成（与本任务无接口交集）。
- Produces: `SfbGfxInit(VOID)`、`SfbGfxReady(VOID)->BOOLEAN`、`SfbGfxLayout(...)`、`SfbDrawRow(...)`（SuperFbGfx.h 导出，SuperFbMenu.c/SuperFbBrowser.c 消费）；`SfbRunBootMenu` 带倒计时（`SFB_AUTO_BOOT_SECONDS = 10`）。

- [ ] **Step 1: 发起 pick（不提交）**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git cherry-pick -x --no-commit 0a79ec07
git status --short | head -20
```

预期冲突：`SuperFbMenu.c`（必然）、`LinuxLoader.c`（大概率，见 Step 2）、`LinuxLoader.inf`/`SuperFbBrowser.c`（可能小冲突）。`SuperFbGfx.c/h` 为新文件自动加入。

- [ ] **Step 2: 逐冲突解决（冲突原则 = spec §4）**

对每个 `UU` 文件执行 `git diff <file>` 查看两侧，按下列点位解决：

**LinuxLoader.c** — 已核对的锚点（当前行号）：
1. `:79-80` include 区 → 最终保留两行：`#include "SuperFbMenu.h"` 换行 `#include "SuperFbGfx.h"`（若冲突标记出现，取并集；其上文 `#include <Protocol/SimpleTextIn.h>` 不动）。
2. `:202-205` `LinuxLoaderEntry Address` DEBUG 与 `InitThreadUnsafeStack` 之间 → **此时还没有 Phoenix（Task 6 才加）**，把 pick 带来的 `SfbGfxInit` 注释块+调用放在此处：
   ```c
     DEBUG ((EFI_D_VERBOSE, "LinuxLoaderEntry Address: 0x%llx\n",
            (UINTN)LinuxLoaderEntry));

     /*
      * Bring up the graphical menu. When the platform has no usable GOP or font
      * this is a no-op and every screen keeps its console text rendering.
      */
     SfbGfxInit ();

     Status = InitThreadUnsafeStack ();
   ```
3. `:234` 后 DEBUG 行 → 取 pick 新文案 `"SFB: power-on volume-up detected=%u (menu is always shown)\n"`（分两行排版照 patch）。
4. `:251` `if (!MenuRequested) { SfbLaunchDefaultEntry (); }` 块 → **按 pick 删除整块**（本仓是 `SfbLaunchDefaultEntry ();` 无参，pick 上下文是有参，必然冲突），保留 pick 写入的新注释（"The menu is shown on every boot..."）与 `SfbShowEnteringMenu ();` 行。

**LinuxLoader.inf** — 若冲突：取并集，确保 `SuperFbGfx.c` 与 `SuperFbGfx.h` 都在（验证 `grep -c SuperFbGfx *.inf` = 2）。

**SuperFbBrowser.c**（±14 行，把 `SfbPrintCentered` 换成 gfx 行绘制）— 原则：pick 的改写为准；本仓该文件若有 pick 上下文之外的自有行，保留不动。

**SuperFbMenu.c** — 最大冲突面，逐 hunk 原则：
- pick 新增的大块（颜色表、圆角条、`SfbDrawMenu` 重写、`SfbRunBootMenu` 倒计时循环）→ **全文照 pick**。
- 本仓 boot.efi 特有区域（`SfbShowBootingScreen` 的 FileName/Line 显示等）→ **保留本仓内容**；注意 pick 对 `SfbShowBootingScreen` 的 hunk（`@@ -250`、`@@ -281`）会插入 `if (SfbGfxLayout()) { ... }` gfx 分支——合并方式 = 函数开头接 pick 的 gfx 分支，其下保留本仓 console 主体（双方签名一致：`(Name, FilePath, ClearScreen)`，已核对无签名冲突）。
- 本仓 284dfa2 优化行（字号/居中）与 pick 同行重叠 → 以 pick 为准（a5c768fd 还会再放大一次字号）。
- 每解完一个文件：`grep -n '^<<<<<<<\|^>>>>>>>' <file>` 必须为 0 命中。

- [ ] **Step 3: 构建验证**

```bash
cd submodules/uefi && rm -rf build && make build 2>&1 | tee /tmp/opencode/task3-build.log | tail -20
grep -q "BDS built successfully" /tmp/opencode/task3-build.log && echo LOG_OK
grep -ci "error" /tmp/opencode/task3-build.log || true
```

预期：`LOG_OK` 且 error 计数为 0（`warning` 可容忍，死代码清理留给 Task 5）。若 `-Werror` 失败于未引用函数 → 把该函数从 Step 2 结果中删除（它是被 pick 取代的旧渲染器残留）。

- [ ] **Step 4: 提交**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git add -A submodules/uefi/edk2/QcomModulePkg/
if [ -f .git/CHERRY_PICK_MSG ]; then
  { cat .git/CHERRY_PICK_MSG; echo; echo "Conflict resolution: kept this tree's boot.efi-feature regions and"; echo "took the pick's drawing rewrite wholesale (spec §4 principles)."; } | git commit -F -
else git commit -m "bds: draw the boot menu on the framebuffer, with a boot countdown"; fi
git log --oneline -1
```

---

### Task 4: cherry-pick a5c768fd（字号放大 + 短列表居中）

**Files:**
- Modify: `submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbMenu.c`（+39/−10，单文件）

**Interfaces:**
- Consumes: Task 3 后的 SuperFbMenu.c（pick 的 hunk 以上文为基）。
- Produces: 放大后的字号常量与居中绘制逻辑（Task 5/6 在其上工作）。

- [ ] **Step 1: pick**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git cherry-pick -x --no-commit a5c768fd
git status --short
```

预期：最多 SuperFbMenu.c 单文件冲突（Task 3 若以 pick 为准解决，此处应近乎干净）。

- [ ] **Step 2: 冲突解决 + 核对**

有冲突则 `git diff SuperFbMenu.c`：以本仓已合并版为基，套用 a5c768fd 的 +39/−10（字号常量与 `SfbDrawMenu` 短列表居中分支）。`grep -n '^<<<<<<<' SuperFbMenu.c` = 0。

- [ ] **Step 3: 构建**

```bash
cd submodules/uefi && rm -rf build && make build 2>&1 | tee /tmp/opencode/task4-build.log | tail -10
grep -q "BDS built successfully" /tmp/opencode/task4-build.log && echo LOG_OK
```

- [ ] **Step 4: 提交**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git add submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbMenu.c
if [ -f .git/CHERRY_PICK_MSG ]; then git commit -F .git/CHERRY_PICK_MSG; else
  git commit -m "bds: raise the menu type sizes and centre a short list"; fi
git log --oneline -1
```

---

### Task 5: 旧渲染器死代码审计与清理（条件提交）

**Files:**
- Modify: 可能涉及 `SuperFbMenu.c` / `SuperFbBrowser.c` / `LinuxLoader.inf`（仅删零引用符号）

**Interfaces:**
- Consumes: Task 3/4 后的代码。
- Produces: 无新接口；仅保证构建通过。

- [ ] **Step 1: 引用审计**

```bash
cd submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader
for sym in SfbPrintCentered SfbRenderRow SfbScaleGlyph mSfbConRow mSfbColors; do
  printf '%-20s refs=%s\n' "$sym" "$(grep -c "$sym" *.c *.h *.inf 2>/dev/null | awk -F: '{s+=$2} END {print s}')"
done
```

（符号清单以本仓 284dfa2 实际改动为准：`git show 284dfa2 --stat` 后对每个新增函数/宏跑同上引用计数。）

- [ ] **Step 2: 清理判定**

- 引用计数 = 1（仅定义处）或 = 定义+声明（无调用方）→ 删除定义（及声明、inf 残行）。
- 引用计数 ≥ 2（有真实调用，console 回退路径在用）→ **保留**，记录到本步备注。
- 若全部有引用 → 无删除项，**跳过 Step 3-4，本任务零提交**（在执行日志注明"无死代码"）。

- [ ] **Step 3: 删除后构建**

```bash
cd submodules/uefi && rm -rf build && make build 2>&1 | tee /tmp/opencode/task5-build.log | tail -10
grep -q "BDS built successfully" /tmp/opencode/task5-build.log && echo LOG_OK
```

- [ ] **Step 4: 提交**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git add submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader/
git commit -m "bds: drop renderer helpers superseded by SfbGfx"
git log --oneline -1
```

---

### Task 6: 看门狗等效 5 分钟（Phoenix 取消 + UEFI arm/rearm/disarm）

**Files:**
- Modify: `LinuxLoader.c`（Phoenix 函数 +1 调用）
- Modify: `SuperFbMenu.c`（各等待循环顶部 arm）
- Modify: `FastbootLib/FastbootMain.c`（fastboot 循环顶部 arm）
- Modify: `SuperFbEntries.c`（两处 `StartImage` 前 disarm）

**Interfaces:**
- Consumes: Task 3 后的 `SfbRunBootMenu`/`SfbRunSubMenu` 循环结构；PLK110 实测的 Phoenix 协议布局（GUID + `DisableTimer` @+0x18，来源 e1fe9842，spec §5/§10）。
- Produces: `SfbDisablePhoenixWatchdog(VOID)`（STATIC，仅 LinuxLoader.c 内）；全局行为 = 任何交互界面内 300 秒无输入 → UEFI 看门狗重置；进入任何 `StartImage` 前 disarm。

- [ ] **Step 1: Phoenix 函数（LinuxLoader.c）**

在 `#include "SuperFbGfx.h"`（Task 3 置入）之后、`#define MAX_APP_STR_LEN` 之前插入以下完整块（逐字来自 e1fe9842，PLK110 实测）：

```c
/*
 * OPPO/OnePlus "Phoenix" boot watchdog (PhoenixDxe in the platform UEFI FV).
 *
 * PhoenixDxe arms a private one-shot timer during DXE - 60 s on a normal boot -
 * and resets the device if the boot has not reached Android when it fires. That
 * timer is an event of its own, not the UEFI watchdog timer architectural
 * protocol, so the gBS->SetWatchdogTimer() disarm the fastboot path performs
 * cannot cancel it. Sitting in the boot menu or in the on-device fastboot
 * screen for longer than the timeout therefore reboots the device.
 *
 * The driver also installs a protocol whose third method cancels the timer
 * (SetTimer(TimerCancel) + CloseEvent). Calling it keeps the menu alive for as
 * long as the user needs. Boot modes the handler treats as "not normal" (the
 * official fastboot) are ignored by Phoenix anyway, which is why only the menu
 * and the superfastboot fastboot screen were affected.
 *
 * The layout below was recovered from the shipped PhoenixDxe binary: interface
 * table at image offset 0x9158, DisableTimer at +0x18 (code at 0x1c88).
 */
#define PHOENIX_PROTOCOL_GUID \
  { 0x7D2A39F3, 0x0F8C, 0x47A0, { 0x9B, 0x51, 0xF2, 0x69, 0xB4, 0xBA, 0xF9, 0x93 } }

typedef struct {
  UINT64                     Version;                   /* +0x00 */
  VOID                       *Reserved0;                /* +0x08 */
  VOID                       *Reserved1;                /* +0x10 */
  EFI_STATUS (EFIAPI         *DisableTimer) (VOID);     /* +0x18 */
  VOID                       *Reserved2;                /* +0x20 */
} PHOENIX_PROTOCOL;

/*
 * Cancel the Phoenix boot watchdog when the platform provides it. A missing
 * protocol is normal on devices without Phoenix, so log it at INFO level only.
 */
STATIC
VOID
SfbDisablePhoenixWatchdog (VOID)
{
  EFI_GUID          PhoenixGuid = PHOENIX_PROTOCOL_GUID;
  PHOENIX_PROTOCOL  *Phoenix = NULL;
  EFI_STATUS        Status;

  Status = gBS->LocateProtocol (&PhoenixGuid, NULL, (VOID **)&Phoenix);
  if (EFI_ERROR (Status) || Phoenix == NULL || Phoenix->DisableTimer == NULL) {
    DEBUG ((EFI_D_INFO, "SFB: phoenix watchdog not present: %r\n", Status));
    return;
  }

  Status = Phoenix->DisableTimer ();
  DEBUG ((EFI_D_INFO, "SFB: phoenix watchdog disable -> %r\n", Status));
}
```

- [ ] **Step 2: 调用点（LinuxLoaderEntry）**

在 `LinuxLoaderEntry` 中（Task 3 已把 `SfbGfxInit ();` 放在 `LinuxLoaderEntry Address` DEBUG 之后），定位：

```c
  Status = InitThreadUnsafeStack ();
```

在其**前**插入：

```c
  /*
   * The boot menu and the on-device fastboot screen can sit idle for minutes;
   * Phoenix's boot watchdog resets the device once its timeout expires, so drop
   * it before any interactive screen is shown.
   */
  SfbDisablePhoenixWatchdog ();
```

（最终顺序：Address DEBUG → SfbGfxInit → phoenix 块 → InitThreadUnsafeStack；与 lingxv 原序仅 SfbGfxInit/phoenix 互换，均在任何交互屏之前，功能等价。）

- [ ] **Step 3: 菜单各循环 arm（SuperFbMenu.c）**

```bash
grep -n "while (TRUE)" submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbMenu.c
grep -n "SfbWaitForKey" submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbMenu.c
```

对**每个**含 `SfbWaitForKey` 调用的 `while (TRUE)` 循环（预期 2 个：`SfbRunBootMenu`、`SfbRunSubMenu`；`SfbWaitForKey` 自身实现与 `SfbReportStatus` 暂停的循环**不加**），在 `while (TRUE) {` 后第一行插入：

```c
    /* Five minutes without any key: reset the handset (spec §5). */
    gBS->SetWatchdogTimer (300, 0, 0, NULL);
```

（循环每次回到顶部都会重 arm → 按键浏览/子菜单/工具返回全部续时；倒计时 10s 自动启动不经过此处，属正常流程。）

- [ ] **Step 4: fastboot arm（FastbootMain.c）**

`FastbootInitialize` 中（约 `:622`）：

```c
  while (1) {
    Status = HandleUsbEvents ();
```

在 `while (1) {` 后、`HandleUsbEvents` 前插入：

```c
    /* Five minutes without USB activity or a key: reset the handset. */
    gBS->SetWatchdogTimer (300, 0, 0, NULL);
```

不动 `FastbootCmds.c:2182` 既有 disarm（时序上先于循环首 arm，由本行覆盖）。

- [ ] **Step 5: 启动前 disarm（SuperFbEntries.c）**

两处（`:930`、`:1093`，以实际 grep 结果为准）：

```bash
grep -n "gBS->StartImage" submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader/SuperFbEntries.c
```

在每个 `Status = gBS->StartImage (...)` 行前插入：

```c
  /* Never carry the armed 5-minute watchdog into the payload. */
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
```

（镜像返回菜单后由 Step 3 循环顶部重新 arm。）

- [ ] **Step 6: 覆盖断言 + 构建（Review Focus #1/#2）**

```bash
grep -rn "SfbDisablePhoenixWatchdog\|SetWatchdogTimer" \
  submodules/uefi/edk2/QcomModulePkg/Application/LinuxLoader/ \
  submodules/uefi/edk2/QcomModulePkg/Library/FastbootLib/ | grep -v "^\s*\*"
```

预期命中 = 8：Phoenix 定义 1 + Phoenix 调用 1 + 菜单 arm 2 + fastboot arm 1 + entries disarm 2 + 既有 FastbootCmds.c:2182 1。少任何一个 → 回查 Step 3-5。
核对 Phoenix error 分支（Step 1 代码）：`EFI_ERROR || NULL` → 仅 INFO 日志 + return，无解引用。

```bash
cd submodules/uefi && rm -rf build && make build 2>&1 | tee /tmp/opencode/task6-build.log | tail -10
grep -q "BDS built successfully" /tmp/opencode/task6-build.log && echo LOG_OK
```

- [ ] **Step 7: 提交**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git add submodules/uefi/edk2/QcomModulePkg/
git commit -m "bds: five-minute inactivity watchdog: cancel Phoenix, arm the UEFI timer

Cancels the Phoenix boot watchdog on entry (PLK110-tested layout from
lingxv e1fe9842), arms gBS->SetWatchdogTimer(300) at the top of every
interactive wait loop, and disarms before any StartImage."
git log --oneline -1
```

---

### Task 7: VERSION bump 6.2.221（推送前最后一笔提交）

**Files:**
- Modify: `VERSION`（`VERSION=6.2.220` → `6.2.221`，`VERSION_CODE=220` → `221`）

**Interfaces:**
- Consumes: 前序全部提交（本提交必须为分支 tip，CI 校验 `HEAD^` 版本号）。
- Produces: CI 通过的版本号 6.2.221。

- [ ] **Step 1: bump + 本地模拟 CI 校验**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
sed -i 's/^VERSION=6\.2\.220$/VERSION=6.2.221/; s/^VERSION_CODE=220$/VERSION_CODE=221/' VERSION
cat VERSION
```

预期输出恰为两行：`VERSION=6.2.221` / `VERSION_CODE=221`。

```bash
PREV=$(git show HEAD:VERSION | grep '^VERSION_CODE=' | cut -d= -f2)
CUR=$(grep '^VERSION_CODE=' VERSION | cut -d= -f2)
[ "$CUR" -eq $((PREV+1)) ] && echo CI_VALIDATE_OK
```

预期：`CI_VALIDATE_OK`（HEAD 仍为 bump 前提交 = 模拟 `HEAD^`）。

- [ ] **Step 2: 提交（tip）**

```bash
git add VERSION
git commit -m "build: bump version to 6.2.221"
git log --oneline -3
```

预期：本提交为最新一行，其上依次是 Task 6/5/4/3/2/1 提交。

---

### Task 8: 推送 + CI 盯守 + artifact 核验（无提交）

- [ ] **Step 1: 推送前终检**

```bash
cd /root/workspace/gbl_root_canoe_cvhhji
git status --porcelain | head          # 必须为空
git log --oneline -8                   # 通读：bump 在顶
grep '^VERSION=' VERSION               # 6.2.221
```

- [ ] **Step 2: SSH443 推送**

```bash
git push ssh://git@ssh.github.com:443/PrestarLin/gbl_root_canoe.git oneplus15
```

- [ ] **Step 3: CI 盯守**

```bash
sleep 10
gh run list --repo PrestarLin/gbl_root_canoe --branch oneplus15 -L1
gh run watch <run_id> --repo PrestarLin/gbl_root_canoe --exit-status
```

预期：`success`（参考基线：219 成功 4m50s、220 成功 5m29s）。

- [ ] **Step 4: artifact 核验（一次性全查）**

```bash
cd /tmp/opencode && rm -rf artifact221 && mkdir artifact221 && cd artifact221
gh run download <run_id> --repo PrestarLin/gbl_root_canoe -n magisk_module
grep '^version' module/module.prop            # 6.2.221
unzip -l *.zip | grep -E "SfbKernelBoot.efi|BOOTENTRIES|BDS.efi"
md5sum <解出的 4 份 BOOTENTRIES>               # 必须一致且含 $Kernel 行
stat -c %s module/BDS.efi                     # 与本地 build/BDS.efi 尺寸比对（md5 不可比：嵌入时间戳）
```

再下载 3 个 toolkit artifact，各 zip 内断言含 `SfbKernelBoot.efi` 与 `BOOTENTRIES`（含 `$Kernel:` 行）。

预期全过 → 任务完成，向用户汇报提交清单 + CI 结果。

## 与 spec §6 的偏差说明

spec §6 将 Commit A（代码+打包）合为一笔；本计划按用户"完成一个提交一次"指令细分为 A1（Task 1）/A2（Task 2），B 细分为 pick×2 + 审计清理（条件），内容、顺序与"bump 压轴"约束完全不变。
