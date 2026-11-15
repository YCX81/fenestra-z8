Nikon Z8 测试包 v41：只插 SD 卡也能用 Fenestra + Live 双卡适配
基础固件 C3.11，版本标识 04.06.p0，以 SHA-256 辨认。
= 上一包 v40（MAIN d1e6a6df：中日韩字体修复 + Live 快门声开关 + Live H.265 + NFP v0.4 四组 + C64 搬家 + C64 开关 + 竖屏录像插件）
  + SD 卡回退（FENESTRA 目录可以放在 SD 卡上）
  + Live / 机内回放的双卡改动
  + bootguard 构建号重新生成。

一、FENESTRA 目录可以放在 SD 卡上
- 每次开机选一次 Fenestra 用哪张卡：
  · A 卡（CFexpress/XQD）上有 FENESTRA/ → 用 A，和以前完全一样。
  · A 卡上没有 FENESTRA/（或 A 槽没插卡），SD 卡上有 → 用 SD：插件、启动标记、Auto activate 设置、
    C64 的 SC*.REC 记录、Live 的设置文件都读写 SD 卡上的 FENESTRA/。
  · 两张卡都没有 → 按 A 处理，与以前相同。
- 两张卡都有 FENESTRA/ 时永远用 A。建议只在一张卡上放 FENESTRA/。
- 卡刚插入、还没挂载好时，Fenestra 会短暂显示 "Checking card"/"Card error"，挂载完成后自动重新检查，最长等 20 秒。
- 开机后才换卡：本次开机选定的卡不变，重启后重新选。
- 只插 SD 卡使用：把本包里的 FENESTRA/ 文件夹拷到 SD 卡根目录。

二、Live 双卡改动
- Live 的 MOV/JPG/MAN 写在“原厂照片 JPG 所在的那张卡”上：只插 A、只插 SD、主卡槽选 SD、溢出、RAW 主卡 + JPEG 副卡都按原厂照片走。
- 备份模式、JPEG+JPEG 模式：Live 先在第一张卡上生成，再把同一组 Live 文件补写到另一张卡（MOV 原样复制，配对 JPG 由那张卡自己的 JPG 生成）。
  两张卡文件夹编号不同或目标已有同名文件时不补写；补写失败会删掉半成品，第一张卡不受影响。
- 机内回放 REC 预览：改为按照片路径 + 文件大小找 Live 文件（以前按开机后卡挂载次数，两张卡同时插着时会找不到）。
- 回放里删除/保护照片时联动 Live 文件：A、SD 两张卡都生效；插拔卡后自动重新开始。
- Live 诊断日志（LIVEDATA/LIVELOG.BIN）写在 Fenestra 所在的卡上。

三、建议的真机检查
1. 只插 SD，SD 根目录放 FENESTRA/：Fenestra 能 Activate，插件页出现；Live 选 On 拍一张，SD 上有 LIVE/…/DSC_nnnn.JPG/.MOV 和 LIVEDATA/…/DSC_nnnn.MAN；回放按 REC 能播。
2. 只插 CFexpress（FENESTRA 在 A）：一切和 v40 相同（Live、快门声开关、REC 预览、竖屏、C64 开关）。
3. 两张卡、备份模式：拍 Live 后两张卡都有完整的 LIVE/LIVEDATA 文件；分别只插一张卡回放 REC 都能播。
4. 两张卡、回放里删一张 Live 照片：两张卡上对应的 Live 文件一起删掉。
5. 中文界面下 Fenestra / Live 文字仍为比例字体，“Requested” 完整（v40 修复保留）。

四、验证范围
- 主机测试：Live 双卡补写（真实 Z8 JPG，104 个写卡失败点全部干净回滚、取消、同名不覆盖、路径防护）；回放同步（SD 卡、跨挂载、只读、清理）。
- Unicorn：SD 选卡逻辑 67 项全部通过；core 路由代码与上一份 SD 回退包（61d5896f）逐字节相同。
- QEMU：见 verification.json（QEMU 里没有固件能用的 SD 卡模型，SD 路径只在主机测试和 Unicorn 中验证）。
- 未上真机。
