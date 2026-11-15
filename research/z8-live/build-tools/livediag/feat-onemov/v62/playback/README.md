# V62 新拍 MOV 的原生回放验证

使用 V61 的 `run_qemu_jpeg.py` / `run-nb.py` 原生 MOV 回放流程，仅在本目录复制的 driver 增加最终 MAIN SHA、目标新 MOV、实际帧数与末次选中照片的验收。未改生产代码。PB 结构 9664 字节；V61 layout 的 runtime.h SHA 与本轮 V62 完全相同。

所有运行绑定最终 MAIN SHA `36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5`，RDW QEMU SHA `9804d5734610465613f80dc762dd3bef0307571b0b016f140cdec81252ba6123`。

先等待 capture guest 正常退出、导出完成，再用 fuser 确认捕获卡无人打开，复制私有卡供新的 guest 启动。原 capture 卡不修改；driver 收尾核对源卡大小与 mtime 未变。**这是同一保存卡的私有副本重启回放，不是同 boot 拍摄后回放，也不是双卡或相机实测。**

## On-r2 新片：播放/返回/清理通过，完整帧覆盖有尾帧限制

捕获来源：`v62-36c5-tone10-on-r2/capture-card.raw`。本轮实际慢快门 Type B 使用原厂 11/12 两段 SFX；新 MOV SHA `21142258ce8c114d1391f72452657a645990a8a31239665041a5813d04977b19`，92 帧。捕获分析副本为 `capture-on-r2-review.json`。

`on-r2-reboot/summary.json`：原 16 项通路验收和 237 项 native 检查全部通过，runner exit 0。随后严格 CRC 审计发现末尾源帧 91 未输出，因此加入完整源帧覆盖检查后的总体 passed=false，另列通路通过。

- 原生 MOV 日志产生 92 次 picture 事件；逐帧 NV12 CRC 表明它们是首帧重复一次加源帧 0–90，不能说覆盖全部 92 源帧。目标为 `A:\\LIVE\\100NCZ_8\\DSC_0001.MOV`。
- 原生 movie 页 `(3,0x56C)` 进入后回到 photo 页 `(3,0x552)`。
- 桥接 start=1、return=1，nb_stage 从 2→3→0，nb_bit 从 1→0，nb_fail=0。
- 最后选中的 DCF ID 为原照片 `0x03200020`，两次文件打开重定向。
- nb_cleanups 观测点处仍为 0；该观测发生在计数自增之前，所以清理结论使用 stage=0 / bit=0 和返回原照片，而不声称观测到该计数增加。

使用真实 MOV 解码路径和 Wave5 decoder 日志；没有用旧 JPEG preview 证明原生回放。播放器音频输出设为 none，因此本项不证明扬声器播放音质；捕获文件的音视频完整解码与快门 PCM 验证由 capture 记录分别提供。

## Off-r4 新片：完整覆盖通过

Off-r4 新片 93 源帧，94 次 native picture 事件。CRC 逐项证明：事件 0 重复首帧，事件 1–93 覆盖全部源帧 0–92，顺序一致；native 235 项检查、播放/返回/清理和完整帧覆盖均通过。

最终冻结结论见 **RESULT.md** 和 **verification.json**，以 Off-r4 证明完整源帧覆盖；On-r2 的尾帧限制明确保留。未追加第三次回放。
