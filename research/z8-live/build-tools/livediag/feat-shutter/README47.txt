Nikon Z8 测试包 v39：Live 快门声开关
基础固件 C3.11，版本标识 04.06.p0，以 SHA-256 辨认。
= 上一包 v38（MAIN bc39aaba：Live H.265 + NFP v0.4 四组 + C64 搬家 + C64 开关 + 竖屏录像插件）
  + Live 菜单新增“快门声”开关
  + bootguard 构建号重新生成（0x7d1ddc8e -> 0xcbb7a6b2）。

一、用法
- 设定菜单 -> Fenestra -> Live Photo，进入后有三行：On / Off / Shutter sound: On（或 Off）。
- 选 “Shutter sound” 这一行切换一次；行上的文字显示当前状态。设定随 Live 开关一起存在卡上 FENESTRA/live-z8.0 / live-z8.1（第 28 字节），关机后保留。
- 设成 Off 后：只有 Live 已开启、并且在拍照档时，快门声被静音；Live 关闭或切到录像档时恢复原厂行为。设成 On（默认）完全是原厂行为。
- 菜单文字只有英文（Fenestra 字库限制）。

二、实现
- 原厂拍照时在 0x40F9CB64 读取自己的静音标志（0xA03BC7008），非 0 就走原厂的静音分支。v39 把这一条读取换成调用 Live 层的判断函数：需要静音时让它返回 1，原厂静音分支照常执行；其余情况返回原厂读到的值。播放代码本身没有改。
- 这条路径是根据原厂固件自行分析的，并在 QEMU 中用观察点确认（Live 开/关路径相同）。没有使用其他项目的代码。
- Live 运行时结构大小不变（USB 调试工具按大小识别版本），新状态放在 Live 界面对象末尾。

三、建议的真机检查
1. 刷入后进 Live Photo 页，确认有第三行 “Shutter sound: On”。
2. Live 选 On，Shutter sound 切到 Off：拍一张，应无快门声；Live 照片和短片正常写出。
3. Shutter sound 仍为 Off，把 Live 选 Off：拍一张，应恢复快门声。
4. 切到录像档、再切回拍照档，各拍一张对照；关机重开后检查设定仍在。
5. 连拍（CL/CH/C15/C30）下有没有残留快门声：这条路径在 QEMU 中未验证。

四、验证范围
- QEMU：见 verification.json（静音组：原厂快门声起始调用未发出，Live 文件正常；对照组：快门声正常发出，Live 文件正常）。QEMU 没有扬声器模型，以原厂“开始播放快门声”调用是否发出为准。
- 未上真机。
