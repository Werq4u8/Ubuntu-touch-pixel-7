# Ubuntu Touch bring-up for Google Pixel 7 (panther)

A custom ARM64 `/init` displays **Hello world and UBPorts community** on a physical Pixel 7 before Android `system` is mounted.

## Roadmap

| Status | Stage | Milestone |
| --- | --- | --- |
| Completed | Early boot and display | Boot the custom ramdisk, bring up the Samsung P10 panel through Exynos DRM/KMS, and display the banner at 1080 × 2400. |
| Current | Ubuntu Touch userspace | Prepare an ARM64 Ubuntu Touch rootfs, mount it from the ramdisk, and hand off to its init. Confirm that userspace starts and capture the boot log. |
| Planned | Graphical session | Bring up the display and touch input in Ubuntu Touch and start the Lomiri shell. |
| Planned | Hardware support | Test and bring up Wi-Fi, Bluetooth, audio, charging, suspend, sensors, camera, and cellular service. |
| Planned | Packaging and testing | Make builds reproducible, document installation and recovery, and test clean installs and updates on `panther`. |

The downloadable image is the completed display demo. It displays the banner and waits.

## Run the display demo

Use the four stock images from **`DP11.261030.003`** and the custom image listed below. Get the stock files from `image-panther_beta-dp11.261030.003.zip` inside the [Google factory package](https://developer.android.com/about/versions/17/qpr3/download).

| Image | Partition |
| --- | --- |
| `boot.img` | `boot` |
| `dtbo.img` | `dtbo` |
| `vendor_boot.img` | `vendor_boot` |
| `vendor_kernel_boot.img` | `vendor_kernel_boot` |
| `panther-init-screen-iommu.img` | `init_boot` |

Flash the images to **a slot of your choice (the inactive slot is recommended)**. Replace `SLOT` with that slot’s letter. If the four matching stock images are already present in that slot, flash only `init_boot`.

```sh
fastboot --slot SLOT flash boot boot.img
fastboot --slot SLOT flash dtbo dtbo.img
fastboot --slot SLOT flash vendor_boot vendor_boot.img
fastboot --slot SLOT flash vendor_kernel_boot vendor_kernel_boot.img
fastboot --slot SLOT flash init_boot panther-init-screen-iommu.img
```

Boot the test slot and watch for the banner after the Google logo.

**SHA-256 for `panther-init-screen-iommu.img`**

```text
f060a766890aa1f62cdf6de61cece1796c26448e26ed55f54722c511acf5c777
```

## Display implementation

The ramdisk loads the PMIC and GPIO drivers for the panel, Samsung DMA heap for framebuffer allocation, and Samsung IOMMU modules before Exynos DRM. The early init then draws the banner and submits the framebuffer through KMS.

[UBports porting documentation](https://docs.ubports.com/en/latest/porting/introduction/Preparations.html)
