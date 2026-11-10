用中文做只读对抗性复审，不要修改文件；禁止 USB、刷写、操作相机、QEMU。FIX14.md 是要求，FIX14-RESULT.raw 是实现方自述。审查 v8：src-fix35/recorder_patch.py、src-fix35/recorder/、recorder-build/（linked-lamp-v8-ready），基线 bg34.main。v4–v7 复审记录在 REVIEW7/8/9/10/11-RESULT.*。verify_link CFG 不完整是已知项。
重点（用对象反汇编或反例核实）：
1. 不开机/死机风险：hook 重放、保存恢复、未 armed 门控仍为 111 指令/32 字节；删除 D 发码后关闭路径里不再有 DELAY/GPIO；RAM 进度写入不会越界、不会在非会话调用中写。
2. worker 报告：只在会话 claimed 且 I 发完后发码；与 I 的灯所有权互斥；在 worker 上下文调用 GPIO/DELAY 的安全性；报告期间不持任何 Live 锁，不影响 worker 原有工作（照片模式下 Live 正常拍摄不受影响，未武装时完全不发）。
3. 编码与解码器一致；进度阶段表覆盖 D1 之后每一步，能把“停在哪一步”分清。
只列有证据的缺陷并标严重度，最后给判决：接受/不接受上机。
