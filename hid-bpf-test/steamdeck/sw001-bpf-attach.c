// SPDX-License-Identifier: GPL-2.0
/*
 * sw001-bpf-attach: one-shot HID-BPF loader/attacher for the Nintendo
 * N-SL SW001, for the Steam Deck packaging.
 *
 * Replaces udev-hid-bpf on the Deck with self-contained files under /home
 * (no pacman, no /usr writes, survives A/B updates).  It loads a prebuilt
 * CO-RE object and:
 *   1. patches the struct_ops map value's hid_id (first field of
 *      struct hid_bpf_ops, offset 0) with the numeric HID id parsed from
 *      the device sysfs name (the trailing %04X of "0005:057E:2009.000A"),
 *   2. runs the object's SEC("syscall") probe program via BPF_PROG_TEST_RUN
 *      (the probe stashes hid_id for the rumble-stop workqueue callback),
 *   3. calls bpf_map__attach_struct_ops() and pins the link under
 *      /sys/fs/bpf/hid/ so it stays attached after this process exits.
 *
 * Build (dev machine only, Deck gets the prebuilt binary):
 *   gcc -O2 sw001-bpf-attach.c $(pkg-config --cflags libbpf) \
 *       -Wl,-rpath,'$ORIGIN/lib' -o sw001-bpf-attach -lbpf -lelf -lz -lzstd
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#define BPFFS_ROOT "/sys/fs/bpf"
#define PIN_ROOT   "/sys/fs/bpf/hid"

/* Must match vmlinux.h struct hid_bpf_probe_args (u32 hid; u32 rdesc_size;
 * u8 rdesc[4096]; s32 retval;) -- retval lives at offset 4104. */
struct hid_bpf_probe_args {
	unsigned int hid;
	unsigned int rdesc_size;
	unsigned char rdesc[4096];
	int retval;
};

static void usage(const char *name)
{
	fprintf(stderr, "usage: %s DEVPATH OBJ\n", name);
	fprintf(stderr, "  DEVPATH: udev $sys$devpath (the HID device path)\n");
	fprintf(stderr, "  OBJ:      path to the prebuilt .bpf.o object\n");
}

/* "0005:057E:2009.000A" -> 0x000A, the kernel per-device hid id
 * (dev_set_name() in drivers/hid/hid-core.c).  The last '.' in the full
 * device path always separates the HID sysname's instance field. */
static unsigned int hid_id_from_devpath(const char *devpath)
{
	const char *dot = strrchr(devpath, '.');
	char *end = NULL;
	unsigned long v;

	if (!dot)
		return 0;
	v = strtoul(dot + 1, &end, 16);
	if (end == dot + 1 || *end != '\0' || v == 0 || v > 0xffff)
		return 0;
	return (unsigned int)v;
}

static int read_rdesc(const char *devpath, unsigned char *rdesc, int cap)
{
	char path[512];
	int fd, n;

	snprintf(path, sizeof path, "%s/report_descriptor", devpath);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, rdesc, cap);
	close(fd);
	if (n < 0)
		return 0;
	return n;
}

static void mkdirp(const char *path)
{
	char tmp[512];
	char *p;

	snprintf(tmp, sizeof tmp, "%s", path);
	p = tmp;
	while (*p) {
		while (*p == '/')
			p++;
		char *s = p;
		while (*p && *p != '/')
			p++;
		if (s < p) {
			char c = *p;
			*p = '\0';
			mkdir(tmp, 0755);
			*p = c;
		}
	}
}

int main(int argc, char **argv)
{
	struct bpf_object *obj = NULL;
	struct bpf_map *m = NULL;
	struct bpf_program *probe;
	struct bpf_link *link;
	struct hid_bpf_probe_args args = { 0 };
	struct bpf_test_run_opts ro;
	const char *devpath, *objfile, *base;
	size_t vsz = 0;
	void *v;
	char pinpath[512];
	size_t pinlen, lastslash;
	int err;

	if (argc != 3) {
		usage(argv[0]);
		return 2;
	}
	devpath = argv[1];
	objfile = argv[2];

	/* Kernels <= 5.11 still enforce RLIMIT_MEMLOCK for BPF; lift it like
	 * bpftool does (harmless on newer kernels). */
	struct rlimit r = { RLIM_INFINITY, RLIM_INFINITY };
	setrlimit(RLIMIT_MEMLOCK, &r);

	err = access(devpath, F_OK);
	if (err) {
		fprintf(stderr, "sw001-bpf-attach: %s: %s\n", devpath, strerror(errno));
		return 2;
	}

	unsigned int hid_id = hid_id_from_devpath(devpath);
	if (!hid_id) {
		fprintf(stderr, "sw001-bpf-attach: cannot parse hid id from %s\n", devpath);
		return 2;
	}

	obj = bpf_object__open_file(objfile, NULL);
	if (!obj) {
		fprintf(stderr, "sw001-bpf-attach: cannot open %s\n", objfile);
		return 1;
	}

	while ((m = bpf_object__next_map(obj, m))) {
		if (bpf_map__type(m) == BPF_MAP_TYPE_STRUCT_OPS)
			break;
	}
	if (!m) {
		fprintf(stderr, "sw001-bpf-attach: no struct_ops map in %s\n", objfile);
		return 1;
	}

	v = bpf_map__initial_value(m, &vsz);
	if (!v || vsz < sizeof hid_id) {
		fprintf(stderr, "sw001-bpf-attach: struct_ops map has no initial value\n");
		return 1;
	}
	memcpy(v, &hid_id, sizeof hid_id);

	err = bpf_object__load(obj);
	if (err) {
		fprintf(stderr, "sw001-bpf-attach: load failed: %s\n", strerror(-err));
		return 1;
	}

	/* Run SEC("syscall") probe so the program stores hid_id into its rumble
	 * map (the wq callback needs it).  Best effort: this sets nothing if the
	 * object has no probe section. */
	probe = bpf_object__find_program_by_name(obj, "probe");
	if (probe) {
		args.hid = hid_id;
		args.rdesc_size = (unsigned int)read_rdesc(devpath, args.rdesc,
						       sizeof args.rdesc);
		memset(&ro, 0, sizeof ro);
		ro.sz = sizeof ro;
		ro.ctx_in = &args;
		ro.ctx_size_in = sizeof args;
		err = bpf_prog_test_run_opts(bpf_program__fd(probe), &ro);
		if (err)
			fprintf(stderr, "sw001-bpf-attach: probe run failed: %s\n",
				strerror(-err));
		else if (ro.retval != 0)
			fprintf(stderr, "sw001-bpf-attach: probe returned %d\n",
				ro.retval);
	}

	link = bpf_map__attach_struct_ops(m);
	if (!link) {
		fprintf(stderr, "sw001-bpf-attach: attach failed: %s\n",
			strerror(libbpf_get_error(link)));
		return 1;
	}

	/* Pin the link or it dies with this process.  HID-BPF uses
	 * /sys/fs/bpf/hid/<sysname>/<obj>, mirroring udev-hid-bpf.
	 * sysname: basename of the device path with ':' and '.' -> '_'. */
	snprintf(pinpath, sizeof pinpath, "%s/", PIN_ROOT);
	pinlen = strlen(pinpath);
	base = strrchr(devpath, '/');
	base = base ? base + 1 : devpath;
	for (const char *c = base; *c; c++)
		pinpath[pinlen++] = (*c == ':' || *c == '.') ? '_' : *c;
	pinpath[pinlen++] = '/';
	/* obj basename minus the .o extension, dots -> '_' */
	base = strrchr(objfile, '/');
	base = base ? base + 1 : objfile;
	for (const char *c = base; *c && strcmp(c, ".o") != 0; c++)
		pinpath[pinlen++] = *c == '.' ? '_' : *c;
	pinpath[pinlen] = '\0';

	if (access(BPFFS_ROOT, F_OK)) {
		if (mount("bpf", BPFFS_ROOT, "bpf", 0, NULL))
			fprintf(stderr, "sw001-bpf-attach: mount %s failed: %s\n",
				BPFFS_ROOT, strerror(errno));
	}
	/* mkdir the parent of the pin file only; the leaf is created by
	 * bpf_link__pin(). */
	lastslash = 0;
	for (size_t i = 0; pinpath[i]; i++)
		if (pinpath[i] == '/')
			lastslash = i;
	if (lastslash) {
		char dir[512];
		size_t n = lastslash < sizeof dir - 1 ? lastslash : sizeof dir - 1;
		memcpy(dir, pinpath, n);
		dir[n] = '\0';
		mkdirp(dir);
	}
	/* Stale pins from an earlier connection of the same named device can
	 * linger in bpffs; unlink first so a reconnect pins cleanly. */
	unlink(pinpath);
	err = bpf_link__pin(link, pinpath);
	bpf_link__destroy(link);
	if (err) {
		fprintf(stderr, "sw001-bpf-attach: pin %s failed: %s\n",
			pinpath, strerror(-err));
		return 1;
	}

	bpf_object__close(obj);
	return 0;
}