用中文回答。解码 Z8 卡灯手机视频 /path/to/user/Downloads/IMG_4996.MOV（v8b 诊断版，已武装 token 10，skip_nr=0；120 fps 慢动作，手机固定）。只读视频，在 runs/v8b-4996/ 下写分析文件和脚本；不要连 USB、刷写或碰相机。
操作：拔 USB→拍 Live→等卡灯灭再等约 5 s→切录像档→按 REC→死机重启。
协议见 src-fix35/recorder/README.md、lamp_protocol.h、video_decoder.py（v8b：REC 路径只发 I 码；之后 Live worker 在录像模式下每约 2.5 s 发一帧带 CRC 的 P 帧，内容为 RAM 进度阶段号 + 附加值 + worker 心跳序号；阶段表见 README，首次 TRIGGER 前/返回/输出等待=24/25/26，drain=30/31/32 等）。可复用 runs/v7-4994/、runs/a4991/ 的跟踪、积分、相位拟合脚本（卡灯黄绿色，“?”键左侧小圆点，相机倒置）。慢动作按真实时间换算（先自行确认时间基）。
未武装原厂基线：REC 后卡灯连续亮约 1.2–1.6 s，重启时再长亮约 3 s（runs/c4992/report.txt）。
任务：逐帧跟踪，输出 lamp.csv、时间线；解出所有 I 与 P 帧，列每帧位串、CRC、置信度，只有 CRC 通过才算有效；按时间列出每个有效 P 的阶段号、附加值、心跳序号；给出 REC 到黑屏时间。结论对照 README 阶段表说明 REC 任务最后停在哪一步、worker 是否持续报码（心跳是否递增、何时停止）。只写视频中有证据的内容。
