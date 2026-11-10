# Z8 拍照模式麦克风探测（Fenestra USB 调试）

目的：在真机的**拍照取景**下，用原厂工厂命令 `audiodbg`（SoundTest）开关麦克风，看能不能录到声音，以及对拍照有没有影响。依据是 C3.11 原厂 MAIN 的静态分析（记忆 `z8-audiodbg-soundtest`）。脚本只在离线模拟相机（`fake_camera.py`）上测过，**没有连过真机**；Z8 的 Fenestra USB 调试本身也从没在真机上跑过。

## 准备

1. Fenestra 主机工具：
   ```bash
   git clone ~/Nikon/_mac-migration/bundles/fenestra-z8.bundle ~/Nikon/fenestra-z8
   ```
   脚本默认找 `~/Nikon/fenestra-z8/host`，也可以用 `--fenestra-host` 指定。
2. 主机环境，二选一：
   - **Windows**（例如 win-lab）：`pip install comtypes==1.4.13`，用系统自带的 WPD 驱动，不需要换驱动。
   - **Mac**：`brew install libusb`，`pip3 install pyusb`。macOS 的 `ptpcamerad` 会抢占相机，连接前在另一个终端里一直停住它：
     ```bash
     while true; do killall -STOP ptpcamerad 2>/dev/null; sleep 0.5; done
     ```
     用完 Ctrl-C，再执行 `killall -CONT ptpcamerad`。
3. 相机：装好现在这版测试固件，激活 Fenestra，SETUP → Fenestra → **USB Debug** 打开，接 USB 线；切到**拍照模式**，取景打开；卡 1 留 30 MB 以上空间。

## 步骤（遇到任何意外就停下）

每一步都会在 `logs/` 下写一份 JSONL 日志，出问题时把它发给我。

1. **只读连通性**
   ```bash
   python3 z8_audio_probe.py check --read-only
   ```
   这一步会：
   - 握手，确认 Fenestra 报告 `C 3.11`，并且开放了 RTOS 调试
   - 读寄存器和几个音频全局变量
   - 执行原厂 `echo`，再用 `help` 确认命令表里有 `audiodbg`

   这一步对相机不做任何改动。
2. **确认命令可用**
   ```bash
   python3 z8_audio_probe.py check
   ```
   在第 1 步基础上，用 `audiodbg 21 1/0` 把一个只有声音测试才用的位深变量改一下，再读回来，证明命令确实执行了、地址也对。最后会恢复成 16 bit。
3. **录 3 秒**
   ```bash
   python3 z8_audio_probe.py record --seconds 3
   ```
   - 依次发 `audiodbg 1 1 0` 开始、等待并监视、`audiodbg 1 0 0` 停止，再用 `audiodbg 20 7` 写出 `A:\TEST0007.WAV`（约 23 MB）。
   - 开始以后不管中途出什么错，脚本都会去发停止。
   - 监视过程中，每 0.25 秒打印一次音频状态，以及两段全局变量里变了哪些字，用来找录音进度计数器。
   - 说话或拍手，方便后面辨认是不是麦克风的声音。
4. **分析 WAV**：把卡 1 根目录的 `TEST0007.WAV` 拷到电脑上：
   ```bash
   python3 z8_audio_probe.py analyze TEST0007.WAV
   ```
   每 0.5 秒给出峰值和 RMS，最后一行判断有没有非零数据。最后再用耳朵听一下。
5. **（可选）读原始缓冲**
   ```bash
   python3 z8_audio_probe.py pull --seconds 0.5
   ```
   通过 USB 把录音缓冲区的原始数据读回电脑，不写卡。脚本会分别按 s16 和 s32 解读，看是不是左对齐的 24 bit。读之前先逐页确认地址已经映射，未映射就不读，避免把相机读死。
6. **录音时按快门**
   ```bash
   python3 z8_audio_probe.py record --seconds 5 --shutter-at 2 --file-no 8
   ```
   第 2 秒提示你全按快门一次。之后看三件事：照片有没有保存、取景有没有中断，以及 WAV 里快门那一刻的声音。

其他：`watch --seconds 10` 只读，不做任何改动，可以在你自己操作相机时观察音频状态。

## 不拔卡读卡（ls / get）

通过 Fenestra 调用原厂串行化文件接口，在相机里读卡，再经 USB 传回来。只读，不写卡。

```bash
python3 z8_audio_probe.py ls --path 'A:\'
python3 z8_audio_probe.py ls --path 'A:\DCIM\100NCZ_8'
python3 z8_audio_probe.py get --path 'A:\CODEC00.TXT' --output CODEC00.TXT
```

- 速度约 35 KiB/s：300 KB 的文本约 10 秒，11.5 MB 的 WAV 约 5–6 分钟；可以用 `--limit` 只取开头。
- 目录项只有短文件名和长文件名，没有大小和属性；对文件执行 `ls` 会报错，可以借此分辨目录和文件。
- 相机里用到的临时内存（约 68 KB）只申请一次，地址记在 `logs/scratch.txt`，之后重复使用；关机后自然释放。
- `A:` 是卡 1；卡 2 应该是 `B:`，未测。

## 出问题时

- 命令超时：脚本**不会自动重发**，因为原厂命令可能还在执行。先关掉 USB Debug；如果相机异常，就关机重启。
- `check` 里如果 `help` 没有列出 `audiodbg`，或者读回值不对，就停下，把日志发我，不要继续 `record`。
- 已知风险：
  - 第一次开始录音会申请约 23 MB 内存，可能不释放，会占用连拍缓冲
  - `audiodbg 1` 第二个参数的含义不清楚，脚本固定用 0
  - 原厂 shell 解析数字时用十进制还是十六进制不确定，所以文件号只用 1–9

## 读取 RTOS 内存

不用这个脚本也行，Fenestra 自带：

```bash
python3 ~/Nikon/fenestra-z8/host/ptp_rtos.py dump --address 0xa02d4c5c0 --count 0x1000 --output svc.bin
```

每次请求最多读 1008 字节，速度要实测，`pull` 会打印 KiB/s。原厂 shell 里也有 `memdump`、`stackdump`、`kernelsysmem`、`binlogcard`，可以用 `ptp_rtos.py execute --line "..."` 调用。不过 shell 的输出缓冲只有 8 KiB，大块数据还是用 `dump`。

注意两点：
- 读到未映射的地址会让相机直接异常，先用 `ptp_rtos.py translate` 确认。
- 不要读 MMIO 寄存器区，读操作本身可能有副作用。
