# hero2lte modem (cellular): stage 1 research

Status 2026-11-16: research only, nothing run on the phone. Sources: LineageOS
`android_kernel_samsung_universal8890` (`drivers/misc/modem_v1`, `drivers/misc/mcu_ipc`,
`arch/arm64/boot/dts/modem-ss335ap-pdata.dtsi`), tonyg's `qnd_cbd.py` (squeak-phone, a working
cbd replacement for the flat S7, SM-G930F), Replicant's libsamsung-ipc, SSTIC 2020 / Comsecuris /
FirmWire material on Shannon.

## Hardware

- Modem: Samsung Shannon 335 (`mif,name = "ss335ap"`, driver type SEC_SS310AP), a Cortex-R7 inside the
  Exynos 8890. No external chip, no USB/HSIC link: AP and CP share DRAM.
- Shared memory: `0xF0000000`, size `0x8800000` (136 MiB). The CP image lives in the first `0x8000000`;
  the IPC region is at `+0x8000000`, size `0x800000`. Our RAM-bank table already stops at `0xF0000000`
  and resumes at `0xF8800000` (`tools/herolte-adt-ram-banks.py`), so iOS never uses it.
- Mailbox: `mcu_ipc@10540000` (interrupt and value registers AP<->CP; numbers in the pdata dtsi:
  `mbx_ap2cp_msg 0`, `mbx_cp2ap_msg 1`, ... `mbx_ap2cp_et_dac_cal 32`).
- CP power and reset go through the secure monitor, not direct PMU writes: `exynos_smc(0x82000700,
  READ_CTRL 3 / WRITE_CTRL 4, value, reg)` (`pmu-cp.c`).

## Boot (what cbd does; qnd_cbd.py reproduces it)

1. Read the TOC (first 512 bytes of the `RADIO` partition): entries `'<12sIIIII'` (name, offset,
   loadaddr, size, crc, entryid); entries 1..3 are BOOT, MAIN, NV.
2. `modem_reset`, then security request mode 2 (re-init): `exynos_smc(SMC_ID, mode, param2, param3)`
   (`link_device_memory_main.c`; which SMC_ID that file sees is still to confirm); params from `shm_get_security_param2/3`.
3. Copy BOOT and MAIN from `RADIO` into shared memory at `loadaddr - BOOT.loadaddr`, in chunks.
4. Copy NV: **from `/efs/nv_data.bin`** (EFS partition), same placement rule.
5. Security request mode 0 (normal) with the BOOT and MAIN sizes: the secure world checks/unlocks the image.
6. `ss310ap_on`: mailbox init, DRAM calibration (0x800 bytes from sysram_alive) to IPC + 0x1000,
   ET_DAC_CAL via `exynos_smc(0x82001014, 0, 0x2002, 0x1C)` into mailbox 32, `pda_active = 1`,
   CP power on via SMC, manual reset release.
7. `boot_on`, `dl_start`, handshake on the boot channel (write `0x0000900d`, read; write `0x00009f00`,
   read), `boot_off`. The CP then talks SIPC 5.0 over the IPC region.

## After boot

- Channels (iodevs) over the shared-buffer-descriptor rings: `umts_ipc0/1` (IPC_FMT: the Samsung IPC
  protocol: power, SIM, network registration, SMS, calls, GPRS), `umts_rfs0` (remote file system: the CP
  reads **and writes** its NV files through it), `umts_csd`, raw-IP channels for packet data (`rmnet0..7`).
- The FMT messages are the Samsung IPC protocol that Replicant's libsamsung-ipc implements (header:
  length, mseq, aseq, group, index, type). libsamsung-ipc boots older modems (XMM626x) but not the S7's;
  its message layer is the best public reference. Shannon may add or change messages.

## Plan for this port

- **Kernel (small, once):** a device node exposing only what a userland loader needs: map of the
  `0xF0000000` window, the mailbox registers and interrupts, and the specific SMC calls above
  (allow-listed IDs only). Everything else in user space, so iteration needs no kernel rebuilds.
- **Userland `herocbd`:** qnd_cbd's sequence in C, firmware read-only from `RADIO`.
- **NV data / EFS (rule: never write EFS):** the CP needs `nv_data.bin` from EFS at boot and writes
  NV back through RFS. Plan: TWRP copies `/efs/nv_data.bin` once, on the phone, into the drop zone
  (never to the PC); RFS writes go to that copy only. Needs the user's explicit OK before it runs.
- **Then:** SIPC5 framing, Samsung IPC for SIM + registration, a PDP context, and a network interface
  for iOS (stage 4). Calls/SMS are out of scope.

## Risks

- The CP writes anywhere in its 136 MiB window: the window must stay out of every RAM bank (it is).
- A wrong security request may make the secure monitor refuse (error 11 as root, per qnd_cbd; cbd
  drops to uid 1001 first). Unknown whether that matters when called from the kernel.
- The radio transmits once the CP registers on a network; that is what it does under Android too.
