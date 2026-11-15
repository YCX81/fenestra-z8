Nikon Z8 v51 离线集成测试候选（C3.11 / 04.06.p0）
MAIN SHA-256：aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433
这是测试候选，不是已经真机验收的固件。请先看本包 verification.json 的实际结果与限制。

合入内容：
1. 保留v49字体恢复、Preparing、v47图标/写卡优化、v48取景器Live，以及已有双卡/SD配置/C64/NFP/FENESTRA。
2. 合并portrait-photo-layout提交8357634600b1a31a9a74fc0c6d0db704a37a9f9b：两张表共548个矩形及直方图修正。
3. 原厂播放器/录像接手Wave5时停止新Live任务，让worker释放主要Live stream缓冲并确认；ACK等待最多1.5秒，超时诊断后继续原厂。Wave5持久分配仍保留。原厂持有期间暂停，拍照取景收到新帧后恢复预录。真机让路效果未验证。
4. 支持1024×682、1280×852等HEVC回放资源方案及编码高度对齐。
5. 录音增益修复：此前把双声道帧数当单样本数，只放大每块前半；现在全部双声道样本统一饱和4倍，消除该0.170667秒周期增益错误。真机音质尚未听测。
6. 新MOV writer：niko/AUD/GOP块/stco、48kHz双声道24位PCM、动态Nikon NCDT19字段及4张本素材4:2:2缩略图。保留Apple UUID和时间标记轨；时间标记放moov最后，兼容无声/录音读取失败回退。Live REC预览的420 JPEG与旧版逐字节一致，LVJB音频兼容旧LVJA。

使用与证据边界：
- 文件仍在原来的LIVE目录；没有新增DCF登记/原厂相册入口。REC键仍使用Live预览。新writer让文件具备原厂解析需要的结构，不等于改变REC键播放器。
- 原厂Apple meta handler窄补丁仅接受meta，其他未知handler保持拒绝。
- NCTG为真实帧数和编码配置30000/1001，MOV stts保留实际VFR时间。QEMU音频输出none，不证明真机出声或声画同步。
- v50/v51既有回放恢复测试出现过原厂Error与取景IRQ停止，也有完整通过；基线也曾在菜单阶段遇原厂错误。最终轮结果和原始对照均保留在verification.json，单次通过不能说明长期稳定。
- 竖屏表/指令执行验证通过；完整竖屏视频UI受模型未实现接口阻塞，未冒充屏幕验收通过。
- Live开麦时真机快门声仍未修复；没有强行绕过原厂声音状态判断。
- 没有连接相机、上卡或刷机。未完成上述真实硬件验证。

构建：v51d，完整Live与playback重链接；最终MAIN由v50与严格old/new-byte差分合成。
本包每个文件的SHA-256见SHA256SUMS.txt；firmware容器CRC及MAIN以外不变性已验证。

本候选最终检查：Live完整运行exit0，编码911帧，写出1组JPG/MOV/MAN、事务错误0；实际MOV77帧/3.298696秒，ffmpeg全片解码及原厂ARM parser通过。REC预览38帧，但回到取景的断言失败，原厂0x40/A2在REC启动之前已经记录。实际MOV原样复制到虚拟卡，原厂解出第1帧后在未建模显示寄存器0x15421104停止；4K替换fixture完整95帧并11次LPCM提交。不能把4Kfixture或首帧当作完整原尺寸/真机回放通过。
