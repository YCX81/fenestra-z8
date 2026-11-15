# Z8 v50 → v51 集成进度（2026-11-14）

## 本轮最终交接：v51d 集成完成，修正 QEMU 配置后核心回归通过

- 唯一最终集成源：`integration-writer/v51d-work`（不是 writer/nctg-work 的整树，后者只提供6个模块文件）。MAIN `aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433`，本地 `integration-writer/v51d/q/main.main`，远端 `/path/to/user/fxfix/v51d/q`，完整build `/path/to/user/v37build/v51d`。
- 集成 v49 全部内容、8357634 竖屏布局/直方图、Wave5让路/释放ACK与新帧恢复、尺寸补丁、录音半块增益修复、最终Nikon兼容MOV writer。最终6源hash、完整MAIN/manifest/portrait/fonts/runtime审计均通过。Live108136B/PB35272B，未与401fd000附近caves重叠。
- 最终Live QEMU exit0：编码911、tx1/error0，实际MOV912118B /77帧/1024×682/3.298696秒，SHA `5bbc309ce4363e4d0040a2de0add71ddafca0d386cdf3324b7c298979da006d7`；JPG/MOV/MAN配对标记一致，ffmpeg全片与原厂ARM parser通过。无真实麦克风音轨。
- 最终REC QEMU：同一固件开启既有 `z8-heif-secondary-stop-model=on` 后，拍摄、45帧预览、半按返回取景及编码恢复全部通过；IRQ1582→2078，编码413→659（最终707），事务1/错误0，原厂错误历史为空。功能配置仅增加该选项，另增加被动观测；固件、输入、deadline及断言未改。固定重跑入口与完整对照见 `playback/A2-MODEL-AB.md`、`playback/v51d-core-regression-final.json`。
- 模型证据边界：实际40040 AND等待及主路40/次路40000通知发生在菜单step046→047；后续回放等待mask40也成功。旧失败轮没有直接采集失败mask，不能倒推它必为40040，或把单次通过称为所有历史A2唯一根因。旧v51d恢复失败、v50成功/失败及v49菜单A2记录均保留。
- 最终native：最终C writer有声容器替换已知4K视频的fixture完整95帧/11LPCM/unknown0；原尺寸host无声片和上述实际MOV都被原厂解析、注册buffer并解出首帧，随后在既有显示模型缺口 `0x15421104` 停止。没有放宽unknown断言、没有冒充完整原尺寸回放通过。`writer/parser/final-native-results.json`。
- Native driver的 `firmware_sha256` 是wrapper的stock占位值，报告单独保留该字段并以QEMU命令loader路径+独立MAIN文件sha核对实际候选；不是把stock hash称为v51d。所有候选文件在各自运行期间保持不变。
- 当前交付包：`/path/to/Nikon/delivery/Z8-v51-Portrait-LiveMOV-aa6e94e0.zip`，解包目录同名。固件bin SHA `c30031834f34ee7c25646502e775293aed99ecfd6358fae09e398a02e1ccf280`；ZIP SHA `270c1098273b44ca97e4903e3fea65fd2e5191432d27bb95ed1bb5e534025676`。已下载并核对7个文件、ZIP CRC及全部清单SHA；收据 `delivery-receipt-v51-qemu.json`。容器MAIN以外不变性检查已通过。
- 当前包 `release_status=qemu_test_candidate`、`core_regression_passed=true`、`physical_camera_verified=false`。旧 `Z8-v51-Lab-Portrait-LiveMOV-aa6e94e0.zip` 保留旧配置失败记录，固件二进制与当前包相同；相邻STATUS指出当前交付包。v50旧包仍不作为当前交付。
- 未解决：Live开麦时真机快门声。真机Wave5争用/恢复、真实音质与同步、完整原尺寸屏幕回放和完整竖屏UI仍未验收；原尺寸播放停在既有模型缺口。自己启动的QEMU均已结束，原有旧font进程80617未动。
- 后续上卡（2026-11-14 15:11 +08）：用户将SD卡插入Mac并明确要求复制固件和测试片。已向 `/Volumes/NIKON Z 8  `（exFAT，disk4s1）根目录写入上述v51 BIN，向V51TEST放完整ZIP/测试说明/复制收据，向DCIM/108NCZ_8新增DSC_9041～9044.MOV。四片分别为已知4K对照、新writer+4K有声fixture、新writer原尺寸1024×682有声host片、实际v51 QEMU写出的77帧无声片。所有目标回读SHA通过，已有三项插件与包相同无需改动，原照片/索引/Live设置保留，已sync。旧BIN（59700d88…）和五个FENESTRA文件备份至 `sd-card-20261114/backup-before-v51`；完整路径、哈希与结果见 `sd-card-20261114/receipt.json`。包内冻结verification记录打包时尚未上卡的状态，本次收据是后续复制记录。尚未刷机或真机验证；SD卡仍挂载，取出前需安全弹出。

以下为过程记录，早期候选及阶段结论由上述最终状态取代。

用户要求延续 v46r/v49 工作、合并 portrait-photo-layout 8357634，多 Agent 与 QEMU 并行；本次只操作本地/远端构建和虚拟卡，没有连接相机、上卡、刷机。

## 已构建候选

- 最终候选 MAIN SHA-256: `4cb5235fc709220f64af5400680653999d2a03b0d5a4821975e0f6a7b2d50977`。
- 本地 `integration/q/main.main`；远端 `win-lab` / WSL `Ubuntu-26.04` 的 `/path/to/user/fxfix/v50/q/main.main`，相邻 `changed-ranges.json`。
- 实际完整构建 `/path/to/user/v37build/v50b`（**不是**早期 `v50` 目录；后者为过时交接草稿且依赖不全而未构建完）。v50b 源与 ELF 是最终的，main SHA `de78b3a28609a1539461ac0bdf9be9f644c277935ad1d605425f967e74e19c2a`。
- 基于 v49 MAIN `63240425765acad54030e58f2dad83b2f4679a9e511a7e5b03e2c4ba1a28718a`；strict old/new-byte splice，拼入视频方案与高度补丁，竖屏274控件x2表与124字节直方图补丁，最后重打 bootguard ID。
- `integration/compose.py` 可重放。v50src 完整树在远端 `/path/to/user/v50src`，构建使用其 `.venv/bin/python` 和 clang-21。

## 已修复并测试

- 录音块增益只处理一半双声道样本：真实读取回调返回 frame 数，旧循环误当 sample 数，8192-frame / 0.170667s 周期精确复现。修正 `*got*LIVE_AUDIO_CHANNELS`。ASan/UBSan 通过，旧版对照失败；最终 ELF120字节核对通过。`audio/RESULT.md`。
- 原厂 Wave5 交接：先 hold，再 preempt；worker 真正释放主要 stream block 后原子 ACK；最多等待1.5s ACK（不含原先 preempt 用时）。hold期间不重新申请，VDI仍被原厂持有就延后恢复epoch，原厂释放并恢复photo LV后再解除。17项摘取真实函数的状态/竞态测试通过。
- 不能写“所有内存都释放”：Wave5 持久分配仍保留；只有主要 stream block，沿用切录像模式的释放路径。1.5s超时会记日志且继续原厂，不应声称保证释放成功。
- 竖屏 ARM64 1024组对照执行通过，548矩形写入严格校验，保留原表属性及插件gate。
- 尺寸 cave 19组 ARM64执行通过，包含1024×682、1280×852、coded856/864与原厂回退。

## QEMU（最终结果须等落盘）

- playback Agent 管理 `/path/to/user/fxfix/v50-playback`：v46/v46r并行Live拍摄+预览对照，最终v50 Live→REC预览及原厂4K。4K最终退出0/95帧/未知MMIO0；仍启用了既有1543缩放器容忍，所以不证明Live实际占用时真机让路；声音输出none。
- audio_shutter Agent 管理 `/path/to/user/fxfix/v50-audio`：sound On成功，encoded925、transaction1，原厂声音id1/id2，实际MOV/JPG导出。QEMU无真实音轨，不证明扬声器/麦克风。Off首轮菜单退出无拍摄，正在独立retry。
- portrait Agent 管理 `/path/to/user/fxfix/v50-portrait`：final a1遇旧模型视频接口缺口；a2使用已有mvq模型继续，截图待审。上游直方图曲线为合成统计模型，不能冒充真机。

## MOV writer 实验（未合入 v50）

- portrait Agent 在 `writer/` 开发 niko / AUD / GOP chunks / lpcm24+24024帧 / stco / 动态NCDT / LVJB预览兼容。
- root 写 `meta_handler_patch.py`：只让 `meta` handler使用原厂kind4跳过，其他unknown保留错误6。24B cave `0x401fd200`，BL hook `0x40cea514`。原厂解析器QEMU virt四组通过，含保留Apple metadata和完整NCDT文件，见 `parser/parser-test.json`。
- 单独 full-native 试验 `/path/to/user/fxfix/v50-writer/native-meta` 由root管理：MAIN=最终v50+窄meta hook（SHA `1fa66a50e773d9873fa937f971566c602f222040f3e4fb397a2e0bd59f4e4967`，未打包），已验证4K文件加回原始Apple UUID/mebx；AV样本逐字节不变。sample SHA `67f17ecf5d9ea35c100c16ceb477a3958ced5ca0a9721743f2a3df8f1fa262e7`。后台PID142320，`native-meta.exit`待查。
- 虚拟卡离线修改：普通文件 sparse partition切片，在原文件已有簇内替换、更新dir checksum，fsck clean并独立extract逐字节相同；无root/设备挂载。

## 尚未完成

- 快门开麦无声：没有足够硬件证据支持安全修复，保留调查；某时序比较器不能误称音频资源判断。
- 最终QEMU全部收尾、汇总、打包与复制delivery。
- 未挂载相机卡；/Volumes只有系统及Antigravity。

## 后续更新：v51 writer 与真实 UI 拒绝门槛

- v50完整REC恢复driver已修复：旧driver等待原厂REC bit，但Live故意消费清0导致旧两对照脚本900s超时。修复仅在consumed_rec实增且44B键状态匹配时放行。v50完整回归exit0，预览45帧，退出后IRQ1580→2074、编码421→667，最终715帧。正式包已复制本地 `delivery/Z8-v50-Portrait-LiveHandoff-AudioGain-4cb5235f` 和zip；固件bin SHA `daae5e523f83075502408345030334d1ad54544e66d77cdce84693b6438c45b3`，全部文件hash/zip校验通过。
- v51最终候选 MAIN `1b0ecc754467c893e37085653f286479f7ea9341d3c10653b28ab1aca93e782b`，远端 `/path/to/user/fxfix/v51/q/main.main`，build `/path/to/user/v37build/v51b`（注意不是初版v51）；root `integration-writer/final-work`。writer最终patch SHA `3f97c87d32d97581b647ae4725549dd2ffa5ebd76fce59c0434bd1718eb45009`。
- v51保持全部v50功能，新增writer niko/AUD/GOP/lpcm24(24024帧块)/stco/NCDT/Apple轨兼容与LVJB旧预览转换。Live payload106560B、PB35272B，meta24B cave401fd200。独立文件/二进制审查通过（playback/candidate-v51-review.json）；PB音频旧LVJA/新LVJB边界、取消、截断测试通过。NCDT取消延迟、末帧JPEG重试导致LVJB间隔问题已修复。
- writer host真实95帧样本 SHA `bbe6254b784507550ae4236747c84744c0670fe0ea5dd47d5ea15757148a572e`；实际C3.11 NCDT4图查询+MOV-open+GOP检查QEMU virt通过。NCVW必须1280×720/1920×1080/3840×2160，原1024×576已纠正。所有图逐block从1MB原scratch缩放，无3MB新帧分配。
- v51真实Live QEMU成功：921编码、tx1/error0；MOV77帧/1024×682/3.294255s/890154B，SHA `8d8aaf38875c97be4eddc13e1311a0739a3069fae84549ab133046631af3682d`，niko/AUD/GOP/NCDT/Apple标记通过、ffmpeg全decode0。无音轨（模型无mic）。证据 `audio/qemu-v51/`。
- v51 REC首轮首帧completion -24/300ms超时，writer未进入；不能写纯菜单flake；相同firmware重跑r2已tx1/error0/编码192并进入预览，等待完整结束。未改firmwaredeadline。
- 原厂整机native-meta（旧可播4K加回Apple metadata +窄meta hook）已exit0/95帧/unknown0，证明保留Apple轨可完整回放。
- **新增阻塞**：新writer host-sample替换已知4K视频的 `writer/parser/HOST_4K.MOV`（SHA `dfc5874a4e20c07467d2584544c88a3f27aeb62d3245b6ac6f050b77d1d39560`）在最终v51整机显示 Cannot display this file，decoded0；低层parser通过不是UI充分条件，暂不打包v51。
- root native试验运行目录 `/path/to/user/fxfix/v50-writer`，regular-file虚拟卡，无mount/root：初始native-writer-final（工具exec session55897）；3个单变量run并行：`run-donor-ncdt`（session95255）、`run-metadata-last`（28772）、`run-no-metadata`（54686）。对应native-variants.json，不应混成生产。portrait Agent正做`native-fields.MOV`（新JPEG+旧82条NCTG改尺寸）/`native-jpegs.MOV`（新7条NCTG+旧JPEG改尺寸）两额外变量，root待制卡/启动。
- portrait的UI方向仍未闭合：actual gate1、stock auto_rotate_setting RAM0xa03d70940=1、sensor class3、UI direction0；模型最后停0x15080014。不再误称开关没开，548矩形写入/机器执行验证成立但视觉未通过。

## 追加门槛（13:29后）

- v51 r2/r3 均拍摄及预览成功，但半按退出后原厂 Error / IRQ 与编码停滞；v50 control r3 相同失败。因此旧v50单次通过不足以证明恢复稳定，交付目录相邻 STATUS.txt 已注明测试候选、当前不推荐刷机。playback Agent继续查原厂错误来源，未放宽断言。
- donor-ncdt 原厂整机通过 exit0/95 decode/11 LPCM/unknown0；metadata-last/no-metadata均0帧拒绝。说明NCDT差异足以解释新writer拒播，轨序不是这轮主因。fields/jpegs/min-fields/core-all正在拆分；actual ARM file-info确认新7字段导致FPS/位深/音频未知；min字段还缺tag0x21格式码3。

## v51c 与后续 writer 修正（13:38）

- v51c构建已完成，MAIN04f530dda16763ffbe02df893f9e9ef65cffe43e0bb8dbf27e15e3230a10b4a8，build/v37.main2d892d29aea547b9e04ff3af983ba5778345e7116efc000461ead32a81216df2，Live107872B。仅加NCDT19字段与专用422 JPEG，普通420预览397359B逐字节不变。源integration-writer/v51c-work；远端fxfix/v51c与v37build/v51c。不是最终候选。
- 格式与JPEG双因素已闭合：fields(旧82字段+新420 JPEG)无法显示；jpegs(新7字段+旧422)能显示缩略图但不启动视频；coreall-422(新当前标量字段+当前图像重新编码422)95帧/11LPCM/unknown0。独立actual ARM JPEG IRQ验证：请求format1遇header2走error0x2800，1/1开始image；不是QEMU不能解420。
- root正跑真实C422样片的4K模型替换fixture native-cwriter-422，SHAff85458cce6688e7034a4399e5dacfaa9cc95cfb13e47a9ba80a024dd8229074，run-cwriter-422 session30722。
- 独立审查发现无音轨实际MOV的meta轨后接udta会在stockparser报6；仅把timed meta放moov最后可使真实片通过。portrait正做writer finish_prefix / NCDT / metadata尾部的流式修正；新候选将用v51d，绝不把v51c声称完整可交付。
- v50/v51无额外W5诊断控制仍同型恢复失败；playback正跑稀疏ErrorInformationService观测，并加原版v49同断言对照以区分v50改动与基线模型。

## 实验包封存后的进一步A2定位

v51d实验包与sha未改。新增只读运行v51d-a2-wait-observer（playback Agent管理）只增加31个稀疏执行点，固件/deadline/恢复断言不变。已观察前两次菜单stop(mode0)的event=a0496efe0/mask1000/timeout96收到通知、返回1，stop0；正在等待同轮进入回放。portrait并行静态证实ISR INTID102/104读W1C15085108/15085508，bit0传入完成通知；模型timer停止边缘是否缺事件尚未证明，禁止伪造完成。

## A2后续实测与配置对照（实验包未改）

- v51d-a2-wait-observer实测排除aux1000等待：真正失败4115fbe4/log30b6，status100经40f5b2a0(LR40f55b60)到A2；之后正式停止路径1000等待返回1/stop0。源在4115faf8的40或40040 AND等待，原始证据qemu-v51d-a2-wait.json。
- 原厂primary global0 bit18→OS40；secondary global1 bit18→OS40000。当前旧driver缺既有z8-heif-secondary-stop-model。它有严格group1已运行→groupstop→source1清除且drained gate，无guestflags/PC/像素改写。旧日志尚不证明所有gate，因此不能预称根因已确证。详见portrait/stop-signal/RESULT.md。
- playback正运行v51d-secondary-stop-model，仅开启同一5ccb...QEMU的既有选项，固件aa6e94e0、deadline、输入、原断言均保持；新增稀疏点读取实际mask/返回/OS通知，final再读4个state。前3次菜单mask40均通过，等待拍摄后的关键回放分支。若有效需更新验证/交付状态，保留旧Lab包和旧失败证据，不能覆盖失败历史。
