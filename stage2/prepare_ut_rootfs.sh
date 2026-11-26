#!/usr/bin/env bash
# prepare_ut_rootfs.sh - Prepare compressed EROFS ARM64 Ubuntu Touch rootfs for Pixel 7 (panther)
# Fits easily inside 1.5GB system_a while keeping slot B Android and userdata untouched.
set -euo pipefail

WORK_DIR="${1:-./ut_rootfs_work}"
OUT_IMG="${2:-./ut-rootfs-system.img}"

NOBLE_URL="https://ci.ubports.com/job/ubuntu-touch-rootfs/job/ubports%252F24.04-1.x/lastSuccessfulBuild/artifact/ubuntu-touch-android9plus-rootfs-arm64.tar.gz"
FOCAL_URL="https://ci.ubports.com/job/ubuntu-touch-rootfs/job/ubports%252Ffocal/lastSuccessfulBuild/artifact/ubuntu-touch-android9plus-rootfs-arm64.tar.gz"

ROOT_DIR="$WORK_DIR/rootfs"
rm -rf "$ROOT_DIR"
mkdir -p "$ROOT_DIR"
TARBALL="$WORK_DIR/ut-rootfs-arm64.tar.gz"

if [ ! -f "$TARBALL" ]; then
    echo "[1/4] Downloading Ubuntu Touch ARM64 rootfs..."
    if ! curl -fL --retry 3 "$NOBLE_URL" -o "$TARBALL"; then
        echo "[!] Noble URL failed, falling back to Focal..."
        curl -fL --retry 3 "$FOCAL_URL" -o "$TARBALL"
    fi
fi

echo "[2/4] Extracting rootfs..."
tar --numeric-owner -xzpf "$TARBALL" -C "$ROOT_DIR"

echo "[3/4] Applying Pixel 7 (panther) Stage-2 debug & boot configuration..."
echo "ubuntu-phablet" > "$ROOT_DIR/etc/hostname"

if [ ! -e "$ROOT_DIR/sbin/init" ] && [ -e "$ROOT_DIR/lib/systemd/systemd" ]; then
    ln -sf /lib/systemd/systemd "$ROOT_DIR/sbin/init"
fi

mkdir -p "$ROOT_DIR/etc/systemd/system.conf.d"
cat > "$ROOT_DIR/etc/systemd/system.conf.d/99-panther-debug.conf" << 'EOF'
[Manager]
LogLevel=debug
LogTarget=kmsg
DumpCore=yes
EOF

mkdir -p "$ROOT_DIR/usr/local/sbin"
cat > "$ROOT_DIR/usr/local/sbin/panther-usb-debug.sh" << 'EOF'
#!/bin/sh
set -e
exec >> /dev/kmsg 2>&1
echo "panther-usb-debug: starting"
mount -t configfs none /sys/kernel/config 2>/dev/null || true
G=/sys/kernel/config/usb_gadget/g1
if [ ! -d "$G" ]; then
    mkdir -p "$G"
    echo 0x18d1 > "$G/idVendor"
    echo 0x4ee7 > "$G/idProduct"
    mkdir -p "$G/strings/0x409" "$G/configs/c.1/strings/0x409" "$G/functions/rndis.usb0"
    echo "panther-ut" > "$G/strings/0x409/serialnumber"
    echo "Google" > "$G/strings/0x409/manufacturer"
    echo "Pixel 7 UT" > "$G/strings/0x409/product"
    echo "rndis" > "$G/configs/c.1/strings/0x409/configuration"
    ln -sf "$G/functions/rndis.usb0" "$G/configs/c.1/"
    UDC=$(ls /sys/class/udc | head -n 1)
    [ -n "$UDC" ] && echo "$UDC" > "$G/UDC" || true
fi
ip link set usb0 up 2>/dev/null || true
ip addr add 10.15.19.82/24 dev usb0 2>/dev/null || true
EOF
chmod +x "$ROOT_DIR/usr/local/sbin/panther-usb-debug.sh"

mkdir -p "$ROOT_DIR/etc/systemd/system/multi-user.target.wants"
cat > "$ROOT_DIR/etc/systemd/system/panther-usb-debug.service" << 'EOF'
[Unit]
Description=Pixel 7 Early USB RNDIS Debug Network
DefaultDependencies=no
After=systemd-udevd.service
Before=network.target ssh.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/panther-usb-debug.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
ln -sf /etc/systemd/system/panther-usb-debug.service "$ROOT_DIR/etc/systemd/system/multi-user.target.wants/panther-usb-debug.service"

cat > "$ROOT_DIR/etc/fstab" << 'EOF'
proc /proc proc defaults 0 0
sysfs /sys sysfs defaults 0 0
tmpfs /tmp tmpfs defaults 0 0
EOF

echo "[4/4] Building EROFS image (lz4hc): $OUT_IMG"
mkfs.erofs -zlz4hc "$OUT_IMG" "$ROOT_DIR"
ls -lh "$OUT_IMG"
