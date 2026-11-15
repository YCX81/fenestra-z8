# 私有 16 位采集输入模型

本模型只补明确的原厂 RX 描述符组合，不改 F 固件、不改旧 32 位输入入口，不作声学或物理串行采样保证。补丁 `rx16.patch` 基于实际 rdw build source SHA-256 `87c1d8f8594b76068429af6b533d037d0e058dbe75a04913f7629183287f1f41`。

F 整机被动观察到 `40c16674` 收到 request25/26、每块9600字节、source `0e200100`、两不重叠目标（初始 `cf0fe400`/`cf100980`）。原厂 ARM `40c18160` 以 serial bit0=0 选 FIFO100，bit0=1 选 FIFO138；`40c7e4ac→40c17dac/17e5c` 按9600字节步进5块ring，IRQ producer `40c7e558` 发event8/1并重装对应bank；stopflag后不发事件或重装。`native-rx.json` 实际执行两种宽度共20次原始构造/续交，DMA安装及eventpost是明确测试边界。相关原厂指令段在最终F与G逐字节相同。

输入契约是显式 LE16 stereo（4800个int16=2400双声道帧），与实际后端 `LAZ8_BLOCK_BYTES=9600`、`LAUDIO_S16LE` 消费一致。它不声称已经测量硅片串行线上的物理左右声道/位边沿。模型按输入字节写入原厂映射目的地，随后调用原有 `pdma_progress`；guest 原始 ISR/任务负责回调、ACK及下一块，禁止模型直接调用固件或写 Ready。

独立入口 `serial5-pcm16-input` 为 `phase:local:960:<19200个hex字符>`。只允许channel0、bit0=0/RX enable、FIFO100、request25+phase、2400words、既有DMA控制字段、唯一有效bank、对所有已武装RX目标无重叠；错误相位、长度、格式、地址、停止后请求均拒绝。状态 `serial5-pcm16-state` 给出实际bank/目的地/块数。

`gate-tests.json`：22组ASan/UBSan检查通过，覆盖width/FIFO/count/control/stride/地址对齐/重叠/重复bank/未enable/完成状态/重复提交/停止/短输入/非法hex/DMA写失败；两bank成功各写精确9600字节，前后canary不变，失败不发新completion。完整固件IRQ及PCM Ready→快门暂停→保存仍待整机验证，不能以此host测试替代。
