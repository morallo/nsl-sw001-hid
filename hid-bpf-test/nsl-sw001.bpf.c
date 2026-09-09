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

/*
 * SW001 rumble-stop cascade.
 *
 * The SW001 latches the rumble setpoint of the last 0x10 report it received;
 * a single zero-amplitude 0x10 does not silence it (SDL HIDAPI sends exactly
 * one stop report and then goes quiet, so its rumble "sticks" for a while).
 *
 * Delivery must be TIME-SPACED: a µs-burst of zero reports all lands inside
 * one of the controller's rumble-sampling windows and can be missed entirely
 * ("stuck in vibration"). hid-nintendo's FF needs ~5 zero reports spaced
 * 50 ms apart before the motors stop.
 *
 * There is no way to delay a report from the kernel here: this kernel has no
 * sleepable bpf_timer (bpf_timer_set_sleepable_cb is absent), and bpf_wq_start
 * takes no delay. Instead the controller's OWN 0x30 input stream (continuous,
 * ~every 15 ms) is used as a pacemaker: a struct_ops hid_device_event hook
 * fires on every input report while a stop is pending and schedules the wq to
 * send one zero report. No output happens from the input-path context (a BT
 * HIDP output can block on the L2CAP socket lock, so the kernel-side input
 * thread must never send); the hook only calls bpf_wq_start, which is safe
 * from any context. The wq callback does the actual (sleepable) output.
 *
 * The state map is only written when the SDL padded 0x10 form is seen, so
 * genuine Pro Controllers (hid-nintendo 10-byte 0x10, own FF countdown) are
 * untouched.
 *
 * A bpf_wq is used instead of a bpf_timer to DO the output because the
 * workqueue callback runs in a sleepable context -- hid_bpf_hw_output_report
 * is KF_SLEEPABLE. udev-hid-bpf pins every map to bpffs when loading, which
 * keeps map->usercnt > 0 (bpf_wq_init returns -EPERM otherwise), so the wq
 * survives the one-shot loader exiting.
 */
#ifndef NSL_RUMBLE_ZERO_TOTAL
/* Zero packets on a stop, incl. the first. The reliable dimension is the
 * SPAN of distinct sampling instances, not the packet count: 1 per tick,
 * TOTAL=20 (~200 ms) stopped every time. ZPT=2 with the same TOTAL shrank
 * the span to ~10 instances -> 95%. To go fast AND keep the span, raise
 * TOTAL instead, e.g. ZPT=2, TOTAL=36 = 17 pair-phases (~200 ms, denser
 * early coverage). */
#define NSL_RUMBLE_ZERO_TOTAL	36
#endif
/* Compile-time knob: pass BPF_CFLAGS=-DNSL_RUMBLE_STOP_CASCADE=0 to disable
 * the paced stop cascade entirely, leaving only SDL's single neutral write
 * (i.e. the pre-fix "stuck" behavior). The wq init and the short report
 * translation stay in the build either way. */
#ifndef NSL_RUMBLE_STOP_CASCADE
#define NSL_RUMBLE_STOP_CASCADE 1
#endif
/* Zero reports per 0x30 tick while a stop is pending, 1..NSL_RUMBLE_ZERO_TOTAL.
 * 1 per tick with TOTAL=20 was the verified-reliable baseline (~200 ms).
 * 2 halves the number of distinct tick instances for a given TOTAL, so only
 * combine it with a raised NSL_RUMBLE_ZERO_TOTAL (see above). */
#ifndef NSL_ZEROS_PER_TICK
#define NSL_ZEROS_PER_TICK 2
#endif

struct rumble_state {
	struct bpf_wq wq;		/* stays live for session lifetime */
	__u32 hid_id;			/* for hid_bpf_allocate_context() */
	__u32 stop_countdown;		/* zero packets still to send */
	__u8 cb_ready;
	__u8 inited;
	__u8 rumble_ctr;		/* monotonic packet number for zero reports */
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct rumble_state);
} sw001_rumble SEC(".maps");

/* Sends ONE zero-amplitude 0x10 report. Pure sender: pacing is done by
 * sw001_tick_stop (one call per input report) and the single immediate
 * start from sw001_fake_spi on the stop. */
static int nsl_rumble_zero_cb(void *map, int *key, void *value)
{
	struct rumble_state *st = (struct rumble_state *)value;
	struct hid_bpf_ctx *hctx;
	__u8 zero[10];

	if (!st)
		return 0;

	/* A payload of eight raw zero bytes is NOT a valid "off" for the
	 * SW001: it decodes the dekuNukem high/low freq + amplitude fields
	 * and treats an all-zero rumble field as undefined, keeping the last
	 * setpoint (verified: a 5-packet all-zero burst did not stop the
	 * motors). Send the real zero-amplitude encoding instead, which is
	 * byte-identical to SDL's set-neutral and hid-nintendo's amp=0
	 * encode: {0x00, 0x0040} per motor (freq bytes with amp bits 0). */
	zero[0] = 0x10;
	zero[1] = st->rumble_ctr++;
	zero[2] = 0x00;
	zero[3] = 0x01;
	zero[4] = 0x40;
	zero[5] = 0x40;
	zero[6] = 0x00;
	zero[7] = 0x01;
	zero[8] = 0x40;
	zero[9] = 0x40;

	hctx = hid_bpf_allocate_context(st->hid_id);
	if (!hctx)
		return 0;

	hid_bpf_hw_output_report(hctx, zero, sizeof(zero));

	hid_bpf_release_context(hctx);

	return 0;
}

HID_BPF_CONFIG(
	HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_ANY, 0x057e, 0x2009),
);

/*
 * HID items appended to the stock descriptor:
 *   report 0x30 = 48 Const bytes (input), report 0x21 = 48 Const bytes
 *   (input): field contents are irrelevant to hid-nintendo (it parses raw
 *   bytes); the reports only need to exist so hid-core routes them to
 *   .raw_event.
 *   report 0x10 = 9 Const bytes (output): the rumble-only report. Needed so
 *   the sleepable hid_bpf_hw_output_report kfunc can short-resend SDL's
 *   padded 0x10 (hid_report_len = 9 + 1 id byte = 10, the SW001's wire
 *   form). hid-nintendo ignores the descriptor and sends its own 10-byte
 *   buffer anyway.
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
	0x85, 0x10,				/*   Report ID 0x10           */
	0x15, 0x00,
	0x25, 0xff,
	0x75, 0x08,
	0x95, 0x09,				/*   Report Count 9 = 9 B     */
	0x91, 0x03,				/*   Output (Const, Var, Abs) */
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

	/* Init the wq here rather than lazily: rdesc_fixup runs while
	 * udev-hid-bpf still holds the map fds (map->usercnt > 0 is required
	 * by bpf_wq_init), and this is the only clone-gated one-shot point. */
	{
		__u32 key = 0;
		struct rumble_state *st = bpf_map_lookup_elem(&sw001_rumble, &key);

		if (st && !st->inited && bpf_wq_init(&st->wq, &sw001_rumble, 0) == 0)
			st->inited = 1;
	}

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
 *   - stick user/factory cal (deltas matching the measured raw ranges:
 *     center 2048, X max-above 2036 / min-below 2048, Y max-above 2047 /
 *     min-below 2037, which also passes hid-nintendo's min<center<max check)
 *   - IMU factory cal 0x6020 (real SW001 values captured from the Android
 *     17 session) and IMU user magic 0x8026 (answered with NO B2 A1 magic).
 *     Without the IMU fakes the SW001 serves the B2 A1 user-cal magic on the
 *     wire followed by garbage user data, so hid-nintendo logs "using user
 *     cal for IMU" with bogus divisors and SDL HIDAPI overrides the factory
 *     accelerometer/gyro scale with the same garbage.
 *
 * hid_bpf_try_input_report (non-sleepable, IRQ-safe) is used instead of
 * hid_bpf_input_report (KF_SLEEPABLE) because it needs no sleepable program.
 * The rumble fix below needs hid_bpf_hw_output_report (KF_SLEEPABLE) once,
 * so this hook is the exception and is declared sleepable
 * (SEC("struct_ops.s/...") — allowed by the kernel's hid_bpf_ops_check_member
 * for the hid_hw_output_report member).
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

/* IMPORTANT: the max/min fields are DELTAS above/below center, not
 * absolute positions. hid-nintendo's joycon_read_stick_calibration computes
 * cal->max = center + max_above and cal->min = center - min_below (SDL
 * HIDAPI reads them the same way). The earlier blobs shipped absolute
 * values (max 0xC00, min 0x400) -> consumers computed max = 2048+3072 =
 * 5120, so the positive branch of joycon_map_stick_val divided by 3072 while
 * the negative branch divided by 1024. Result (matches field reports): up and
 * right topped out at ~66% (2047*32767/3072 = 21820) while down and left
 * clamped to 100%.
 *
 * The deltas below are the raw values MEASURED on this SW001 unit
 * (diagnostic_scripts/oneshot_raw.py): X sweeps 0..4084, Y sweeps 11..4095,
 * rest centered at 2048. Both sticks measured identically, so one left/right
 * pair serves both.
 *
 * 12-bit values packed as pairs sharing a nibble byte; see SDL's
 * SwitchLoadCalibration / hid-nintendo's joycon_read_stick_calibration.
 * Left order: max-above, center, min-below. Right order: center, min-below,
 * max-above. */
static const __u8 sw001_cal_magic[2] = { 0xb2, 0xa1 };
static const __u8 sw001_cal_left[9] = {
	0xf4, 0xf7, 0x7f,	/* X max above 2036, Y max above 2047 */
	0x00, 0x08, 0x80,	/* X/Y center 0x800 */
	0x00, 0x58, 0x7f,	/* X min below 2048, Y min below 2037 */
};
static const __u8 sw001_cal_right[9] = {
	0x00, 0x08, 0x80,	/* X/Y center 0x800 */
	0x00, 0x58, 0x7f,	/* X min below 2048, Y min below 2037 */
	0xf4, 0xf7, 0x7f,	/* X max above 2036, Y max above 2047 */
};
static const __u8 sw001_user_cal_blob[22] = {
	0xb2, 0xa1,
	0xf4, 0xf7, 0x7f, 0x00, 0x08, 0x80, 0x00, 0x58, 0x7f,
	0xb2, 0xa1,
	0x00, 0x08, 0x80, 0x00, 0x58, 0x7f, 0xf4, 0xf7, 0x7f,
};
static const __u8 sw001_factory_cal_blob[18] = {
	0xf4, 0xf7, 0x7f, 0x00, 0x08, 0x80, 0x00, 0x58, 0x7f,
	0x00, 0x08, 0x80, 0x00, 0x58, 0x7f, 0xf4, 0xf7, 0x7f,
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

SEC("struct_ops.s/hid_hw_output_report")
int BPF_PROG(sw001_fake_spi, struct hid_bpf_ctx *hctx)
{
	__u8 reply[50] = { 0 };
	__u8 short_rumble[10];
	__u8 *data;
	__u32 addr;
	__u8 len;

	/* Short output reports (< 16 B) are hid-nintendo's natural 16-byte
	 * subcmd requests and its 10-byte rumble 0x10 (which also covers the
	 * sleepable re-send below): pass them through untouched. */
	if (hctx->size < 16)
		return 0;

	data = hid_bpf_get_data(hctx, 0, 16);
	if (!data)
		return 0;

	if (!sw001_is_clone)
		return 0;

	/* SDL's HIDAPI Switch driver (BT) sends the rumble-only report 0x10
	 * zero-padded to k_unSwitchBluetoothPacketLength (49 B). The SW001 only
	 * reacts to short reports and ignores that form (rumble dead via SDL,
	 * works via hid-nintendo's 10-byte 0x10). Re-send the short form
	 * ({0x10, packet_num, rumble_data[8]}, same layout hid-nintendo's FF
	 * emits) and swallow the padded original by returning hctx->size, so
	 * hidraw_write reports a full success to SDL. The 10-byte re-send
	 * re-enters this hook (size < 16) and is passed through to the device. */
	if (data[0] == 0x10) {
		struct rumble_state *st = NULL;
		__u32 key0 = 0;

		short_rumble[0] = data[0];
		short_rumble[1] = data[1];
		__builtin_memcpy(short_rumble + 2, data + 2, 8);

		st = bpf_map_lookup_elem(&sw001_rumble, &key0);
		if (st && st->inited && !st->cb_ready) {
			if (bpf_wq_set_callback(&st->wq, nsl_rumble_zero_cb, 0) == 0)
				st->cb_ready = 1;
		}

		if (hid_bpf_hw_output_report(hctx, short_rumble,
					     sizeof(short_rumble)) != (int)sizeof(short_rumble))
			/* kfunc failed: forward the original unchanged (SDL's padded
			 * form, i.e. today's behavior) rather than dropping it. */
			return 0;

		#if NSL_RUMBLE_STOP_CASCADE
		if (st && st->inited && st->cb_ready) {
			bool zero_amp;
			__u8 i;

			/* Amplitude lives in the low 3 bits of bytes 0 and 2 of
			 * each motor's 4-byte group (dekuNukem encoding, same as
			 * hid-nintendo). SDL's set-neutral write is
			 * {0x00,0x01,0x40,0x40} per motor -- not all-zero bytes --
			 * so test the amplitude bits, not the raw bytes. */
			zero_amp = true;
			for (i = 0; i < 8; i += 2)
				zero_amp &= (short_rumble[2 + i] & 0x07) == 0;

			if (zero_amp) {
				/* The first two zero reports (SDL's neutral
				 * resend + one immediate wq start) go out now;
				 * sw001_tick_stop pumps the rest, paced by the
				 * controller's own 0x30 stream so the SW001
				 * holds a zero setpoint across its sampling
				 * windows instead of a single missed burst. */
				st->stop_countdown = NSL_RUMBLE_ZERO_TOTAL - 2;
				bpf_wq_start(&st->wq, 0);
			} else {
				/* New amplitude: kill a pending cascade. */
				st->stop_countdown = 0;
			}
		}
#endif

		return hctx->size;
	}

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
	__u32 key = 0;
	struct rumble_state *st;

	ctx->retval = 0;

	/* hid_bpf_allocate_context() keys on the numeric hid id; the wq
	 * callback has no hctx, so grab it here (runs before any hook). */
	st = bpf_map_lookup_elem(&sw001_rumble, &key);
	if (st)
		st->hid_id = ctx->hid;

	return 0;
}

/*
 * Rumble-stop pacemaker. Fires on every input report while a stop cascade is
 * pending and schedules one zero-amplitude 0x10 per incoming 0x30 report, so
 * the controller sees a zero setpoint at (approximately) its own sampling
 * cadence for a few hundred ms instead of one µs-burst. Only schedules the
 * wq -- never sends output from the input-path context (BT HIDP socket lock).
 */
SEC("struct_ops/hid_device_event")
int BPF_PROG(sw001_tick_stop, struct hid_bpf_ctx *hctx,
	     enum hid_report_type report_type, u64 timestamp_ns)
{
	struct rumble_state *st;
	__u32 key0 = 0;

	(void)hctx;
	(void)timestamp_ns;

	if (!sw001_is_clone || report_type != HID_INPUT_REPORT)
		return 0;

	st = bpf_map_lookup_elem(&sw001_rumble, &key0);
	if (!st || !st->inited || !st->cb_ready || st->stop_countdown == 0)
		return 0;

	{
		__u8 i;
		__u8 n = st->stop_countdown < NSL_ZEROS_PER_TICK ?
			    st->stop_countdown : (__u8)NSL_ZEROS_PER_TICK;

		st->stop_countdown -= n;
		for (i = 0; i < n; i++)
			bpf_wq_start(&st->wq, 0);
	}

	return 0;
}

HID_BPF_OPS(sw001) = {
	.hid_rdesc_fixup = (void *)sw001_fix_rdesc,
	.hid_hw_output_report = (void *)sw001_fake_spi,
	.hid_device_event = (void *)sw001_tick_stop,
};