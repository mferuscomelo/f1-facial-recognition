# Status (updated 2026-11-11 00:25 CET)

## main
- Stage 0 done & accepted on hardware.
- Stage 1 plain-PPP verified: attaches on 1NCE APN `iot.1nce.net` (roaming 26201), POSTs reach webhook.site.
  12-min run: no crash/leak, 9/12 POSTs OK (boot POST lost, 2 transient connect aborts).
- Stock SG firmware backup: `backup/sg-stock-firmware-16MB.bin` (gitignored).

## Unmerged agent branches (worktrees under .claude/worktrees/)
| Branch | Commit | State | Next step |
|---|---|---|---|
| worktree-agent-a117c99df7b54fcdf | 20bed74 | LTE: CMUX (Kconfig fallback to PPP), 30 s health poll, RAT via AT+SQNMODEACTIVE?, ICCID after attach, 3-level reconnect, publish retries + async queue, boot event must-deliver. Builds. | Merge, flash, 10-min run (target 10/10 POSTs, CSQ changing). Watch for PPP drops ~30 s (then CONFIG_LWIP_ENABLE_LCP_ECHO=n). |
| worktree-agent-ac8a4b9f4277adefc | 199dd61 | Camera: usb_host_uvc 2.6 + esp_jpeg 1.3, 320x240 MJPEG -> 96x96 gray, hot-plug. Builds flag on/off (off = byte-identical). | Needs camera. Test steps in components/camera/README.md. Add decode-rate cap. |
| worktree-agent-a3741b1438b28039e | a3d38ec | Inference WIP: EI SDK CMake w/ ESP-NN S3 (fixed *.S glob), wrapper + arena placement (PSRAM/internal). Compiles, not wired into main. | main.cpp task + baked test images, then flash. Placeholder model is unlicensed, local only. |

## To do / open questions
- Add RSRP via `AT+SQNMONI=9` (Sequans forum: https://forum.sequans.com/t/what-is-the-command-to-check-the-current-cell-coverage/96) to the CMUX health poll.
- Ask SG Wireless: does J202 supply 5 V in USB host mode, and how much current?
- User: choose TLS backend (HTTPS vs MQTT) — deferred. Final Edge Impulse project — deferred.
- Later: UART 921600 (AT+IPR=921600,0) like SG.

## 2026-11-11 update
- LTE branch (CMUX) merged + verified on HW: Cat-M1 confirmed (AT+SQNMODEACTIVE), boot POST on 1st attempt, 30 s health polls.
- Camera branch merged. J202 DOES supply VBUS (Logitech 046d:0829, bMaxPower 500 mA, streams).
- Root cause of "0 frames": S3 default USB FIFO split limits ISOC MPS to 596 B; driver fell back to the
  camera's 512 B alt while it committed 800 B payloads -> pipe halted. Fix: custom FIFO split
  (rx 184 lines) + vendored usb_host_uvc with MAX_MPS_IN 724 -> camera uses its 700 B alt.
- Stage 3 acceptance met: ~7.4 fps decoded (decode ~50 ms + resize 6 ms), lens-cover changes mean
  brightness (55 -> 30), 3 hot-plug cycles OK, internal heap stable, UART console unaffected.
- A second camera (YUY2-only 160x120, single alt MPS 956) cannot work on ESP32-S3 (FIFO max ~724).
- webhook.site inbox appears full ("Incomplete chunked data") -> new URL needed.
- Next: wire Stage 2 inference (branch worktree-agent-a3741b1438b28039e) to camera frames.
