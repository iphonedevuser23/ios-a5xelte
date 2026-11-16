/*
 * hero2lte herocbd: the modem loader (docs/herolte-modem.md). Stage 2, step 1.
 *
 *   herocbd nv      NV data: check the slot (HERONV01) and make the working copy, nothing else
 *   herocbd toc     NV as above, and the RADIO partition's TOC
 *   herocbd boot    all that, then load BOOT/MAIN/NV, ask the secure world, power the CP on and watch it
 *
 * Talks only to /dev/hakios-modem (0363/0364) and /dev/rufsroot0 (the NV slot). Never touches EFS.
 */
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DKIOCGETBLOCKSIZE  _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT _IOR('d', 25, uint64_t)

struct hm_smc { uint64_t x[4]; };
struct hm_reg { uint32_t block, write, offset, value; };
struct hm_radio { uint64_t block, buffer; uint32_t count, pad; uint64_t nblocks; };
#define HMIO_SMC   _IOWR('h', 1, struct hm_smc)
#define HMIO_REG   _IOWR('h', 2, struct hm_reg)
#define HMIO_RADIO _IOWR('h', 3, struct hm_radio)

#define BLK          4096u
#define LOG_BLOCKS   (1u + (4u << 20) / BLK)
#define NV_BLOCKS    1024u
#define IPC_OFF      0x8000000u       /* the IPC region inside the shared memory */
#define WORKDIR      "/private/var/herolte/modem"
#define NV_COPY      WORKDIR "/nv_data.bin"

#define SMC_CP       0x82000700ULL
#define CP_CTRL_S    0
#define CP_CTRL_NS   1
#define CP_PWRON     (1u << 1)
#define CP_RESET_SET (1u << 2)
#define CP_START     (1u << 3)

static int md = -1;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void
say(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	printf("herocbd: ");
	vprintf(fmt, a);
	printf("\n");
	fflush(stdout);
	va_end(a);
}

static int
radio_read(uint64_t byte_off, void *dst, size_t len, uint64_t *nblocks)
{
	/* whole blocks through the kernel, copied out of a bounce buffer */
	static uint8_t bounce[256 * BLK];
	uint8_t *out = dst;
	while (len > 0 || nblocks) {
		uint64_t blk = byte_off / BLK;
		uint32_t skip = (uint32_t)(byte_off % BLK);
		size_t want = len + skip;
		uint32_t count = (uint32_t)((want + BLK - 1) / BLK);
		if (count > 256) {
			count = 256;
		}
		struct hm_radio r = { .block = blk, .buffer = (uint64_t)(uintptr_t)bounce, .count = len ? count : 0 };
		if (ioctl(md, HMIO_RADIO, &r) != 0) {
			say("RADIO read at block %llu: %s", blk, strerror(errno));
			return -1;
		}
		if (nblocks) {
			*nblocks = r.nblocks;
			nblocks = NULL;
		}
		if (!len) {
			break;
		}
		size_t got = (size_t)count * BLK - skip;
		if (got > len) {
			got = len;
		}
		memcpy(out, bounce + skip, got);
		out += got;
		len -= got;
		byte_off += got;
	}
	return 0;
}

static uint64_t
smc(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t *x2_out)
{
	struct hm_smc s = { { x0, x1, x2, x3 } };
	if (ioctl(md, HMIO_SMC, &s) != 0) {
		say("SMC 0x%llx/%llu refused: %s", x0, x1, strerror(errno));
		return (uint64_t)-1;
	}
	if (x2_out) {
		*x2_out = s.x[2];
	}
	return s.x[0];
}

static uint32_t cp_read(int reg) { return (uint32_t)smc(SMC_CP, 3, 0, (uint64_t)reg, NULL); }
static uint64_t cp_write(int reg, uint32_t v) { return smc(SMC_CP, 4, v, (uint64_t)reg, NULL); }

static uint32_t
reg_rw(uint32_t block, uint32_t off, int write, uint32_t v)
{
	struct hm_reg r = { block, (uint32_t)write, off, v };
	if (ioctl(md, HMIO_REG, &r) != 0) {
		say("register %u/0x%x: %s", block, off, strerror(errno));
		return 0xffffffffu;
	}
	return r.value;
}
static uint32_t mbox_get(unsigned n) { return reg_rw(1, 0x80 + 4 * n, 0, 0); }
static void mbox_set(unsigned n, uint32_t v) { reg_rw(1, 0x80 + 4 * n, 1, v); }

static int
shm_write(uint64_t off, const void *src, size_t len)
{
	const uint8_t *p = src;
	while (len) {
		size_t n = len > (1u << 20) ? (1u << 20) : len;
		ssize_t w = pwrite(md, p, n, (off_t)off);
		if (w != (ssize_t)n) {
			say("shared memory write at 0x%llx: %s", off, w < 0 ? strerror(errno) : "short");
			return -1;
		}
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static void
hex(const uint8_t *d, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++) {
		sprintf(out + 2 * i, "%02x", d[i]);
	}
}

/* NV: the slot below the drop package (TWRP wrote it from EFS, read only), copied once into WORKDIR. */
static int
nv_prepare(void)
{
	struct stat st;
	if (stat(NV_COPY, &st) == 0 && st.st_size > 0) {
		say("NV: working copy %s, %lld bytes (kept from an earlier boot)", NV_COPY, (long long)st.st_size);
		return 0;
	}
	int d = open("/dev/rufsroot0", O_RDONLY);
	if (d < 0) {
		say("NV: /dev/rufsroot0: %s", strerror(errno));
		return -1;
	}
	uint64_t count = 0;
	uint32_t bs = 0;
	ioctl(d, DKIOCGETBLOCKCOUNT, &count);
	ioctl(d, DKIOCGETBLOCKSIZE, &bs);
	uint64_t total = count * bs, slot = total / BLK - LOG_BLOCKS - NV_BLOCKS;
	char head[BLK + 1] = {0};
	if (pread(d, head, BLK, (off_t)(slot * BLK)) != BLK || strncmp(head, "HERONV01 ", 9) != 0) {
		say("NV: no slot at block %llu of %llu (run the TWRP NV script first)", slot, total / BLK);
		close(d);
		return -1;
	}
	unsigned long size = 0;
	char want[65] = {0};
	if (sscanf(head + 9, "%lu %64s", &size, want) != 2 || size == 0 || size > (NV_BLOCKS - 1) * BLK) {
		say("NV: bad slot header");
		close(d);
		return -1;
	}
	uint8_t *buf = malloc((size + BLK - 1) / BLK * BLK);
	ssize_t got = pread(d, buf, (size + BLK - 1) / BLK * BLK, (off_t)((slot + 1) * BLK));
	close(d);
	uint8_t sum[CC_SHA256_DIGEST_LENGTH];
	char sumhex[65];
	CC_SHA256(buf, (CC_LONG)size, sum);
	hex(sum, sizeof(sum), sumhex);
	if (got < (ssize_t)size || strcmp(sumhex, want) != 0) {
		say("NV: slot checksum mismatch (%s, want %s)", sumhex, want);
		free(buf);
		return -1;
	}
	mkdir(WORKDIR, 0700);
	int f = open(NV_COPY, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (f < 0 || write(f, buf, size) != (ssize_t)size) {
		say("NV: %s: %s", NV_COPY, strerror(errno));
		free(buf);
		return -1;
	}
	close(f);
	free(buf);
	say("NV: slot ok (%lu bytes, sha256 %.16s...), working copy made", size, sumhex);
	return 0;
}

struct toc {
	char name[12];
	uint32_t offset, loadaddr, size, crc, entryid;
};

static int
read_toc(struct toc *t, int *n)
{
	uint8_t b[512];
	uint64_t nblocks = 0;
	if (radio_read(0, b, sizeof(b), &nblocks) != 0) {
		return -1;
	}
	say("RADIO: %llu MiB", nblocks * BLK >> 20);
	*n = 0;
	for (int i = 0; i < 16; i++) {
		memcpy(&t[i], b + 32 * i, 32);
		if (t[i].name[0] == 0) {
			break;
		}
		char name[13] = {0};
		memcpy(name, t[i].name, 12);
		say("TOC %d: %-8s offset 0x%08x load 0x%08x size 0x%08x crc 0x%08x id %u", i, name, t[i].offset,
		    t[i].loadaddr, t[i].size, t[i].crc, t[i].entryid);
		(*n)++;
	}
	return *n >= 4 && strncmp(t[0].name, "TOC", 3) == 0 ? 0 : -1;
}

static int
load_from_radio(const struct toc *e, uint32_t base)
{
	uint8_t *buf = malloc(4u << 20);
	for (uint32_t done = 0; done < e->size;) {
		uint32_t n = e->size - done > (4u << 20) ? (4u << 20) : e->size - done;
		if (radio_read((uint64_t)e->offset + done, buf, n, NULL) != 0 ||
		    shm_write((uint64_t)(e->loadaddr - base) + done, buf, n) != 0) {
			free(buf);
			return -1;
		}
		done += n;
	}
	free(buf);
	return 0;
}

static void
watch(int seconds)
{
	uint32_t last_status = 0xffffffffu, last_msg = 0xffffffffu;
	for (int t = 0; t <= seconds * 10; t++) {
		uint32_t status = mbox_get(5), msg = mbox_get(1), ns = (t % 10 == 0) ? cp_read(CP_CTRL_NS) : 0;
		if (status != last_status || msg != last_msg || t % 50 == 0) {
			uint32_t magic[4] = {0};
			pread(md, magic, sizeof(magic), IPC_OFF);
			say("t=%d.%ds cp_status(mbox5) 0x%x cp2ap_msg(mbox1) 0x%x CP_CTRL_NS 0x%x ipc[0..3] %08x %08x %08x %08x",
			    t / 10, t % 10, status, msg, ns, magic[0], magic[1], magic[2], magic[3]);
			last_status = status;
			last_msg = msg;
		}
		usleep(100000);
	}
}

int
main(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "toc";
	say("%s (pid %d)", what, getpid());
	md = open("/dev/hakios-modem", O_RDWR);
	if (md < 0) {
		say("/dev/hakios-modem: %s (kernel without 0363/0364?)", strerror(errno));
		return 1;
	}
	int nv_ok = nv_prepare() == 0;
	if (!strcmp(what, "nv")) {
		return nv_ok ? 0 : 1;
	}
	struct toc t[16];
	int n = 0;
	if (read_toc(t, &n) != 0) {
		say("RADIO: no usable TOC");
		return 1;
	}
	say("CP_CTRL_NS 0x%x CP_CTRL_S 0x%x, mailbox cp_status 0x%x", cp_read(CP_CTRL_NS), cp_read(CP_CTRL_S), mbox_get(5));
	if (strcmp(what, "boot") != 0) {
		return 0;
	}
	if (!nv_ok) {
		say("boot: no NV data, not starting the modem");
		return 1;
	}
	const struct toc *boot = &t[1], *mainfw = &t[2], *nv = &t[3];
	if (strncmp(boot->name, "BOOT", 4) || strncmp(mainfw->name, "MAIN", 4) || strncmp(nv->name, "NV", 2)) {
		say("boot: TOC is not BOOT/MAIN/NV, stopping");
		return 1;
	}

	/* reset if it runs (ss310ap_reset / exynos_cp_reset) */
	uint32_t ns = cp_read(CP_CTRL_NS);
	if (ns & CP_PWRON) {
		say("CP is powered (0x%x): reset", ns);
		static const uint32_t conf[] = { 0x0280, 0x1170, 0x11D0, 0x11D8, 0x11D4, 0x11CC };
		for (unsigned i = 0; i < 6; i++) {
			reg_rw(0, conf[i], 1, 0);
		}
		cp_write(CP_CTRL_NS, cp_read(CP_CTRL_NS) | CP_RESET_SET);
		usleep(100);
	}
	say("security request re-init -> 0x%llx", smc(SMC_CP, 2, 0, 0, NULL));

	if (load_from_radio(boot, boot->loadaddr) || load_from_radio(mainfw, boot->loadaddr)) {
		return 1;
	}
	say("BOOT and MAIN loaded");
	int f = open(NV_COPY, O_RDONLY);
	struct stat st;
	fstat(f, &st);
	size_t nvlen = (size_t)st.st_size < nv->size ? (size_t)st.st_size : nv->size;
	uint8_t *nvbuf = malloc(nvlen);
	if (f < 0 || read(f, nvbuf, nvlen) != (ssize_t)nvlen || shm_write(nv->loadaddr - boot->loadaddr, nvbuf, nvlen)) {
		say("NV load failed");
		return 1;
	}
	close(f);
	free(nvbuf);
	say("NV loaded (%zu bytes at +0x%x)", nvlen, nv->loadaddr - boot->loadaddr);

	say("security request normal (boot 0x%x, main 0x%x) -> 0x%llx", boot->size, mainfw->size,
	    smc(SMC_CP, 0, boot->size, mainfw->size, NULL));

	/* ss310ap_on */
	for (unsigned i = 0; i < 64; i++) {
		mbox_set(i, 0);
	}
	mbox_set(33, 9 | (0 << 8));             /* sys_rev (androidboot.hw_rev=9), ds_det 0 (single SIM) */
	uint32_t cal[0x200];
	for (unsigned i = 0; i < 0x200; i++) {
		cal[i] = reg_rw(2, 4 * i, 0, 0);
	}
	shm_write(IPC_OFF + 0x1000, cal, sizeof(cal));
	uint64_t x2 = 0;
	uint64_t r = smc(0x82001014ULL, 0, 0x2002, 0x1C, &x2);
	uint32_t dac = (uint32_t)((x2 & 0xFFFF0000ULL) >> 16);
	mbox_set(32, dac);
	say("DRAM calibration copied, ET_DAC_CAL 0x%x (smc 0x%llx)", dac, r);
	mbox_set(6, 1);                         /* pda_active */
	ns = cp_read(CP_CTRL_NS);
	if (!(ns & CP_PWRON)) {
		cp_write(CP_CTRL_NS, ns | CP_PWRON);
	}
	uint32_t s = cp_read(CP_CTRL_S);
	cp_write(CP_CTRL_S, s | CP_START);
	say("CP powered: CP_CTRL_NS 0x%x -> 0x%x, CP_CTRL_S 0x%x -> 0x%x", ns, cp_read(CP_CTRL_NS), s, cp_read(CP_CTRL_S));
	watch(20);
	say("stage 2 step 1 done: the CP is left as it is (the next reboot resets it)");
	return 0;
}
