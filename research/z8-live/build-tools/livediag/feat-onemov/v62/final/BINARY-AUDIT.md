# V62 最终二进制独立审计

**PASS：相对 V61 的 7,545 个变化字节全部解释，未解释差异为 0。**

- V61 MAIN：`db0c847b333f0a6e07222f2207435e9bf340ce1d544704f6d801ec3ecc1088e5`
- V62 MAIN：`36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5`
- 两者长度均为 81,807,709 B。机器收据为 `binary-audit.json`。

## 差异归属

| 区域 | 变化字节 | 独立核对依据 |
| --- | ---: | --- |
| PB 完整 bank `40196000..4019f020` | 0 | 全区逐字节相同，含未使用尾部；旧/新 PB ELF 也完全相同 |
| Shutter bank `4019f020..4019ff40` | 2,111 | 与相应 ELF `.live_shutter` 及零填充逐字节重建一致 |
| Live 主区 `401e2000..401fd000` | 5,421 | 与 ELF `.text`/`.rodata`、对齐/尾部零填充、单独菜单 helper 重建一致 |
| Live extra `401fd218..401fe000` | 4 | 仅 `401fd9c8` 的 B：原厂 prepare `40f9cccc` → 新 helper `401fb764` |
| Menu state BL `401cd410` | 4 | 原 `state(401cc314)` → `401fcf40` helper |
| SFX dispatch B `40e30e3c` | 1 | 完整指令解码后，旧 `live_shutter_dispatch=4019f5b0` → 新同名符号 `4019f5f4` |
| Build ID `401fe060` | 4 | 清零后 SHA256 低 32 位 OR `01000000`，复算得到 `fb84a7ed` |

这不是把三个大区域直接加入差异白名单。旧/新全部 ELF alloc sections 均与对应 MAIN 的实际字节一致；再对完整 bank 的零填充、间隙和边界逐字节重建。主区新增的菜单 helper 是独立 80 B，位于 `401fcf40`；菜单补丁重新应用只改变它与上述 BL 共 76 个字节，重算 build ID 后完整 MAIN 精确复现。

## 编译及源文件证据

本机 V61/V62 源树均为 406 个文件，仅 `src-fix37/audio/live_shutter_z8.c` 与 `src-fix37/hooks.S` 不同。最终 V62 本机源、上传输入清单、远端实际编译 work 副本逐文件 SHA 相符。

使用原 Clang/LLD 21.1.8，在临时目录重新编译旧/新这两个单元，产出的四个对象与已构建对象完全相同。其余 **29/31 个对象文件逐字节相同**，包括全部 PB、录音后端、runtime、capture、编码器和 mux。旧/新生成的 Live/PB 构建脚本及链接脚本也完全相同；随后使用这些对象重链接旧/新 Live/PB 共四个 ELF，均与交付构建 ELF 完全相同。既有源和构建文件没有被改写。

因此 Live 区域的变化可追溯到这两个受审源码单元及其重定位。`.rodata` 起点后移 328 B；内部仅四个 64 位指针随相同偏移重定位（8 个变化字节），其它内容完全相同。菜单修复是明确的独立二进制补丁，不属于这两个编译单元。

## 保留行为的静态证据

已用实际 ELF 符号核验 **35 个原厂入口 B/BL 和 4 个回调指针**，包括所有 PB、Live、prepare/fire、SFX 和 EVF 入口；另外核对两条固定 crop/aspect 入口及菜单 helper 的原厂调用目标。除上表的 SFX dispatch 重定位和新菜单 BL 外，其它区外入口/回调均保持原字节。

PB 完整 bank SHA 为 `c43c058a99d8845d2e692d547dae2e3254008195610628889cca30e9dab915ff`。crop/aspect 两个汇编区、bootguard/metadata helper 区完全不变。

V60/V61 视频结尾冻结帧规则仍保留：`count > 3 && count % 3 == 1`，且倒数第二帧时间不早于按快门时刻，才去掉尾帧。`capture/live_stream.c`、其目标文件以及最终 `freeze()` 的 **552 B 机器码**（地址 `401f4834`）在 V61/V62 均完全一致。没有调整视频帧选择、编码或容器规则。

复跑本地二进制审计：

```sh
build-tools/livediag/feat-rec/.recorder-venv/bin/python build-tools/livediag/feat-onemov/v62/final/audit_final.py
```

原始编译/重链接收据在 `audit-inputs/remote-object-audit.json`，对应操作脚本为 `audit_remote_build.py`。本审计确认静态来源、边界与保留字节，不替代 QEMU 的实际 PCM/录音恢复验证或相机声学听测；没有操作相机 USB、刷机或实体卡。
