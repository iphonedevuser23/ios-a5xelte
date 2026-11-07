# hero2lte (Galaxy S7 edge SM-G935F, Exynos 8890): port from herolte

The S7 edge is the same board as the flat S7 (`herolte`, SM-G930F) with a
different panel, battery and possibly a different touch controller. This
port reuses the HEROLTE kernel config, loader, ADT generator and root image
unchanged and adds one runtime-selected patch (`0326`). One kernel boots both
phones.

Evidence is kept separate below: **from sources** (Samsung's DTS) versus
**on the phone** (nothing yet for the edge).

## What differs, from sources

Compared: `exynos8890-herolte_eur_open_08.dts` and
`exynos8890-hero2lte_eur_open_08.dts` from
`LineageOS/android_kernel_samsung_universal8890`, preprocessed, compiled
with `dtc -s` and diffed (25 hunks, phandles normalized).

Identical (so the S7 port's addresses apply as they are): memory nodes and
`reserved-memory`, GIC-400, timer, CPUs/PSCI, UFS `0x155a0000`, DECON_F/DSIM
(the display `.dtsi` files differ only in whitespace and the S7's
`gpio_pcd`), framebuffer handover, gpio-keys (Power GPA2-4, Vol+/Vol-/Home
GPA0-3/4/5), touch bus HSI2C `0x14e10000`, touch INT GPA1-0, touch supplies
(`tsp_io`/`tsp_avdd` = S2MPS16 LDO32/33), MAX77854 fuel gauge on HSI2C_1,
abov touchkey `0x20`, Wi-Fi (BCM4359 PCIe), audio, PMU reset/recovery.

Different:

| Item | S7 (herolte) | S7 edge (hero2lte) | Port impact |
| --- | --- | --- | --- |
| Touch IC | sec_ts @0x48 only (STM @0x49 `disabled`) | **both enabled**: sec_ts @0x48 *or* STMicro FTS @0x49 (`stm,fts_touch`, max_coords 0xfff/0xfff), chosen per unit by TSP-ID GPE7-1 | `0326`: try 0x48, fall back to FTS at 0x49 |
| Battery | 3000 mAh (fuelgauge capacity 0x173b) | 3600 mAh (0x1b5e) | `0326`: boot-arg `herobatmah=3600` |
| Panel | S6E3HA3, 5.1" | S6E3HF4, 5.5" curved; same 1440×2560, same DECON node | none expected (S-Boot initializes it); confirm with the probe |
| GPE7-1 | panel crack detect | touch TSP-ID | not used by the port |
| DTBH hw_rev | 8..8 (rev 08 dts) | 8..10 | none: `herolte-pack-boot.py` keeps your own BOOT's DTBH |
| Charging tables, haptic duty, LED ratios, sensor hub mag matrix, camera mclk drive | — | different | not used by the port today |

Unknown until the phone answers: which touch IC this particular unit has,
partition numbers (S7: BOOT `sda5`, USERDATA `sda18`; the port finds
USERDATA by GPT name, BOOT is written by path), and whether the edge panel
behaves the same under the DECON HW trigger and the 0296 vclk change.

## Patch 0326 (`0326-hero2lte-fts-touch-battery-capacity.patch`)

`bsd/dev/herolte_touch.c`:
- The I²C target address is a variable (`ht_addr`).
- After the touch supplies come up (unchanged APM path), `herotouchic`
  selects the controller: `0` (default) reads sec_ts BOOT_STATUS at 0x48
  and, if nothing ACKs, switches to 0x49; `1` sec_ts only (old behaviour);
  `2` FTS only.
- FTS bring-up as Samsung's `fts_init()`: system reset `B6 00 23 01`, pop
  events with `0x85` until CONTROLLER_READY (`0x10`), chip ID `B6 00 07`
  (expect `39 80` or `39 6c`), SLEEPOUT `0x91`, SENSEON `0x93`,
  FORCECALIBRATION `0xa2`, FLUSHBUFFER `0xa1`, INT enable `B6 00 1C 41`.
  The firmware is not updated (Android's stays in the IC).
- Events: 8 bytes from `0x85` while INT (GPA1-0) is low; ENTER `x3`, LEAVE
  `x4`, MOTION `x5` with the finger in bits 7:4, x = `b1 | (b2 & 0xf) << 8`,
  y = `b2 >> 4 | b3 << 4`, scaled 4096 → 1440×2560 like sec_ts. A
  CONTROLLER_READY mid-session (IC self-reset) releases all fingers and
  re-sends the sense-on sequence. A stuck INT sends FLUSHBUFFER.
- Log lines start with `HEROLTE TOUCH: FTS`.

`iokit/Kernel/arm/SweetBattery.cpp`: the design capacity is
`herobatmah` (1000..10000, default 3000) and is logged as
`HEROLTE BATTERY: design capacity N mAh`. Percent still comes from the
gauge's REPSOC, so this only fixes the mAh numbers iOS shows.

Checked here: both files rebuilt from the series (0062/0211–0278 replayed
in order), `0326` applies on top with `git apply --check`, and the changed
touch driver passes `clang -fsyntax-only -Wall -Wextra` with stub kernel
headers (no warnings). **Not yet built by the xnu workflow and not run on
an edge.**

Known risk: Linux reads FTS registers with a repeated start; this driver
(like the working sec_ts path) sends write + STOP, then read. If the FTS
chip ID reads back as zeros or the READY wait times out while 0x49 ACKs,
that is the first thing to change.

## Bring-up plan on the edge

Rules from `AGENTS.md` still hold: trials write only BOOT; keep the stock
BOOT and a TWRP backup of EFS off the phone; never write EFS, RECOVERY or
the bootloader; USERDATA only after the user confirms.

1. **Survey (read-only).** In TWRP: `tools/hero2lte-collect.sh survey/`.
   Gives identity, partition map, which touch driver TWRP's kernel bound,
   S-Boot's cmdline, the live DTB and `boot-stock.img` (+ `info` of its
   DTBH; must list chip 8890). Take a TWRP backup of EFS too and keep both
   off the phone.
2. **Probe** (`shim/herolte/probe`, packed with `herolte-pack-boot.py`
   from *this phone's* BOOT). Expect, as on the S7: text over the logo, the
   TWRP countdown and an automatic reboot to TWRP. Photo of the screen:
   MIDR/MPIDR, FDT and image addresses, DECON trigger value. If the screen
   stays on the Samsung logo, the edge panel needs a different refresh kick
   (command mode): stop there.
3. **XNU without root** (`cpus=1`, no `rd=`): expect the console to reach
   `Waiting on <dict>IOProviderClass IOMedia ...` as the S7 did.
4. **XNU with the UFS probe**, then the iOS root: same images and commands
   as `docs/herolte-howto.md`, with BOOT/USERDATA paths from step 1 and,
   added to the cmdline, `herobatmah=3600`.
5. **Touch.** Pull the log (`tools/herolte-log-pull.py`) and look for
   `HEROLTE TOUCH:`. sec_ts unit: same lines as the S7. FTS unit:
   `no ACK at 0x48; trying STMicro FTS at 0x49`, `FTS ready`, `FTS chip id
   39 ..`, `FTS sensing at 0x49`, then `FTS id=.. event=3 raw=..` on taps.
   Check the corners map to the right screen corners (the curved edges
   may report coordinates past the flat area).
6. Then the S7's daily line (8 cores, GPU display, Wi-Fi, audio) and
   record each result in `HEROLTE_PROGRESS.md` under a `hero2lte` heading.
