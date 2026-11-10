用中文做只读对抗性复审，不要修改文件；禁止 USB、刷写、操作相机、QEMU。范围只限 FIX11.md 改动的竞争者 close_lock 等待逻辑（REVIEW10-RESULT.md 的两项 P2），FIX11-RESULT.raw 是实现方自述。基线 bg34.main，源 src-fix35/、recorder-build/。
用对象反汇编和自建 Unicorn 交错反例核实：硬截止只算一次、循环只看 now<deadline、没有续等分支；时间回绕/now 读取异常时不会永久等待；未 armed 时等价原 4 s；0.2/6.2 s 释放成功；持锁卡死与 REVIEW10 两组交错都在 deadline 内结束。顺带确认改动没有影响 hook 重放、保存恢复、未 armed 门控（111 指令/32 字节）与 D 码归属。
只列有证据的缺陷并标严重度，最后给判决：接受/不接受进入完整链接和 QEMU。
