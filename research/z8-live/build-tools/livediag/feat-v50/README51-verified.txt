Nikon Z8 v51 集成候选（C3.11 / 04.06.p0）
MAIN SHA-256：aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433
固件 Z_8_0406.bin SHA-256：c30031834f34ee7c25646502e775293aed99ecfd6358fae09e398a02e1ccf280

已完成离线与QEMU核心回归，尚未真机验收。本次没有连接相机、上卡或刷机。

合并内容
- v49字体恢复与Preparing、v47图标/写卡优化、v48取景器Live；保留已有双卡、SD配置、C64、NFP、FENESTRA。
- portrait-photo-layout / 8357634600b1a31a9a74fc0c6d0db704a37a9f9b：视频竖屏布局与直方图修复；两表548个矩形与124字节直方图代码。
- 原厂播放器/录像接手Wave5时，Live停止新任务并让worker释放主要stream缓冲，等待ACK最多1.5秒；Wave5持久分配仍保留。原厂持有期间暂停；拍照取景有新帧后恢复预录。超时会记诊断并继续原厂，真机沙漏是否消除仍需验证。
- 1024×682、1280×852等HEVC回放资源方案和编码高度对齐。
- 修复双声道录音只处理每块前半样本导致的0.170667秒增益跳变；所有样本统一饱和4倍。真实音质仍需听测。
- 新MOV writer：niko/AUD/GOP块/stco、48kHz双声道24位PCM、动态NCDT19字段及本素材4张422缩略图；保留Apple UUID和时间标记。无声/部分录音失败回退也兼容原厂解析。时间标记轨位于moov最后。原Live预览420字节保持，LVJB兼容LVJA。

最终测试
- 独立Live完整运行exit0：编码911帧，写出1组JPG/MOV/MAN，事务错误0。实际MOV77帧、1024×682、3.298696秒，ffmpeg全片与原厂ARM parser通过；三个配对标记相同。
- 修正QEMU配置后，完整Live→REC预览→退出恢复通过：预览45帧，IRQ1582→2078，编码413→659（最终707），无原厂A2错误。所有原始断言和固件deadline保留。
- 原配置曾在停止等待失败，且未启用既有z8-heif-secondary-stop-model。只开启该模型的对照，在菜单step046→047确实等待mask40040，收到主路40与次路40000后返回成功，模型严格条件触发1次；后续回放入口mask40也成功。旧失败轮未直接采集实际mask，因此不能倒推它必为双路等待失败，或把本次通过外推为所有历史故障唯一根因。此处改的是测试配置，固件字节、deadline和断言未变，真机时序未证实。
- 最终C容器/音频/元数据保持、替换已知4K视频的模型fixture完整播放95帧、11次LPCM提交。实际原尺寸MOV原样上虚拟卡后解出首帧，停在既有0x15421104显示模型缺口，不能称完整原尺寸屏幕播放已通过。
- 10路ASan事务/回滚、7个实际ARM解析样片、音频与Apple标记、LVJA/LVJB、最终镜像/manifest/源码hash均通过。竖屏1024组指令和字节检查通过；完整竖屏视频UI仍受模型接口阻塞。

仍需注意
- Live开麦时真机快门声仍未修复；没有强行绕过原厂声音状态。
- 音频QEMU输出为none，不证明真机出声或声画同步。NCTG用真实帧数与编码名义FPS，stts保留实际VFR时间，二者在低帧率仿真中可不同。
- MOV仍保存在原LIVE目录，没有新增DCF登记/原厂相册入口；REC键仍使用Live预览。
- 原厂Apple meta兼容依赖本固件的窄handler补丁，其他未知handler仍拒绝。

验证细节及历史对照见verification.json；每个包内文件hash见SHA256SUMS.txt。
旧Z8-v51-Lab包是同固件、旧测试配置的冻结证据；本包更新了验收记录，并未更改固件二进制。

固定重跑入口：/path/to/user/fxfix/v50-playback/launch_validated_recovery.sh UNIQUE_RUN_NAME；driver SHA 1ec568271fd93739a4d92005b56790e765f19a9da6f3a1782e8a84ccb45a6add。依赖当前win-lab QEMU/runtime，脚本先核对MAIN/QEMU/driver哈希。
