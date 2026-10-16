---
name: yomu-kindle
description: >
  Build the yomu Kindle binary and copy it to the Paperwhite.
  Use when the user asks to deploy, install, scp, or refresh yomu on the Kindle, or runs /yomu-kindle.
  Copy files only after they ask for a deploy.
---

# Kindle deploy

Rebuild with the clean Kindle command in `AGENTS.md`. The binary is `dist/kindle/yomu/yomu`. Confirm it is a stripped static ARM EABI5 ELF before copying.

The device is `root@192.168.6.205` on SSH port 2222 (`ssh -p 2222`, `scp -P 2222`).

Copy:

- `dist/kindle/yomu/yomu` and `dist/kindle/yomu/yomu.sh` to `/mnt/us/yomu/`
- `dist/kindle/extensions/yomu/config.xml` and `menu.json` to `/mnt/us/extensions/yomu/`

`chmod 755` the binary and `yomu.sh`, then `sync`. The `md5` on the Mac and on the device must match.

`platform/kindle/yomu.sh` re-execs copies at `/var/tmp/yomu.sh` and `/var/tmp/yomu.bin`. Replacing `/mnt/us/yomu/yomu` leaves a running process on the old binary. The new file loads on the next KUAL `yomu` launch.
