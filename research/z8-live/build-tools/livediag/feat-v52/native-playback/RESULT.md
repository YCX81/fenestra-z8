# v52c 原厂播放并行回归

两条 lane 均加载冻结 MAIN `4790c3e6138f4f22a5c189988e3ba7338de8f315b9bb3515930b3b97132ac00f`，私有 wrapper 保留真实 SHA 函数。`result.json`、实际 `-kernel` 文件和 identity 记录三者相符。报告使用 rdw QEMU 和真实 MCU 固件、显式模型/ROM输入，未执行 vendor VPU microcode；音频输出为 none。两条 lane 都是 Live Off，不能作为 Live On 交接的硬件验收。

| 输入 | 结果 |
|---|---|
| B 实际 CFR Live MOV（99 帧，916157 B，SHA `46bacc4c…b0c9ae`） | 原厂解析 1024×682，解码第 1 帧；随后遇显示模型未知 MMIO `0x15421104`，原断言失败并停止，不能算整片通过。 |
| 4K 参考 MOV（SHA `2aa43b03…d843a`） | 95 个 `DEC_PICTURE`，11 个 LPCM 命令、7 次实际转换 job；无未知 MMIO，driver 检查全部通过。 |

CFR 在私有 sparse 虚拟卡中替换 `DCIM/100NCZ_8/LIV_9013.MOV`，重新导出 SHA 与真实拍摄产物完全一致；fsck 只读检查通过。没有物理卡操作。完整 compact 数据是 `evidence/summary.json`，原始 `result.json` / stderr 和可重跑脚本在 `evidence/`。远端归档 `/path/to/user/fxfix/v52-native-playback/evidence.tar.gz`。
