# Runtime / hook 靜態與 Unicorn 覆核

`hook_test.py` 對實際連結的 `payload.elf` 執行 32 個 Unicorn case（IRQ/full press × 全部 16 種 NZCV）。notify helper 使用故意破壞 X0…X30/NZCV 的 stub；每個 wrapper 正確還原全部整數 register/flags。IRQ 保留被置換的原生 `stp x19,x30,[sp,#-16]!`：resume `0x40e9083c` 時 SP 為原 SP−16，stack 內容吻合。full press SP 不變，確實接回舊 movie target `0x40a020c4`。結果與 ELF SHA 見 `hook-test-results.json`；這不執行 helper 本體/原生服務，不是 QEMU 或 SIMD context 測試。

Runtime 的 IRQ/full press wrapper 沒有取得 mutex、做檔案 I/O 或等 copy/encoder；`state()` 透過 acquire 發布的持久 UI/engine root 取資料。copy worker 與 UI poll 同 task，卡配置 I/O 可能延遲 copy，因此將來啟用 source lease 前仍須量測 scheduling/drop。這輪沒有 source lease，故不複製像素、不持有大 ring。

Native allocator 對照既有 `live-ram/REPORT.md` 與 `release-asm.txt`：W0 caller tag、W1 logical class60、W2 bytes；D+8→R，R+0 pointer、R+8 actual bytes、D+0x10 actual class；free 0x4104d600。native adapter 對 NULL、selector改變、非class24、返回過小、地址溢出皆拒絕，已取得 descriptor 的拒絕分支配對 free。selector0/class24 檢查仍不能證明原生 still 需求峰值或 plan transition 全部完成；本輪實際 gate 未進 allocator。

發現並已通知父層修正的 runtime review 項目（以最終 runtime.c 為準）：

1. writer_active 的 CAS 直接發布1後才寫 writer_file_id/path-valid；其他 open hook 可讀到前一張 ID。建議先 CAS 到 initializing=2，填欄位後 release-store1。path/path-valid 的跨任務存取也應改為同一 task ticket 或明確原子發布；僅 matching file ID 不等於已證仍是同一張 ticket。
2. task_error 在 engine 發布後有普通跨 CPU read/write，建議 atomic；native task start 返回失敗但 native_error=0 時仍須設非零 failure indicator。
3. engine boot 的「先讀是否存在，再分配/發布」依賴 UI install 僅單一 dispatcher 執行。若不能證明單一 caller，需 CAS 發布，否則可能重複 worker/泄漏。當前沒有實證表明發生。

未發現本 wrapper 額外持有原生鎖等待 encoder 的死鎖鏈；這只是有界源碼覆核，不能保證未知原生服務的多核鎖/優先級行為。
