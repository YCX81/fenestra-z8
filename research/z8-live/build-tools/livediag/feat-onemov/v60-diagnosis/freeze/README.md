# V60 freeze 离线边界测试

已执行的宿主测试：1,474,560 例通过，其中 483,840 例触发剪帧，7,560 例由末尾快门保护保留原帧数。ASan/UBSan 无报告。这里仅保存此前已执行的夹具及结果，没有重跑或扩大测试。

在仓库根目录运行：

```sh
sh research/z8-live/build-tools/livediag/feat-onemov/v60-diagnosis/freeze/run.sh
```

需要支持 AddressSanitizer 和 UndefinedBehaviorSanitizer 的 Clang；可用 `CC` 指定编译器。程序在临时目录编译并运行，退出后清理二进制，不修改生产源码。

`test.c` 的 `freeze()` 提取自 v53 integration-g-final 的 `capture/live_stream.c`，在实际成功检查后插入从 `patch_om7.py` AST 读取的生产 `rule` 字符串。未包含 `OM_FORCE` 测试分支。`is_cfr()` 原样提取自同一基线的 `codec/live_mov.c`。保存时检查这些片段确实逐字存在于此前执行过的临时夹具，并确认拷贝字节相同。源文件和提取片段 SHA256 见 `results.json`。

夹具使用最小字段结构体与环形队列读取/拷贝桩，覆盖 1–128 帧、30 个 CFR 网格相位、3 个 ring head，以及每帧快门位置和有效的 +1μs 边界。断言检查窗口仍包含快门、最多减少一帧、字节/指针一致、末帧时长、CFR 和音频样本窗口算术。

这不是完整固件执行：未验证固件 ABI、任务并发、实际音频后端、写卡、回放 UI 或真机行为。末尾快门保护保留 `3k+1` 是现有规则的明确分支，测试不将其误报为失败。
