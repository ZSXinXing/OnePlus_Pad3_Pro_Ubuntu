#!/bin/busybox sh
set -eu
export PATH=/bin
/bin/busybox --install -s /bin
trap 'echo OPD2513_DESKTOP_UPDATE_FAILED; poweroff -f' EXIT
mkdir -p /dev /proc /sys /target
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys

test "$(cat /sys/class/block/vda/serial)" = op-root-copy
test "$(cat /sys/class/block/vda/ro)" = 0
mount -t ext4 /dev/vda /target
ver=$(uname -r)
test "$ver" = "$(cat /payload/release)"
test -f /target/etc/os-release
test ! -e "/target/usr/lib/modules/$ver"
install -m 644 /payload/Image "/target/boot/vmlinuz-$ver"
tar -xf /payload/modules.tar -C /target/usr/lib/modules
tar -xf /payload/firmware.tar -C /target/usr/lib/firmware
test -s "/target/usr/lib/modules/$ver/modules.dep"
(cd /target/usr/lib/firmware && sha256sum -c /payload/firmware.sha256)
if [ -f /payload/torch/install.sh ]; then
    /bin/sh /payload/torch/install.sh --root /target
fi
chroot /target /usr/bin/dpkg-query -W ubuntu-desktop gnome-shell gdm3
sync
umount /target
echo OPD2513_DESKTOP_UPDATE_OK
trap - EXIT
poweroff -f
