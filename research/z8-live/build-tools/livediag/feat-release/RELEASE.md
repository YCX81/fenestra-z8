用中文回答。制作 Z8 Live 正式版 v36 的源码（只改本目录，不连 USB、不刷写、不碰相机、不跑 QEMU）。
材料：src-fix32/（v32 Live 源码，已在相机上用过：含竖屏回放、音频增益、切档不关编码器等）、pbfix/live-playback-ui-icon/（回放层）、w5in/v32_patch.py、w5in/chain.sh、relive_w5.py（实验室构建脚本副本，仅参考）。图标：../feat-icon/（用户选定第三版，icon32_generate.py、icon32_patch.py、README-icon32.md，README 说明按 src-fix31+v32_patch 的源码打补丁）。
根因（已在真机证实，见 ~/.claude 记忆外的 ../feat-rec/runs/v8b-4996/report.txt、../feat-rec/FIX17-RESULT.raw、../feat-rec/runs/v9b/）：struct live_w5 位于 Wave5 DMA pool 块（非缓存 0x8xxxxxxxx 别名），对其成员的 CAS（LDAXR/STLXR）在真机上永不成功、死循环，导致 REC 卡死重启。v9b 把 diag_n/close_lock 移到 struct live_runtime（可缓存）后 REC 正常、录像文件正常。
要求：
1. 生成 src-fix36/（从 src-fix32 复制）与 pb-fix36/（从 pbfix/live-playback-ui-icon 复制），在其中应用 v9b 同等修复：close_lock（及任何其他对 pool/DMA/非缓存内存成员的 CAS/fetch_*/exchange）移到可缓存的 struct live_runtime；参考 ../feat-rec/runs/v9b/v8b-source.diff 的做法，但不要带入任何记录器/灯码/诊断代码（recorder、fr_*、lamp、W5F/W5L 文件、D/P/I 码）。struct live_runtime 大小如需变化，说明对 UI/arm 工具的影响。
2. 全面审计 src-fix36 与 pb-fix36 中所有原子 RMW（runtime.c、capture/live_stream.c、capture/live_capture.c、ui/fx_registry.h、pb 层等）：逐个列出对象所在内存（堆/静态/pool/外设），凡位于 pool/DMA/非缓存内存的都改到可缓存内存或改成单写者无 RMW 设计；用 clang --target=aarch64-none-elf 编译对象并列出每条 LDAXR/STLXR/LDXR/STXR 的基址来源。
3. 按 ../feat-icon/README-icon32.md 应用第三版图标到 src-fix36 和 pb-fix36（拍照与回放），保持 geometry.json 尺寸。
4. 写 RELEASE-v36.md：改动清单、与 v32 的 diff 摘要、构建命令（实验室：relive_w5.py src-fix36 … → PB_SRC=pb-fix36 chain.sh …）、需要真机回归的项目（Live 拍照/竖屏回放/音频、切录像档 REC 开始与停止、回放刚录的 MOV、录像后回照片模式继续 Live、多次往返切换、开机后马上插 USB）。
5. 用 clang 编译所有单元确认无警告错误（-Werror，参数参照 relive_w5.py），输出 src/pb 相对 v32 的完整 diff 文件。
