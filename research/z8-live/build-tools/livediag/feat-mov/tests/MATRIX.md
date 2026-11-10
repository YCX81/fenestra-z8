| 文件 | 对照 | 唯一语义变化 | 字节数 |
|---|---|---|---:|
| `00-original.MOV` | 输入 | 原始字节副本，保留 Apple 配对 | 4,222,730 |
| `01-only-niko.MOV` | 00 | ftyp[20..23] 加入 niko | 4,222,734 |
| `02-only-remove_mebx.MOV` | 00 | 删除定时 mebx 轨 | 4,222,217 |
| `03-only-audio_stco.MOV` | 00 | 音轨 co64 转 stco | 4,222,726 |
| `04-only-size_from_sps.MOV` | 00 | 容器尺寸及对应矩阵平移跟随 SPS | 4,222,730 |
| `05-niko-no-mebx.MOV` | 01 | 删除定时 mebx 轨 | 4,222,221 |
| `06-niko-no-mebx-stco.MOV` | 05 | 音轨 co64 转 stco | 4,222,217 |
| `07-all-four.MOV` | 06 | 容器尺寸及对应矩阵平移跟随 SPS | 4,222,217 |
| `08-all-four-qemu-ncdt.MOV` | 07 | 加入归档 QEMU MOV 的完整 NCDT | 4,546,386 |
| `09-06-crop-to-container.MOV` | 06 | 仅给 SPS 增加底部 6 行裁剪，显示 1024×682 | 4,222,218 |
