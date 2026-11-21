#!/bin/busybox sh
set -eu
export PATH=/bin
/bin/busybox --install -s /bin
trap 'echo OPD2513_DESKTOP_ASSEMBLY_FAILED; poweroff -f' EXIT
mkdir -p /dev /proc /sys /base /standard /scratch /merged /input /target
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
disk() {
    for dev in /sys/class/block/vd*; do
        if [ -r "$dev/serial" ] && [ "$(cat "$dev/serial")" = "$1" ]; then
            printf '/dev/%s\n' "${dev##*/}"
            return
        fi
    done
    printf 'Missing virtual disk: %s\n' "$1" >&2
    return 1
}
base=$(disk op-base)
standard=$(disk op-standard)
input=$(disk op-input)
output=$(disk op-output)
for dev in "$base" "$standard" "$input"; do
    test "$(cat "/sys/class/block/${dev##*/}/ro")" = 1
done
test "$(cat "/sys/class/block/${output##*/}/ro")" = 0
mount -t squashfs -o ro "$base" /base
mount -t squashfs -o ro "$standard" /standard
mount -t ext4 -o ro "$input" /input
mount -t ext4 "$output" /target
mount -t tmpfs -o size=64m tmpfs /scratch
mkdir /scratch/upper /scratch/work
mount -t overlay -o lowerdir=/standard:/base,upperdir=/scratch/upper,workdir=/scratch/work overlay /merged

printf '%s\n' 'Copying layered Ubuntu Desktop into ext4'
mkdir -p /merged/mnt
mount -o bind /target /merged/mnt
mkfifo /scratch/stream
chroot /merged /usr/bin/tar -c --numeric-owner --xattrs --xattrs-include='*' --acls --sparse \
    --one-file-system --exclude='./mnt' -C / . > /scratch/stream &
writer=$!
chroot /merged /usr/bin/tar -x --numeric-owner --xattrs --xattrs-include='*' --acls \
    --same-owner --same-permissions --warning=no-timestamp -C /mnt < /scratch/stream
wait "$writer"
umount /merged/mnt

printf '%s\n' 'Installing the host-built kernel, modules and local firmware'
mkdir -p /target/boot /target/mnt /target/usr/lib/firmware/novatek /target/usr/lib/firmware/qca /target/usr/lib/firmware/qcom/kaanapali/iceland
ver=$(uname -r)
install -m 644 /input/vmlinuz "/target/boot/vmlinuz-$ver"
install -m 644 /input/board-reference.dtb "/target/boot/board-reference-$ver.dtb"
tar -x -f /input/modules.tar -C /target
install -m 644 /input/firmware/novatek_nt36536_fw.bin /target/usr/lib/firmware/novatek/
# btqca on this OPD2513 kernel requests the WCN7860 files by gng* name.
# Install them into the actual Ubuntu rootfs, not only the staging image.
install -m 644 /input/firmware/qca/gngbtfw20.mbn /target/usr/lib/firmware/qca/
install -m 644 /input/firmware/qca/gngbtnv20.bin /target/usr/lib/firmware/qca/
# The signed ZAP image must match OPD2513 for Adreno hardware rendering.
install -m 644 /input/firmware/qcom/kaanapali/iceland/gen80200_zap.mbn \
    /target/usr/lib/firmware/qcom/kaanapali/iceland/gen80200_zap.mbn
# Firmware for ADSP/CDSP must be present in the installed rootfs so the
# remoteproc drivers can come up during the first boot.
for fw in adsp.mbn adsp_dtb.mbn cdsp.mbn cdsp_dtb.mbn; do
    install -m 644 "/input/firmware/qcom/kaanapali/iceland/$fw" \
        "/target/usr/lib/firmware/qcom/kaanapali/iceland/$fw"
done
printf '%s\n' 'Installing the OPD2513 GNOME flashlight control'
/bin/sh /input/torch/install.sh --root /target
: > /target/etc/machine-id
test "$(stat -c '%u:%g:%a' /target)" = 0:0:755
test -e "/target/usr/lib/modules/$ver/modules.dep"
test "$(chroot /target /usr/bin/dpkg --print-architecture)" = arm64
chroot /target /usr/bin/dpkg-query -W \
    -f='${Package} ${Version} ${db:Status-Abbrev}\n' ubuntu-desktop gnome-shell gdm3 libreoffice-core
sync
umount /target
printf '\n%s\n' OPD2513_DESKTOP_ASSEMBLY_OK
trap - EXIT
poweroff -f
