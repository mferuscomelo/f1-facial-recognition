# PlatformIO: erase the whole flash on every upload, so a freshly flashed F1 starts from scratch
# (no registered people, no Wi-Fi data). esptool's --erase-all belongs after "write_flash",
# which `upload_flags` can't do, hence this script.
Import("env")  # noqa: F821

flags = env["UPLOADERFLAGS"]
if "write_flash" in flags and "--erase-all" not in flags:
    flags.insert(flags.index("write_flash") + 1, "--erase-all")
