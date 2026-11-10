用中文回答。解码 Z8 卡灯手机视频 /path/to/user/Downloads/IMG_4994.MOV（v7 心跳诊断版，已武装 token 9，skip_nr=0；120 fps 慢动作，手机固定）。只读视频，在 runs/v7-4994/ 下写分析文件和脚本；不要连 USB、刷写或碰相机。
操作：拔 USB→拍 Live→等卡灯灭再等约 5 s→切录像档→按 REC→死机重启。
协议见 src-fix35/recorder/README.md、lamp_protocol.h、video_decoder.py（v7：I/R/S/M 半位 50 ms；D 码半位 25 ms，100|阶段5位|值2位|CRC4；D1 值=busy|alive<<1；D3 心跳用阶段字段 6|busy、值=已等整秒数；D4 超时(值1)；D5 !alive 返回；D2/10/11/13/15/17/18 原义）。可复用 runs/a4991/ 的跟踪、积分与相位拟合脚本（卡灯黄绿色，“?”键左侧小圆点，相机倒置）。慢动作视频按真实时间换算（参考 runs/c4992/report.txt 的 PTS÷4 做法，先自行确认）。
未武装原厂基线：REC 后卡灯会连续亮约 1.2–1.6 s，重启时再长亮约 3 s（runs/c4992/report.txt）。
任务：逐帧跟踪，输出 lamp.csv、时间线；解出所有 I/D/R/S/M，列每帧位串、CRC、置信度，只有 CRC 通过才算有效；给出 D1 的 busy/alive 值、所有 D3 心跳（busy 与秒数）、是否有 D4/D5/D2/R，以及 REC 到黑屏时间。结论只写视频中有证据的内容，并对照 README 判定表说明含义。
