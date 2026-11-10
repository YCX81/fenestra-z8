用中文回答。真实全量链接已在实验室完成，产物在 feat-rec/linked-recorder/（live-fixed.emulation-only.main、payload.elf、objects/）、recorder-owner/；bg34.main 已确认等于 chain-nfp4v31 的 pool 经 bootguard 后的镜像，bg34.changed-ranges.json 是 chain-nfp4v31 真实清单，仅把 output_sha256 改为 bg34。原来 q34 目录的清单是 q14 模板，符号陈旧，不要用。
现状：链接成功，但 verify_link 拒绝：('live_ui_menu_page','old forwarding proof failed','RET before stock or unknown return', 0x401e9ee4)。
任务：
1. 用 linked-recorder 里的真实对象反汇编判断：这是检查器对合法路径的保守误拒，还是真实错误。列出证据。真实错误则修 recorder 源，并说明。
2. 若是误拒：修 verify_link.py，让真实合法转发能被证明，不得削弱：REVIEW3-RESULT.md 的负例、43 个已有负例及全部 RET 入口攻击必须仍然 FAIL。不允许给 live_ui_menu_page 或任何符号加“跳过”特例；条件早返回的路径须证明是原 wrapper 源码语义（对照 ui/live_ui.c 等）。
3. 对真实产物跑 verify_link 直到得出结论，写 linked-recorder/final-check.json；运行全部测试。不要修改 bg34.main 与 linked-recorder 的镜像（除非第 1 点需要重链，那就说明要在实验室重链）。
4. 另外审查：实际 linked 镜像是 src-fix35（v35）的 Live 源码重新链接在 v31 的播放层/pool 层之上。确认 v35 的 Live 新代码与 bg34 保留层之间没有符号/接口不一致；确认 v33/v34/v35 的旧诊断（RAM 记录、卡灯、闪码）在此镜像中的状态，是否仍会在未 armed 时运行、是否干扰本次判断。
禁止 USB、刷写、操作相机、QEMU。如实列出已验证与未验证项。
