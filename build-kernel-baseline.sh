#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 1 ]]; then
    printf 'Usage: %s BUILD_DIR\n' "$0" >&2
    exit 2
fi
root=$(cd -- "$(dirname -- "$0")" && pwd)
src="$root/iceland-kernel"
out=$(realpath -m -- "$1")
want=b9e356dfbd7a8c70a891b0ef819ba5f5eff1774c
[[ $(git -C "$src" rev-parse HEAD) == "$want" ]]
[[ -z $(git -C "$src" status --porcelain) ]]
mkdir -p -- "$out"
export ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
export CC="${TARGET_CC:-${CROSS_COMPILE}gcc}"
mk() { make -C "$src" O="$out" CC="$CC" -j"${JOBS:-12}" "$@"; }

mk defconfig
"$src/scripts/kconfig/merge_config.sh" -m -O "$out" "$out/.config" \
    "$src/arch/arm64/configs/sm8850.config" "$root/kernel-baseline.config"
mk olddefconfig
while IFS= read -r line; do
    case "$line" in
        CONFIG_*=n) expected="# ${line%=n} is not set" ;;
        CONFIG_*=*) expected=$line ;;
        *) continue ;;
    esac
    if ! grep -Fxq -- "$expected" "$out/.config"; then
        printf 'Config not satisfied: %s\n' "$line" >&2
        exit 1
    fi
done < "$root/kernel-baseline.config"
mk Image.gz qcom/kaanapali-oneplus-iceland.dtb modules
printf 'Built %s (reference only; no boot image or tablet writes)\n' "$out"
