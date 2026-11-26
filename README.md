# Ubuntu Touch bring-up for Google Pixel 7 (panther)

> [!CAUTION]
> **DISCLAIMER / ПРЕДУПРЕЖДЕНИЕ:**  
> This project is experimental early-stage bring-up software. Flashing custom `init_boot` or logical `super` partitions carries risk. **I am not responsible for bricked devices, data loss, dead SD/UFS storage, or anything else that happens to your phone.** Proceed at your own risk, keep one A/B slot with stock Android untouched, and know how to recover via Fastboot.  
> *(Я не ручаюсь за «кирпичи», потерю данных и любые последствия — всё делаете на свой страх и риск.)*

A custom ARM64 `/init` brings up the Pixel 7 (`gs201`) display, UFS storage, `super` logical partitions, USB NCM/ACM gadget, and hands off to Ubuntu Touch `systemd` (`24.04` / Noble).

## Roadmap

| Status | Stage | Milestone |
| --- | --- | --- |
| **Completed** | **Stage 1: Early boot & display** | Boot custom ramdisk, bring up Samsung P10 panel (`1080 × 2400`) through Exynos DRM/KMS + IOMMU, and render on-screen console. |
| **Completed** | **Stage 2: Ubuntu Touch userspace** | Mount EROFS Ubuntu Touch rootfs from `super` (`system_a`/`system_b` via `liblp` + `dm-linear` + `overlayfs`), boot `systemd 255.4`, capture live `/dev/kmsg` on screen + USB ACM serial, and bring up USB NCM Ethernet + root SSH (`10.15.19.82`). |
| **Current** | **Stage 3: Graphical session** | Load touchscreen drivers (`goog_touch_interface` / panel touch), hand off DRM Master from boot console to `lightdm` / Lomiri shell. |
| Planned | Stage 4: Hardware support | Wi-Fi, Bluetooth, audio, charging, suspend, sensors, camera, and cellular service. |
| Planned | Stage 5: Packaging and testing | Reproducible builds, installer/documentation, and clean updates on `panther`. |

---

## Stage 2: Ubuntu Touch Userspace (`stage2/`) — Completed

### What works in Stage 2
- **Exynos DRM/KMS (`1080×2400`)** with MIPI DSI Command Mode flush (`DIRTYFB` + `SETCRTC`) and live filtered `/dev/kmsg` boot console.
- **UFS storage & `liblp` `dm-linear`** (`ufs-pixel-fips140.ko`, `ufs-exynos-gs.ko`, `super` metadata parsed at offset `12288`).
- **EROFS + `overlayfs`** rootfs mounted from `system_a` (or `system_b`) in `super` without touching Android's encrypted `userdata` or the inactive slot.
- **Hardware key controls** (`s2mpg12-key.ko`): `Vol Down` = soft reboot to Fastboot (preserves `ramoops`), `Power` = Power Off, `Vol Up` = cancel 180s auto-reboot.
- **USB NCM + ACM Composite Gadget (`1D6B:0104`)**: loads `max77729-pmic.ko`, `max77759-charger.ko`, `max77759_helper.ko`, `tcpci_max77759.ko`, `google_tcpci_shim.ko`, `phy-exynos-usbdrd-super.ko`, and `dwc3-exynos-usb.ko` to expose both:
  - USB Serial (`acm.usb0`, `/dev/ttyGS0`) streaming full `/dev/kmsg`
  - USB Ethernet (`ncm.usb0`, `usb0` = `10.15.19.82/24`) with standalone `sshd` on port 22.
- **Ubuntu Touch `systemd 255.4` booted** with `lightdm.service` running and root SSH verified.

### Source files
- [`stage2/ut_stage1_init.c`](stage2/ut_stage1_init.c) — Stage 1/2 `/init` binary source (C, static `aarch64-linux-musl`).
- [`stage2/prepare_ut_rootfs.sh`](stage2/prepare_ut_rootfs.sh) — Builds compressed EROFS `ut-rootfs-system.img` from Ubuntu Touch ARM64 rootfs.
- [`stage2/repack_cpio.ps1`](stage2/repack_cpio.ps1) — Replaces `/init` inside an unpacked stock/Stage-1 `ramdisk.cpio`.

### Build & Flash (Stage 2)
1. **Compile `/init`** (using Zig `0.13.0` or `aarch64-linux-musl-gcc`, passing your SSH public key):
   ```sh
   zig cc -target aarch64-linux-musl -O2 -static \
     -DUT_SSH_PUBKEY='"ssh-ed25519 AAAA... you@host\n"' \
     stage2/ut_stage1_init.c -o ut_stage1_init
   ```
2. **Repack `init_boot.img`**: replace `init` in `ramdisk.cpio` (e.g. via `stage2/repack_cpio.ps1`), compress with `lz4 -l -12 --favor-decSpeed`, and update `ramdisk_size` in the `ANDROID!` boot header.
3. **Prepare EROFS rootfs** (on Linux):
   ```bash
   ./stage2/prepare_ut_rootfs.sh ./ut_work ./ut-rootfs-system.img
   ```
4. **Flash to the test slot** (example uses **slot A**, keeping slot B Android intact):
   ```sh
   fastboot reboot fastboot   # enter fastbootd
   fastboot create-logical-partition system_a 1610612736
   fastboot resize-logical-partition system_a $(stat -c%s ut-rootfs-system.img)
   fastboot flash system_a ut-rootfs-system.img
   fastboot reboot bootloader
   fastboot flash init_boot_a panther-ut-stage1-init_boot.img
   fastboot oem watchdog disable
   fastboot --set-active=a
   fastboot reboot
   ```
5. **Connect via SSH** once booted: assign `10.15.19.1/24` to the `UsbNcm Host Device` adapter on host and run `ssh root@10.15.19.82`.

---

## Stage 1: Display Demo (`panther-init-screen-iommu.img`)

Use the four stock images from **`DP11.261030.003`** (`image-panther_beta-dp11.261030.003.zip` inside the [Google factory package](https://developer.android.com/about/versions/17/qpr3/download)) and `panther-init-screen-iommu.img`:

```sh
fastboot --slot SLOT flash boot boot.img
fastboot --slot SLOT flash dtbo dtbo.img
fastboot --slot SLOT flash vendor_boot vendor_boot.img
fastboot --slot SLOT flash vendor_kernel_boot vendor_kernel_boot.img
fastboot --slot SLOT flash init_boot panther-init-screen-iommu.img
```

**SHA-256 for `panther-init-screen-iommu.img`**

```text
f060a766890aa1f62cdf6de61cece1796c26448e26ed55f54722c511acf5c777
```

[UBports porting documentation](https://docs.ubports.com/en/latest/porting/introduction/Preparations.html)
