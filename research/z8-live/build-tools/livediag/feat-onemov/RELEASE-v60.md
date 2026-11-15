# V60 源码检查点（2026-11-15）

本文保留 V60 历史检查点；当前归档入口为 [V62](RELEASE-v62.md)。V60 不是当前最新固件，也未完成整机验收。根目录核心构建路线与此 Live 实验路线保持独立。

## 内容与来源

- `v60-source/src-fix37`、`v60-source/pb-fix37` 是完整的 V60 Live/PB 源码，目录名沿用历史构建约定。
- 输入为 `../feat-v53/integration-g-final/work`，依次应用 `patch_e3.py → patch_om1.py → patch_om3.py → patch_om4.py → patch_om5.py → patch_om6.py → patch_om7.py`；生产模式不设置 `OM_FORCE` / `OM_NORULE`。
- `README54.txt` 至 `README60.txt`、`verification54.json` 至 `verification60.json` 是当时交付记录。`README.md` 保存 V54–V60 演进；V59 因触发规则时退回 VFR 已被 V60 取代。
- V38–V53 的功能、构建与分析源码在相邻版本目录；原始导出哈希见 `../../../export-foundation-v60.json`、`../../../export-onemov-v60.json`。这里的源码路径经过脱敏；固件、卡镜像和私人影像不发布。

## 功能和操作

Live Ready 后拍照，等待保存完成；保存期间的连拍可能只有原厂照片而没有 Live。V60 在 `LIVE/目录/同名.MOV` 写单个短片，移除内嵌 LVJ1 预览；回放匹配的 JPEG 照片时按 REC 进入原厂视频页，结束返回照片。MAN 继续用于配对识别。V60 识别跟随 JPEG，不会从主卡 RAW 自动跨卡找到副卡 JPEG 的 Live。

冻结短片时，若帧数为 3k+1、超过三帧且去掉末帧仍覆盖快门，则丢最后一个 AU 并同步修改 `clip_end`，保持 30000/1001 CFR 和音频窗口。最多缩短约 33 ms。末尾快门保护可能保留 3k+1，因此不能保证所有输入都避开该帧数。

既有 3k+1 短片不会自动修复。Live 中无快门声，Shutter sound 行在此版本无效；音频采集失败会写等长静音轨。

## 证据与未解决问题

硬件对照片段确认 97/100 帧结尾卡死，98/99 帧正常；原厂内部根因未知，这不是所有素材的普遍证明。历史 QEMU 强制规则测试得到 90 帧 CFR、144144 音频帧且正常退出；正式构建拍摄得到 92 帧。正式拍摄回归没有覆盖同次开机“新拍 → 识别 → REC”的闭环。

后续 V60 实机反馈新照片没有 Live 标志。诊断区分了保存期间未生成短片和已有完整文件却未识别的两类情况；另复现了同照片首次 MAN 查找失败后不自动重试的旧缺陷。双卡 RAW/JPEG 是当时符合现象的候选，V60 诊断本身未确认用户实际回放路径。详见 [诊断记录](v60-diagnosis/RESULT.md)。历史诊断链接中的原始卡文件、日志、生成文件和实验室执行产物没有随本源码发布，不能单靠此仓库复跑那些证据分析。

## 重建与本次检查

从仓库根目录验证补丁链：

```sh
cp -R research/z8-live/build-tools/livediag/feat-v53/integration-g-final/work /tmp/z8-v60-source-new
python3 research/z8-live/build-tools/livediag/feat-onemov/patch_om7.py /tmp/z8-v60-source-new
```

目标目录须不存在。与 `v60-source` 比较时应逐文件一致。`build_onemov.py`、V53 compose 和 pack 脚本保留实验室布局；需要配置 `/path/to/user` 路径、LLVM 21、V36 seed、匹配 overlay/manifest 及私人原厂输入。V36 的 [from-stock](../../../from-stock/README.md) 是前置重建路线，不等于已有可移植的 V60 一键构建。单独克隆仓库无法声称重建出相同 MAIN。

本次实际重放了生产补丁链、Python 语法检查、freeze 边界夹具和仓库宿主测试；没有重新链接固件、运行 QEMU、操作 USB、存储卡或刷机。freeze 夹具通过 1,474,560 例，ASan/UBSan 无报告，边界不覆盖固件 ABI、任务并发和真机 IO。

历史产物身份：MAIN `641725cec49a702a9440e790a6495b6aef4fb0f7ef5a71b6acbda2719ba47d49`；更新包 `29d80ac78ddef976442d90e831b217dcf99bcc9f98570eb561180070a92d888f`。未上传这些固件字节。
