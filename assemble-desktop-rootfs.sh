#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 3 ]]; then
    printf 'Usage: %s ISO_LAYER_DIR QEMU_AARCH64 NEW_OUTPUT_DIR\n' "$0" >&2
    exit 2
fi
root=$(cd -- "$(dirname -- "$0")" && pwd)
layers=$(realpath -e -- "$1")
qemu=$(realpath -e -- "$2")
out=$(realpath -m -- "$3")
kernel="$root/kernel-baseline-artifacts"
torch="$root/driver-fixes-20261119/torch-quick-settings"
dtc=${DTC:-dtc}
fdtoverlay=${FDTOVERLAY:-fdtoverlay}
command -v "$dtc" >/dev/null
command -v "$fdtoverlay" >/dev/null

(cd "$layers" && sha256sum --strict -c SHA256SUMS)
(cd "$kernel" && sha256sum --strict -c SHA256SUMS)
mkdir -m 700 -- "$out"
mkdir -p -- "$out/initramfs/bin" "$out/inputs/firmware"
unsquashfs -cat "$layers/minimal.squashfs" usr/bin/busybox > "$out/initramfs/bin/busybox"
chmod 755 "$out/initramfs/bin/busybox"
install -m 755 "$root/desktop-assembly-init.sh" "$out/initramfs/init"
(cd "$out/initramfs" && find . -print0 | cpio -o --null -H newc --owner=0:0) | gzip -n > "$out/assembly-initrd.gz"
gzip -dc "$kernel/Image.gz" > "$out/inputs/vmlinuz"
"$dtc" -I dts -O dtb -o "$out/inputs/opd2513-flash.dtbo" "$torch/opd2513-flash.dtso"
"$fdtoverlay" -i "$kernel/kaanapali-oneplus-iceland.dtb" \
    -o "$out/inputs/board-reference.dtb" "$out/inputs/opd2513-flash.dtbo"
cp -- "$kernel"/*-modules.tar.zst "$out/inputs/modules.tar.zst"
zstd -qdf "$out/inputs/modules.tar.zst" -o "$out/inputs/modules.tar"
install -m 644 "$root/local-firmware/novatek/novatek_nt36536_fw.bin" "$out/inputs/firmware/"
# Qualcomm WCN7860 Bluetooth firmware requested by btqca on OPD2513.
# Keep these in the generated Ubuntu rootfs; copying them manually after boot
# made Bluetooth appear fixed only until the next rootfs rebuild.
install -d -m 755 "$out/inputs/firmware/qca"
install -m 644 "$root/local-firmware/qca/gngbtfw20.mbn" "$out/inputs/firmware/qca/"
install -m 644 "$root/local-firmware/qca/gngbtnv20.bin" "$out/inputs/firmware/qca/"
gpu_fw="$root/local-firmware/qcom/kaanapali/iceland/gen80200_zap.mbn"
printf '%s  %s\n' 5f403a0b169bead5daae695bef72daa1a95485f9123c2678d36c974b1c92f1f3 "$gpu_fw" | sha256sum --strict -c -
install -D -m 644 "$gpu_fw" "$out/inputs/firmware/qcom/kaanapali/iceland/gen80200_zap.mbn"
# Stage the remoteproc firmware required by desktop-assembly-init.sh.
for fw in adsp.mbn adsp_dtb.mbn cdsp.mbn cdsp_dtb.mbn; do
    install -m 644 "$root/local-firmware/qcom/kaanapali/iceland/$fw" \
        "$out/inputs/firmware/qcom/kaanapali/iceland/$fw"
done
install -d -m 755 "$out/inputs/torch/extension"
install -m 644 "$torch"/{install.sh,90-opd2513-torch.gschema.override,opd2513-torch,opd2513-torch.socket,opd2513-torch@.service} "$out/inputs/torch/"
install -m 644 "$torch"/extension/{extension.js,metadata.json,stylesheet.css,torch-symbolic.svg} "$out/inputs/torch/extension/"
mke2fs -q -t ext4 -d "$out/inputs" "$out/inputs.ext4" 192M
mke2fs -q -t ext4 -L OPD2513_STAGE "$out/ubuntu-desktop-root.ext4" 12G
[[ $(blkid -p -s LABEL -o value "$out/ubuntu-desktop-root.ext4") == OPD2513_STAGE ]]

drive() {
    printf 'file=%s,if=none,format=raw,id=%s,readonly=%s' "${1//,/,,}" "$2" "$3"
}
timeout 900 "$qemu" -machine virt,gic-version=3 -cpu max -m 2048 -smp 4 \
    -nodefaults -no-reboot -display none -monitor none -serial stdio -nic none \
    -kernel "$out/inputs/vmlinuz" -initrd "$out/assembly-initrd.gz" \
    -append 'console=ttyAMA0 rdinit=/init loglevel=3 panic=5' \
    -drive "$(drive "$layers/minimal.squashfs" lower on)" -device virtio-blk-device,drive=lower,serial=op-base \
    -drive "$(drive "$layers/minimal.standard.squashfs" upper on)" -device virtio-blk-device,drive=upper,serial=op-standard \
    -drive "$(drive "$out/inputs.ext4" inputs on)" -device virtio-blk-device,drive=inputs,serial=op-input \
    -drive "$(drive "$out/ubuntu-desktop-root.ext4" output off)" -device virtio-blk-device,drive=output,serial=op-output \
    | tee "$out/assembly.log"
rg -qx 'OPD2513_DESKTOP_ASSEMBLY_OK\r?' "$out/assembly.log"
e2fsck -fn "$out/ubuntu-desktop-root.ext4"
(cd "$out" && sha256sum ubuntu-desktop-root.ext4 > SHA256SUMS)
