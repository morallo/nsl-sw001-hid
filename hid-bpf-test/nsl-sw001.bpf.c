// SPDX-License-Identifier: GPL-2.0-only
/*
 * HID-BPF fix to make the Nintendo N-SL SW001 bluetooth controller work
 * with the in-tree hid-nintendo driver (no more hid-nintendo blacklist,
 * no custom kernel module).
 *
 * Problem: hid-nintendo parses reports 0x30/0x21 by byte offset in its
 * .raw_event hook, but hid-core only reaches .raw_event for report IDs that
 * are declared in the device report descriptor (see __hid_input_report() in
 * hid-core.c). The SW001's stock descriptor declares neither, so:
 *   - subcmd replies (0x21) are dropped before raw_event -> the probe-time
 *     SPI reads time out with -110 and hid-nintendo never binds
 *   - the autonomous 0x30 input stream is dropped -> no input, no IMU
 *
 * Fix: append report ID declarations to the stock descriptor with
 * hid_rdesc_fixup.
 *
 * Gating: a genuine Pro Controller also matches 0005:057E:2009, so the
 * rdesc fixup only appends when the descriptor does NOT already declare
 * report 0x21 (genuine controllers do; the SW001 doesn't).
 */
#include "vmlinux.h"
#include "hid_bpf.h"
#include "hid_bpf_helpers.h"
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

/* Set by sw001_fix_rdesc only when it actually appends the 0x21/0x30 report
 * declarations (i.e. the device is a SW001 clone, not a genuine Pro
 * Controller). Used by sw001_fake_spi to avoid faking cal data for genuine
 * controllers, which serve their own real calibration. */
static bool sw001_is_clone;

HID_BPF_CONFIG(
	HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_ANY, 0x057e, 0x2009),
);

/*
 * HID items appended to the stock descriptor:
 *   report 0x30 = 48 Const bytes, report 0x21 = 48 Const bytes.
 * Field contents are irrelevant to hid-nintendo (it parses raw bytes); the
 * reports only need to exist so hid-core routes them to .raw_event.
 */
static const __u8 sw001_rdesc_extra[] = {
	0x85, 0x30,				/*   Report ID 0x30            */
	0x15, 0x00,				/*   Logical Minimum   0      */
	0x25, 0xff,				/*   Logical Maximum 255      */
	0x75, 0x08,				/*   Report Size   8          */
	0x95, 0x30,				/*   Report Count 48 = 48 B   */
	0x81, 0x03,				/*   Input (Const, Var, Abs)  */
	0x85, 0x21,				/*   Report ID 0x21           */
	0x15, 0x00,
	0x25, 0xff,
	0x75, 0x08,
	0x95, 0x30,
	0x81, 0x03,
};

static __always_inline bool rdesc_has_report_id(const __u8 *data, __u32 size, __u8 id)
{
	__u32 i;

	/* Bound the search window: genuine Pro Controller descriptors are
	 * ~200 bytes, so a compile-time cap is plenty and keeps the BPF
	 * verifier from unrolling a loop over the full runtime max. */
	if (size > 1024)
		size = 1024;

	for (i = 0; i + 1 < size; i++) {
		if (data[i] == 0x85 && data[i + 1] == id)
			return true;
	}

	return false;
}

SEC(HID_BPF_RDESC_FIXUP)
int BPF_PROG(sw001_fix_rdesc, struct hid_bpf_ctx *hctx)
{
	__u8 *data;
	__u32 off;
	__u32 size;

	data = hid_bpf_get_data(hctx, 0, HID_MAX_DESCRIPTOR_SIZE);
	if (!data)
		return 0;

	size = hctx->size;
	if (size > HID_MAX_DESCRIPTOR_SIZE)
		return 0;

	/* Genuine Pro Controllers already declare 0x21: leave them alone. */
	if (rdesc_has_report_id(data, size, 0x21))
		return 0;

	off = size;
	if (off + sizeof(sw001_rdesc_extra) > HID_MAX_DESCRIPTOR_SIZE)
		return 0;

	__builtin_memcpy(data + off, sw001_rdesc_extra, sizeof(sw001_rdesc_extra));

	sw001_is_clone = true;

	return off + sizeof(sw001_rdesc_extra);
}

/*
 * NOTE on Y-axis direction: the SW001's raw 12-bit Y convention already
 * matches genuine (pushing up -> raw value toward 0xFFF, down -> toward 0).
 * Both consumers invert Y themselves and expect exactly that convention:
 *   - SDL HIDAPI Switch sends ~axis after stick calibration
 *   - hid-nintendo does y = -joycon_map_stick_val(...)
 * so NO flip is done here. A device_event flip was needed only for the old
 * hid-generic path, which mapped raw 0..4095 linearly with no negation.
 *
 * The SW001 ignores SPI reads of the stick calibration areas (0x603D/0x603F
 * factory, 0x8010/0x8012/0x801B/0x801D user), which is why:
 *   - hid-nintendo's probe SPI reads time out with -110 and fall back to
 *     default stick calibration
 *   - SDL's HIDAPI Switch driver fails its init ("Couldn't load stick
 *     calibration") unless SDL_HIDAPI_IGNORE_DEVICES forces the evdev backend
 *
 * Both consumers send the SAME output report for a SPI read: report 0x01,
 * subcmd 0x10 at byte 10, address (LE32) at bytes 11-14, length at byte 15.
 * Answer those reads on the wire: forward the output report untouched AND
 * inject a synthetic 0x21 subcmd reply (ack 0x90, subcmd 0x10) echoing the
 * requested address/length with canned, plausible calibration data.
 *
 * Covered areas:
 *   - stick user/factory cal (all stick axes at 2048 center / 3072 max /
 *     1024 min, which also passes hid-nintendo's min<center<max sanity check)
 *   - IMU factory cal 0x6020 (real SW001 values captured from the Android
 *     17 session) and IMU user magic 0x8026 (answered with NO B2 A1 magic).
 *     Without the IMU fakes the SW001 serves the B2 A1 user-cal magic on the
 *     wire followed by garbage user data, so hid-nintendo logs "using user
 *     cal for IMU" with bogus divisors and SDL HIDAPI overrides the factory
 *     accelerometer/gyro scale with the same garbage.
 *
 * hid_bpf_try_input_report (non-sleepable) is used instead of
 * hid_bpf_input_report (KF_SLEEPABLE) because struct_ops programs loaded by
 * udev-hid-bpf are not marked sleepable.
 */
#define NSL_SPI_STICK_USER_MAGIC	0x8010
#define NSL_SPI_STICK_USER_LEFT		0x8012
#define NSL_SPI_STICK_USER_RIGHT_MAGIC	0x801b
#define NSL_SPI_STICK_USER_RIGHT	0x801d
#define NSL_SPI_STICK_FACTORY_LEFT	0x603d
#define NSL_SPI_STICK_FACTORY_RIGHT	0x6046
#define NSL_SPI_IMU_FACTORY		0x6020
#define NSL_SPI_IMU_USER_MAGIC		0x8026
#define NSL_SPI_IMU_USER_DATA		0x8028

/* 12-bit values packed as pairs sharing a nibble byte; see SDL's
 * SwitchLoadCalibration / hid-nintendo's joycon_read_stick_calibration.
 * Left order: max, center, min. Right order: center, min, max. */
static const __u8 sw001_cal_magic[2] = { 0xb2, 0xa1 };
static const __u8 sw001_cal_left[9] = {
	0x00, 0x0c, 0xc0,	/* X/Y max   0xC00 */
	0x00, 0x08, 0x80,	/* X/Y center 0x800 */
	0x00, 0x04, 0x40,	/* X/Y min   0x400 */
};
static const __u8 sw001_cal_right[9] = {
	0x00, 0x08, 0x80,	/* X/Y center 0x800 */
	0x00, 0x04, 0x40,	/* X/Y min   0x400 */
	0x00, 0x0c, 0xc0,	/* X/Y max   0xC00 */
};
static const __u8 sw001_user_cal_blob[22] = {
	0xb2, 0xa1,
	0x00, 0x0c, 0xc0, 0x00, 0x08, 0x80, 0x00, 0x04, 0x40,
	0xb2, 0xa1,
	0x00, 0x08, 0x80, 0x00, 0x04, 0x40, 0x00, 0x0c, 0xc0,
};
static const __u8 sw001_factory_cal_blob[18] = {
	0x00, 0x0c, 0xc0, 0x00, 0x08, 0x80, 0x00, 0x04, 0x40,
	0x00, 0x08, 0x80, 0x00, 0x04, 0x40, 0x00, 0x0c, 0xc0,
};

/* SW001 factory IMU calibration (SPI 0x6020), captured from the Android 17
 * init session (bluetooth_captures/sw001_calibration_0x6020.md). Layout is
 * the per-axis hid-nintendo/SDL factory format, each s16 LE:
 *   accel_offset[3], accel_scale[3], gyro_offset[3], gyro_scale[3]
 * The SW001 serves real (but often bogus user-cal) values at 0x8026/0x8028
 * on the wire, so both hid-nintendo and SDL HIDAPI must be steered to the
 * factory path: 0x8026 is answered with NO user-cal magic, and 0x6020 with
 * these values. Servable on both consumers' SPI subcmd reads. */
static const __u8 sw001_imu_factory_cal[24] = {
	0xa2, 0x00, 0x3b, 0xff, 0x64, 0x01,	/* accel offset X/Y/Z */
	0x00, 0x40, 0x00, 0x40, 0x00, 0x40,	/* accel scale X/Y/Z */
	0xee, 0xff, 0x0c, 0x00, 0x11, 0x00,	/* gyro offset X/Y/Z */
	0xe7, 0x3b, 0xe7, 0x3b, 0xe7, 0x3b,	/* gyro scale X/Y/Z */
};

SEC(HID_BPF_HW_OUTPUT_REPORT)
int BPF_PROG(sw001_fake_spi, struct hid_bpf_ctx *hctx)
{
	__u8 reply[50] = { 0 };
	__u8 *data;
	__u32 addr;
	__u8 len;

	/* hid_bpf_get_data needs a compile-time size; the output-report ctx is
	 * sized to the report (16B for a kernel subcmd request, 49B for SDL's
	 * BT reports). 16 covers both; smaller reports just return NULL here.
	 */
	if (hctx->size < 16)
		return 0;

	data = hid_bpf_get_data(hctx, 0, 16);
	if (!data)
		return 0;

	if (!sw001_is_clone)
		return 0;

	if (data[0] != 0x01)
		return 0;

	/* subcmd output report: byte 10 = subcmd id, 0x10 = SPI flash read */
	if (data[10] != 0x10)
		return 0;

	addr = data[11] | (data[12] << 8) | (data[13] << 16) | (data[14] << 24);
	len = data[15];

	reply[0] = 0x21;
	reply[13] = 0x90;	/* ACK + data following */
	reply[14] = 0x10;
	reply[15] = addr & 0xff;
	reply[16] = (addr >> 8) & 0xff;
	reply[17] = (addr >> 16) & 0xff;
	reply[18] = (addr >> 24) & 0xff;
	reply[19] = len;

	if (addr == NSL_SPI_STICK_USER_MAGIC) {
		__builtin_memcpy(reply + 20, sw001_user_cal_blob, sizeof(sw001_user_cal_blob));
	} else if (addr == NSL_SPI_STICK_USER_LEFT) {
		__builtin_memcpy(reply + 20, sw001_cal_left, sizeof(sw001_cal_left));
	} else if (addr == NSL_SPI_STICK_USER_RIGHT_MAGIC) {
		__builtin_memcpy(reply + 20, sw001_cal_magic, sizeof(sw001_cal_magic));
	} else if (addr == NSL_SPI_STICK_USER_RIGHT) {
		__builtin_memcpy(reply + 20, sw001_cal_right, sizeof(sw001_cal_right));
	} else if (addr == NSL_SPI_STICK_FACTORY_LEFT) {
		__builtin_memcpy(reply + 20, sw001_factory_cal_blob, sizeof(sw001_factory_cal_blob));
	} else if (addr == NSL_SPI_STICK_FACTORY_RIGHT) {
		__builtin_memcpy(reply + 20, sw001_cal_right, sizeof(sw001_cal_right));
	} else if (addr == NSL_SPI_IMU_USER_MAGIC) {
		/* reply was zero-inited: no B2 A1 magic -> consumers take the
		 * factory-cal path instead of the SW001's bogus user cal */
	} else if (addr == NSL_SPI_IMU_FACTORY || addr == NSL_SPI_IMU_USER_DATA) {
		__builtin_memcpy(reply + 20, sw001_imu_factory_cal, sizeof(sw001_imu_factory_cal));
	} else {
		return 0;
	}

	/* bytes after the requested length stay zero (reply was zero-inited) */
	hid_bpf_try_input_report(hctx, HID_INPUT_REPORT, reply, sizeof(reply));

	return 0;
}

/* udev-hid-bpf runs this to decide whether the device matches. */
SEC("syscall")
int probe(struct hid_bpf_probe_args *ctx)
{
	ctx->retval = 0;

	return 0;
}

HID_BPF_OPS(sw001) = {
	.hid_rdesc_fixup = (void *)sw001_fix_rdesc,
	.hid_hw_output_report = (void *)sw001_fake_spi,
};