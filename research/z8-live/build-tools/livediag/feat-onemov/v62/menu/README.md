# V62 菜单文字调查与补丁（离线）

照片中的空白项是视频拍摄菜单的 `Log设定`；子页应包含 `N-Log / BT.2020`、`S-Log3 / S-Gamut3.Cine`、`Log3G10 / REDWideGamutRGB`。中文、英文及全部 36 种语言的原文字都存在于 MAIN，问题发生在文字编号分派层，不是资源缺失。

## 当前采用的上下文补丁

使用 **`patch_menu_context.py`**，不要把旧 `patch_menu_fallback.py` 的 12 字节方案再叠加。输入是还没改菜单的 pre-menu composed MAIN；脚本不重算 build ID。

```sh
python3 patch_menu_context.py --input PRE_MENU_MAIN --expected-sha256 INPUT_SHA256 --build-manifest BUILD/build.json --output menu.main --receipt menu-patch.json
```

改动只在两个代码窗口：

- `0x401CD410`：把原 `state()` 调用改为调用 helper，一条 BL。
- `0x401FCF40`：80 字节 helper，位于 Live 主区已知尾部预留空间。预留上限 176 字节，未使用 shutter 尾洞。

helper 只对冲突编号 **3180–3224** 读取实际原厂菜单栈。当前页为 group 2 且 ID 在 `[0x7000,0x9500)` 时保留真实 Fenestra State，执行原有动态文字分派；原厂页面、空栈和非 group 2 页面返回 NULL，使原 dispatcher 走原厂语言表。

因此曾打开 Modules 管理页也不会让原厂 Log 标签再次变空，而实际 Fenestra 页面中的有效非空、有效空以及未命中仍完全保持原行为。固定 Fenestra 区 3154–3179、模块区 >=3254 和其他文字编号均不改。

脚本强制绑定输入 SHA，守卫原 dispatcher、当前页 getter、栈顶、导航顺序的代码窗口，以及全部 36 种语言的表布局和 1620 条 NFP 文本。要求 `build.json` 的 `live_payload_bytes` 对应末尾不超过 `0x401FCF40`，`live_free` 一致，而且 payload 末尾至 helper 预留区末尾全部为零。输出只能在上述两个窗口不同；build ID 由主构建流程最后单独更新。

## 根因及为什么旧 12 字节方案不够

Fenestra dispatcher 把 3180–3253 作为 legacy UI 文字区，与 NFP 原有的 3180–3224 共 45 个编号重叠。UI 没有命中时，原逻辑清空并返回，吞掉 NFP 原文字。

另一个必要情形是常驻管理 UI：`manage_modules()` 首次打开 Modules 页才分配 `s->management`，关闭页面不释放。实际 `module_ui(-1)`（0x401CD014）返回该 UI，不检查当前原厂页面。UI 自身 text_base 为 3180，即使在原厂 Log 页，同编号也可能成为有效本地槽并返回空格。

已执行实际 ARM 代码复现：management UI 存在时，3217–3221 各返回一个空格。只在“未命中”时回 native 的旧 12 字节补丁无法修复此情况；而把该补丁留在真实 Fenestra 页还会将本应空的未命中槽变成 NFP 文字。因此最终只采用上下文 gate，`0x401CD4C8` 的原未命中空分支保持逐字节不变。

早期 `patch_menu_fallback.py`、`test_menu_arm.py`、`final-arm-results.json` 是调查阶段产物，不能作为当前最终方案验收；`context-*.json` 和随后绑定最终 MAIN 的 context receipt 才适用于当前补丁。

## 当前页与原厂导航证据

原厂 `current_page` 为 `0x403DBDB4`，读取 `0xA032B5160` 的菜单栈，经 `0x403E9A90` 取栈顶 FxNativePage。identity 低 16 位为 group，高 16 位为 page ID。Fenestra 扩展页区为 group 2 / `[0x7000,0x9500)`，Modules 管理页从 `0x704A` 起。

普通导航在 `0x403ADAA8` 调用 `0x403DBBF4`；后者在 `0x403DBC74` 将目标页推入上述栈（已存在则调整栈），其后才到 `0x403ADAFC` / `0x403ADBF0` 等调用 `0x403AE314` 创建页面图层和布局，再由 `0x403B168C` 打开页面。该顺序支持正常进入/返回期间用当前页区分文字归属。空栈函数返回 NULL。

Log 子页 page ID 为 **0xB5（181）**、group 2。page record 位于 `0x44D640A0`，identity=`0x00B50002`，descriptor=`0x40B10718`；标题 3218，三行文字 3219–3221、row ID `0x35C0`–`0x35C2`。视频菜单的入口行位于 `0x40A30108`，row ID `0x2E38`、label 3218。

## ARM 验证与边界

`test_menu_context_arm.py` 执行实际 MAIN 中的 helper、原厂 current_page 和 stack-top、dispatcher、`fx_native_ui_text` 和原厂语言表。当前页**不是函数桩**；测试建立真实栈布局供原厂指令读取。State getter、UI getter、owner text provider 为明确的服务桩。

114164 项断言通过：全部 36 语言、45 个冲突编号、9 种原厂/空栈场景、4 种扩展页边界、management 常驻及有效空/非空/本地空格/未命中，另含冲突范围外回归、输入 SHA/空间重叠/非零预留拒绝。每次调用检查栈与 x19–x29 恢复。

最终 `final-context-arm-results.json` 已完整重跑并绑定 MAIN SHA **`36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5`**，114164 项全部通过。输入 pre-menu 为 `c65a310dd7401cd5da633525082e73fc3ab6fb8aac9754a12860f50949930d8e`。测试支持 `--expected-candidate-sha256` 和 `--allow-build-id-update`，只允许已绑定最终文件在 `0x401FE060` 四字节 build ID 与菜单补丁结果不同；本次 ID 为 `0xFB84A7ED`。早期 `context-arm-results.json` 是整合前候选验证，已被该最终记录取代。

可读结果在 JSON 的 `resolved_log_labels_resident_UI_on_native_page`：中文标题 `Log设定`，英文标题 `Log settings`；三项分别为 `N-Log / BT.2020`、`S-Log3 / S-Gamut3.Cine`、`Log3G10 / REDWideGamutRGB`。

未操作相机，未进行菜单像素视觉验收。旧 QEMU 菜单工具存在，但 v49 曾无法进入 NFP Log 子页，本轮未为截图扩大模型或改变音频 guest 的菜单状态。
