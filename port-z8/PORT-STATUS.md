# Fenestra → Z8 C3.11 移植进度

工作树 `third-party-20261029/fenestra-z8`，分支 `z8-c311`，基于 Fenestra 6777484。本分支是 Fenestra 的派生作品，许可证仍为 CC BY-NC-SA 4.0，并保留摄影商用例外，具体适用范围见 [LICENSE-Z8.md](../LICENSE-Z8.md)；没有改成其他工作树的 GPL 许可。**只在 QEMU 里运行过，没有刷机。候选 MAIN 不能用于刷机。** 下文是 2026-10-30 移植记录；源码发布不代表重新执行了这些运行或完成实机验收。

公开脚本不再固定本机用户名路径：构建时设置 `FENESTRA_Z8_MAIN` 或
`FENESTRA_Z9_MAIN` 指向自己的原始输入。实验启动器另需
`FENESTRA_NIKON_WORKSPACE`，依赖独立安装的研究环境；可用
`FENESTRA_QEMU_STATE`、`FENESTRA_PYTHON` 和 `FENESTRA_QEMU_LOCK`
覆盖相应位置。这些实验辅助脚本不会随源码附带固件、介质、工具链或许可证。

## 结果（2026-10-30）

正式构建 `build/z8-core/z8-core.main`：SHA `9fc5566b8105b866f68ed2b8a01b6a607d607c93b4ac2c48d59931bdcfccf652`，核心 255,528 B，放在零区 `0x401a0000` 起。构建时"还原补丁后等于原 MAIN"的校验通过。

运行 `fen-20`（qemu-i6i-he03-01，hello.wlm 放在卡上）：

| 阶段 | 结果 |
|---|---|
| 启动 | exit 0，0 个 Z9_UNKNOWN，系统事件 19 触发安装 |
| 菜单注入 | Setup 顶部出现 Fenestra、WonderLantern，共 46 行，原厂行正常 |
| 后台任务和读卡 | 任务循环运行；配置文件不存在时状态为 Ready |
| 激活 | OK 键触发，写 flag 并读回，状态 Active，无错误 |
| 模块 | `hello` 列在 Modules 里（On）；WonderLantern 里打开 Hello module 页面，Press 可选中、可按，按后无错误 |

运行结果在 WSL `~/.local/share/nikon-z8-qemu/fenestra-z8/`；截图在 `shots/`。

运行 `fen-21`、`fen-22`（demo.wlm，同一个正式构建）：

- 从 WonderLantern 打开 demo，字符雨动画正常绘制，走的是 `camera.display.scene` 和 OpenVG 路径。
- 相邻截图内容区有 64,000–76,000 个像素变化（共 536,900 个），说明画面在动。
- 按 Back 后回到菜单，状态仍为 Active，错误 0，0 个 Z9_UNKNOWN。
- 这只说明 QEMU 的显示模型上绘制正确，不代表实机 GPU 上也一样，帧率也没有测。

运行 `fen-23`、`fen-24`（探点版，hello.wlm），关机和异常关机保护：

- 激活后空闲：固件发出事件 7，启动状态转为 STOPPED，后台任务停止，0 个 Z9_UNKNOWN。
- 卡上 `BOOT-Z8.FLAG`：未关机的 `fen-19` 是活动文件，`fen-23` 已标记删除。
- `fen-23` 的 exit=1 是探针 900 秒总时限到期（等待时间拉长所致），当时客机已停机。
- 把 `fen-19` 的真实 flag 放回卡上启动（`fen-24`）：状态 ABNORMAL，按 Activate 不激活。
- 执行 Clear abnormal flag 后回到 READY，再按 Activate 进入 RUNNING。

## 没有验证的

- 按下 Press 之后模块是否真的收到事件：hello 只在内部计数，界面上看不到。只确认了没有报错。
- 按电源开关关机：只测了空闲待机触发的事件 7，开关路径是否相同未确认。
- 工作中的模块和卡正在写入时关机；关机测试只带 hello 模块（探点版），模块退出的时序没有单独验证。
- 断电：异常关机是用真实残留 flag 模拟的，没有模拟写入中途断电。
- 带保存配置的第二次开机、Auto activate。
- USB/PTP 调试：PTP 地址已映射，未运行。
- 字体钩子只确认了不影响运行。
- Z8 更新包容器：没有做。`fenestra.py`、`repack.py`、`verify_platform_build.py`、`verify_native_namespace.py` 还不认识 z8-c311。
- 实机。QEMU 和实机是两回事。

## 和 Z9 版的差异

- 去掉了 USB Linux 桥（`usb_debug.c` 换成空桩，PTP 不再声明此能力）：所需共享内存窗口在 Z8 上未确认。
- 共享代码只改一处：`src/native_font.c` 的语言数上限 32→36。

| 项 | Z9 C5.32 | Z8 C3.11 |
|---|---|---|
| Setup 页 / 行数 | 638 / 45 | 663 / 44 |
| 原生文本数 / 语言数 | 3204 / 29 | 3136 / 36 |
| 页号上限 | 0x6b8 | 0x68c |
| 导航事件 | 0x290 起 | 整体 -0x12；终止事件 0x3ca→0x3bb |
| 导航表 | 0x43014bb8 | 0x43bff8e8 |
| 编辑器模板页 | 678 | 704 |
| 控件 ID | 0x4bec/ed/f1 | 0x4ba7/a8/ac |
| 后备字体 | 0x42271338 | 0x42d93856（170,765 B，内容相同） |

## 工具（本目录）

- `gen_z8.py`：从 Z9 平台层生成 Z8 平台层，并给共享脚本加 z8-c311 条目。遇到未知或低可信地址直接中止。重跑前要先 `git checkout` 它改过的 4 个脚本。
- `addrmap.py` / `collect.py` / `z8-final-map.txt`：地址映射工具和结果（252 个）。
- `findloops.py`、`nav*.py`、`consts.py`、`textcount.py`、`lowsim.py`：结构定位用的分析脚本。
- `build-z8.sh`、`build-hello.sh`、`run-boot.sh`、`fenestra_boot.py`、`exfat_put.py`：构建、运行、预置卡文件。
- `diagbuild.py`、`diagcap.py`：临时探点构建，结束后自动还原源码。

按键注意：从标签栏按第一下 `right` 只是把焦点移进列表，第二下才进入页面；执行动作用 `ok`（解码值 0x8000）。
