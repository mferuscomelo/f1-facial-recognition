# Status (paused 2026-11-10 23:10 CET)

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
