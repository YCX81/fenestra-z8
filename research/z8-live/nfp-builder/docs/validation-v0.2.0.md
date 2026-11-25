# 上一公开版 v0.2.0 / Previous public release

v0.2.0 提供 ProRes 四档以及 C15／C30 HE／HE★ RAW 连拍和预拍。RAW 格式、画质与双卡设置直接取自机内菜单，不读取 `Z8PRE.CFG`。发布附件和 tag 保持原样；当前版本说明见[验证摘要](validation.md)，版本变化见[更新日志](../CHANGELOG.md)。

## 已完成的公开版检查

- 公开构建包含 ProRes、RAW 预拍及两者组合，三种选择均经过独立构建与字节核验；源码与预编译输出一致。
- 实际公开组合包的实拍共 81 个独立拍摄帧，产生 102 份 NEF 和 62 份 JPEG。NEF 全部完成 Adobe DNG Converter 转换及完整像素读取；21 对双卡 NEF 的 RAW 条带、像素和预览一致，60 对 RAW／JPEG 分卡文件对应。
- 所有 NEF 带 8256×5504 整幅预览，RAW 条带结束处即文件末尾，没有诊断尾标。
- HQ／Proxy 两段 4K 29.97p、10-bit 4:2:2 ProRes 共 345 帧，编码结构及视频／音频完整解码通过。

## 与当前版本的区别

v0.2.0 没有像素位移合成、开放门或四档 N-RAW。其预拍剩余张数与空间预留仍按原厂高速 JPEG 计划；数据库拒绝后存在直接写临时 NEF 的后备路径。当前版本的计数和登记拒绝处理见[RAW 指南](raw-precapture.md)，停止文件在两版中的行为差异见[停止文件指南](stop-file.md)。

## English

Released v0.2.0 provides four ProRes grades and C15/C30 HE/HE★ RAW/pre-capture with ordinary-menu controls and dual-card routing. Its public camera checks cover 102 fully decoded NEFs, 62 JPEGs and 345 ProRes frames. Twenty-one dual-card NEF pairs have matching RAW data, pixels and previews; 60 split RAW/JPEG pairs correspond. NEFs contain full-size previews and no diagnostic footer.

It does not include pixel-shift merging, open gate or four N-RAW tiers. Its remaining counts retain the stock JPEG estimate, and database refusal can enter a direct temporary-NEF fallback. The current version's behavior is described in the feature guides. Original release contents remain available at the [v0.2.0 release](https://github.com/XinmiaoZhao/NikonFirmwarePatch/releases/tag/v0.2.0).
