> v2 注意：下文是复制时的原版集成说明。当前 v2 接入 best-effort source；重试构建用 8 帧／400 ms 间隔及 General heap。它不证明 DMA lease。

# Capture 模組交付

`live_capture.c` 實作 90 × 640 × 424 I420 = **36,633,600 bytes** 的原生 descriptor ring；NV12 Y/UV 分開讀，stride 與來源尺寸均由 source 物件取得，縮小後才入 ring。預卷最多 45 幀，按時間收集全按後 1.5 秒；24 fps 實際少於 90 幀，不複製舊幀補數。影片應用保存的逐幀 `timestamp_us` 與 `press_us`，不是固定第 45 幀。`press_us` 仍是軟體按鍵時間，未校準成光學曝光。

父層必須建立持久 `struct la_capture`，傳入 allocator、source lease callback，呼叫 `la_init`。native wrapper 將 0x40e90838 的 W0/X1 和當前時間轉交 `la_irq_signal`；此函式僅寫 seqlock 世代/時間信箱，完全不碰像素、不分配、不等待。單一 copy worker 呼叫 `la_step(c, now_us, eligible)`。full-press wrapper 呼叫 `la_full_press`，不可等待 worker；原廠 still 路徑必須無條件繼續。`eligible` 要由整合層證實普通 PHOTO/selector0、非 HSFC/預釋放/C30/C60、card/LV/wake 有效。

`la_begin_encode` 成功後，encoder 用 `la_frame_at` 順序讀取像素與真實時間，期間每幀檢查 `la_cancelled`；完成/取消後呼叫 `la_finish_encode`。關閉、模式切換、睡眠、換卡呼叫 `la_cancel`，copy worker 觀察取消後只在 encoder reader 退出時釋放。`la_step(...,eligible=0)` 同樣會使正在編碼者取消。取消前仍在執行的像素複製先完成驗證再丟棄；不會發布已取消幀。分配只發生在第一個可證 source lease 成功後；沒有 source proof 時不持有大塊 RAM。

`native_capture.c` 包含：

- B1 runtime format/stride `0x1542210c/10/38/3c`，F1 width/height `0x1542312c/30`。沒有寫死 1024×680、UV offset 或 slot step。
- `0x4104d2dc(0x4c41,0x60,bytes)` 和配對 `0x4104d600`，前後檢查 selector0 和 descriptor class24、返回容量。selector 檢查不能代替所有 mode/card eligibility。
- 帶 processor/affinity 參數的 `la_native_task_start`；`la_native_cpu2_start` 用 3/4、caller 私有 stack。既有 Fenestra callers 不受影響。每幀用 `la_native_yield`，原生 priority 仍須整合測量。
- `la_native_time_us` 將既有 QEMU 的 native CNTPCT/24 單位乘 984/1000；僅限該仿真模型，不能稱為硬體時間。

## 尚未閉合的 source adapter

沒有提供假裝成功的 `lease_begin`。預設空 callback 時狀態為 REQUESTED、reason `LA_SOURCE_UNPROVEN`，不分配 ring、不顯示 READY、不產生 Live 檔案。需要已驗證 retain 或 producer 生命週期協定，保證來源在 copy 期間不被 DMA 改寫，並完成 CPU/DMA cache 映射處理；取得 lease 後還會前後驗證，且檢查自己的 IRQ 世代未變。

核對 `/mnt/c/path/to/user/Desktop/Nikon/z8-qemu/qemu/nikon-z9-pixels.h:125–169` 與 `nikon-z9-lab.c:12470–12502`：DMA 真正讀 `engine1543_latched`，而 guest 讀 C0+8/+0x200 得到 submitted selector/table。新 bank 未完整時 model 可以重用舊 latched bank。C0 result snapshot +0x18/+0x1c/+0x80 尚是 strict/unimplemented；不能從單次 config 或完成 IRQ 得出實際 completed buffer。`frame_probe.c` 明確透過 host model-private state 取得 actual Y/UV，不是可直接移植成 guest adapter 的寄存器 API。

可研究從 C start 與每次完整 bank 提交重建模型狀態，但必須包含被重用舊表、完整 64-word validity、停啟、missed IRQ/重配等條件；本交付沒有把未證重建充作 source lease。硬體 DMA start/cache 所有權仍獨立未驗。

## Guarded hooks

`hook-evidence.json` 保存 stock/base SHA 與各入口的精確 expected bytes。**0x40fdc9c8 已被現有 movie Live 改成 branch**，新 wrapper 必須 chain 原 branch target，不能重放 stock prologue 後跳過舊 movie 功能。0x40e90838 在指定 base 仍為 stock prologue。父層負責實際 wrapper placement 與 free-code-space/零 overlap 證明；本子模組沒有把零 bytes 視為已證安全洞。

## 已執行驗證

`./capture/check.sh`：host ASan+UBSan，split-plane/padded NV12→I420、copy 後 lease 撤銷、NULL 分配、沒有 proof 不分配、24/30 fps timed window、重複按鍵、stale READY、取消/模式改變與 encoder reader 延後釋放均通過；AArch64 freestanding `-Wall -Wextra -Werror` 編譯通過。LeakSanitizer 因執行器 ptrace 不能運作而關閉，其餘 sanitizer 與明確 allocation/free 斷言仍開啟。此子任務沒有啟動 QEMU，沒有修改原 firmware 或目錄外 source。
