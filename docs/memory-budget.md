# Memory budget

Logged by `board::log_heap_once()` every 10 s (`heap_caps_*` for INTERNAL and SPIRAM).

| Stage | Commit | Internal free | Internal largest | Internal min-ever | PSRAM free | PSRAM largest | Notes |
|---|---|---|---|---|---|---|---|
| 0 | a0ab4f4 | 378 267 | 286 720 | 378 267 | 8 386 156 | 8 257 536 | steady state after 20 s; LED (RMT) + heap task only. App image 219 KB. |
| 1 (plain PPP) | c8f4097 | 339 955 | 262 144 | 332 279 | 8 371 972 | 8 257 536 | 12-min run, LTE attached + PPP + HTTP POST every 60 s. No crash, no leak (free oscillates 339.9–340.6 K). ~38 K internal used by esp_modem/PPP/lwIP/http. |
