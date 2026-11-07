#!/usr/bin/env bash
# Read-only survey of a Galaxy S7 edge (SM-G935F, hero2lte) booted into TWRP.
# Writes nothing to the phone. Run from the PC with adb in PATH:
#
#   tools/hero2lte-collect.sh OUTDIR
#
# OUTDIR gets: device identity, partition map, which touch IC TWRP's kernel
# found (sec_ts at 0x48 or STMicro FTS at 0x49), the kernel command line from
# S-Boot, the live device tree, and a copy of BOOT (needed by
# herolte-pack-boot.py; it is your way back to Android).
#
# OUTDIR contains serial numbers and per-device data: keep it out of git.
set -euo pipefail

out=${1:?usage: hero2lte-collect.sh OUTDIR}
mkdir -p "$out"
sh() { adb shell "$@" 2>/dev/null | tr -d '\r'; }

adb get-state >/dev/null || { echo "no adb device (boot TWRP first)"; exit 1; }

dev=$(sh getprop ro.product.device)
model=$(sh getprop ro.product.model)
boot_ver=$(sh getprop ro.boot.bootloader)
echo "device=$dev model=$model bootloader=$boot_ver" | tee "$out/identity.txt"
case "$dev$model" in
	*hero2lte*|*G935*) ;;
	*) echo "warning: this does not look like a hero2lte / SM-G935F" ;;
esac

byname=/dev/block/platform/155a0000.ufs/by-name
sh "ls -l $byname" > "$out/by-name.txt"
for p in BOOT RECOVERY USERDATA EFS; do
	blk=$(sh "readlink -f $byname/$p")
	size=$(sh "blockdev --getsize64 $blk")
	echo "$p $blk $size" | tee -a "$out/partitions.txt"
done

sh "cat /proc/cmdline" > "$out/cmdline.txt"
sh "cat /proc/cpuinfo" > "$out/cpuinfo.txt"
sh "cat /proc/iomem" > "$out/iomem.txt" || true
sh "cat /proc/bus/input/devices" > "$out/input-devices.txt" || true
sh "dmesg" > "$out/dmesg.txt" || true
sh "cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/stride" > "$out/fb0.txt" || true

# Which touch controller did the recovery kernel bind?
{
	echo "# i2c devices"
	sh "ls /sys/bus/i2c/devices/" || true
	for d in $(sh "ls /sys/bus/i2c/devices/" ); do
		n=$(sh "cat /sys/bus/i2c/devices/$d/name")
		echo "$d $n"
	done
	echo "# dmesg touch lines"
	grep -iE 'sec_ts|fts|stm,|tsp|touch' "$out/dmesg.txt" | head -60 || true
} > "$out/touch.txt"
if grep -qiE '\b(fts|stm)' "$out/touch.txt"; then
	echo "touch: STMicro FTS lines found -> expect herotouchic auto to fall through to 0x49"
elif grep -qi 'sec_ts' "$out/touch.txt"; then
	echo "touch: sec_ts lines found -> same controller as the flat S7"
else
	echo "touch: undetermined (see $out/touch.txt)"
fi

# Live device tree, as S-Boot passed it.
if sh "test -r /sys/firmware/fdt && echo y" | grep -q y; then
	adb exec-out "cat /sys/firmware/fdt" > "$out/live.dtb"
else
	sh "cd /proc/device-tree && tar cf /tmp/dt.tar ." && adb pull /tmp/dt.tar "$out/device-tree.tar" >/dev/null
	sh "rm -f /tmp/dt.tar"
fi

# BOOT backup (read only).
blk=$(sh "readlink -f $byname/BOOT")
sh "dd if=$blk of=/tmp/boot-stock.img bs=1048576"
adb pull /tmp/boot-stock.img "$out/boot-stock.img" >/dev/null
sh "rm -f /tmp/boot-stock.img"
sha256sum "$out/boot-stock.img" | tee "$out/boot-stock.sha256"
if [ -f "$(dirname "$0")/herolte-pack-boot.py" ]; then
	python3 "$(dirname "$0")/herolte-pack-boot.py" info "$out/boot-stock.img" \
		> "$out/boot-inspect.txt" 2>&1 || true
	cat "$out/boot-inspect.txt"
fi
echo "done: $out"
