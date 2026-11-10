# Memory budget

Logged by `board::log_heap_once()` every 10 s (`heap_caps_*` for INTERNAL and SPIRAM).

| Stage | Commit | Internal free | Internal largest | Internal min-ever | PSRAM free | PSRAM largest | Notes |
|---|---|---|---|---|---|---|---|
| 0 | a0ab4f4 | 378 267 | 286 720 | 378 267 | 8 386 156 | 8 257 536 | steady state after 20 s; LED (RMT) + heap task only. App image 219 KB. |
