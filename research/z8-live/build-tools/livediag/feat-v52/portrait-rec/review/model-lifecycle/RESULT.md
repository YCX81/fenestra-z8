# 第二次 HEADER：Wave5 upstream model 生命周期独立检查

目前没有证据表明 host codec/header 状态在 DESTROY 后残留。实际 Eclose 整机日志的第二轮也没有向模型提交 HEADER 对应的 `ENC_PIC(0x100)`。因此不能把该轮失败直接归因于 x265 或模型 HEADER 分支；需要继续查原厂提交前的返回路径。

## 整机日志证据

输入 `/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/v52e-hevc-close`，QEMU SHA `5ccbfc7e051a78dd24176aec98b3ab767709bc078dd3e179923c74b2f50a2ac2`。运行的是 `Z8_W5P` upstream model（不是旧 `WlWave5`）。

第二轮原始 stderr：92649 INIT1；92653 CREATE8；92655/92657 SET_PARAM200（option1/0）；92659 QUERY4000/2；92661 SET_FB80。全部有正常 COMPLETE，最后92662。此后直到日志末尾没有 `COMMAND cmd=100`。完整按行号截取在 evidence.json。

## 实际模型源

`merged/work/qemu-lvfix-src/hw/arm/nikon-z8-wave5-port.h` 与 qemu-10.2.4 副本 SHA 相同：`f2071ad69784c4bd4e34491c0867667ee02cabf90f0ecda7f9495cb380f6b61c`。

- 464～481：CREATE 检查尚未创建，设置独立的 work/ring/codec_mode/created。
- 525～554：只有 ENC_PIC0x100 才会首次 open host codec，并调用 headers。故本轮第二次 host open/headers 实际尚未发生。
- 645～649：DESTROY0x20 先 close host codec，再 memset 整个 instance，清除 native_end_written/native_drained/header_lengths/result/ring等全部状态。
- 664～685：原厂7位reset bundle只有在没有busy/job/pending/instance时才允许；完成时清 registers/FIO/boot等，且不伪造任何完成。

## 独立运行

私有 `/path/to/user/fxfix/v52-portrait-lifecycle`，复制上述实际头文件，复用仓库 W5P_UNIT_TEST 的 RAM/IRQ/timer shim，未修改共享源码或任何运行中 QEMU binary。新增 `lifecycle.c`：

- 1024×704，30000/1001，真 x265/x264。
- 连续3轮 HEVC＋3轮 AVC；每轮 INIT→CREATE→SET_PARAM×2→QUERY→SET_FB→HEADER→picture→EOS→drain→DESTROY→7位assert/reset/release。
- 每轮只通过模型公开命令/MMIO/reset入口驱动，未在轮次间直接 memset 模型实例；确认模型 DESTROY 自行清零。
- 第二、三轮 HEADER 与第一轮长度及 CRC 完全一致：HEVC88B/9411d450，AVC40B/e9e56256。
- ASan/UBSan 6轮全部 PASS；结果和 stderr 已归档。

这证明此显式序列不会复现 host header 生命周期残留。测试不执行 guest ARM/RTOS/完整地址拓扑或原始微码，也不排除模型其他条件的缺口。根因仍应以实际原厂返回分支继续追踪，未增加完成事件或放宽任何断言。
