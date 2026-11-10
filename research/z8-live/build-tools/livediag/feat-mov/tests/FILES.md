此表保留 11 月 9 日的 00–07 归档。完整 00–09 对照与新增 NCDT、SPS 裁剪实验见[当前矩阵](MATRIX.md)。

| 文件 | 对照与唯一语义变化 | 字节数 |
|---|---|---:|
| `00-original.MOV` | 原始字节副本 | 4222730 |
| `01-only-niko.MOV` | 相对 00 仅 niko | 4222734 |
| `02-only-remove_mebx.MOV` | 相对 00 仅 remove_mebx | 4222217 |
| `03-only-audio_stco.MOV` | 相对 00 仅 audio_stco | 4222726 |
| `04-only-size_from_sps.MOV` | 相对 00 仅 size_from_sps | 4222730 |
| `05-niko-no-mebx.MOV` | 相对 01 仅删除 mebx | 4222221 |
| `06-niko-no-mebx-stco.MOV` | 相对 05 仅 audio stco | 4222217 |
| `07-all-four.MOV` | 相对 06 仅尺寸及对应矩阵平移 | 4222217 |

每项保留 mdat 原始字节；删除轨道只删除引用，原 9 字节仍留在 mdat。
尺寸项同时更新标准旋转矩阵的平移，是同一几何修正。
`ncdt-structure-only.bin` 是 66 字节外层结构探针，不是已验证可播放 metadata；未注入任何 MOV。
