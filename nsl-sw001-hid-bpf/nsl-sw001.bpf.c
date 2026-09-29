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
 *   - subcmd 0x02 request device info (the SW001 never answers it; SDL/Steam
 *     HIDAPI and hid-nintendo classify the controller from this reply, and
 *     Steam Input gates the gyro feature on getting a Pro Controller type)
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

/*
 * Rumble stop handling.
 *
 * The SW001 accepts the Nintendo neutral rumble packet, but a neutral 0x10
 * sent too soon after the preceding output (< ~30 ms on this unit) can be
 * ignored. hid-nintendo avoids this by keeping a five-packet zero-amplitude
 * stop sequence alive on a 50 ms cadence.
 *
 * We reproduce that behavior here for the SDL HIDAPI path:
 *
 *   host stop 0x10 -> send neutral immediately (packet #1)
 *                  -> send four more neutral reports, 50 ms apart
 *
 * Thus every stop request produces five neutral reports in total. A later
 * non-zero rumble cancels the outstanding cascade. A later stop request
 * starts a fresh five-packet sequence and restarts the 50 ms spacing.
 *
 * bpf_wq_start() itself has no delay argument, so it cannot provide the
 * 50 ms pacing. We use a bpf_timer as the clock and a sleepable bpf_wq
 * callback for the actual hid_bpf_hw_output_report() call. The timer is
 * re-armed only after a synthetic neutral report has been sent, so the
 * spacing is measured from the previous output rather than from the timer
 * callback's scheduling point.
 *
 * Both bpf_timer and bpf_wq are available in the SteamOS 3.8 / Linux 7.2
 * and Fedora / Linux 7.4 kernels targeted by this program. bpf_wq callbacks
 * are sleepable, which is required by hid_bpf_hw_output_report().
 */

#ifndef NSL_RUMBLE_STOP_PKT_CNT
#define NSL_RUMBLE_STOP_PKT_CNT	5
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifndef NSL_RUMBLE_PERIOD_NS
#define NSL_RUMBLE_PERIOD_NS	50000000ULL	/* 50 ms */
#endif

/* A stop request sends one report synchronously, so the timer-driven
 * cascade has four reports left to send. */
#define NSL_RUMBLE_STOP_REMAINING \
	(NSL_RUMBLE_STOP_PKT_CNT - 1)

struct rumble_state {
	struct bpf_timer timer;
	struct bpf_wq wq;

	/* HID device ID used by hid_bpf_allocate_context(). */
	__u32 hid_id;

	/* Number of timer-driven neutral reports still required. */
	__u32 stop_remaining;

	/* Incremented for every new host rumble command. A queued work item
	 * from an older stop sequence becomes stale when this changes. */
	__u32 generation;
	__u32 work_generation;

	/* Nintendo output-report packet number for synthetic 0x10 reports. */
	__u8 rumble_ctr;

	__u8 wq_ready;
	__u8 timer_ready;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct rumble_state);
} sw001_rumble SEC(".maps");

static __always_inline bool nsl_is_neutral_rumble(const __u8 *r)
{
	/* SDL's Nintendo neutral state:
	 *   00 01 40 40 00 01 40 40
	 *
	 * Do not classify an all-zero field as neutral: the SW001 ignores that
	 * undecodable encoding rather than treating it as silence. */
	return r[0] == 0x00 && r[1] == 0x01 &&
	       r[2] == 0x40 && r[3] == 0x40 &&
	       r[4] == 0x00 && r[5] == 0x01 &&
	       r[6] == 0x40 && r[7] == 0x40;
}

static __always_inline void nsl_make_neutral(__u8 *report, __u8 packet_num)
{
	report[0] = 0x10;
	report[1] = packet_num;

	report[2] = 0x00;
	report[3] = 0x01;
	report[4] = 0x40;
	report[5] = 0x40;

	report[6] = 0x00;
	report[7] = 0x01;
	report[8] = 0x40;
	report[9] = 0x40;
}

/* The timer callback cannot call hid_bpf_hw_output_report() directly because
 * the timer callback is non-sleepable. It only kicks the sleepable workqueue
 * callback, which performs the actual HID output. */
static int nsl_rumble_timer_cb(void *map, int *key,
				       struct rumble_state *st)
{
	int ret;

	(void)map;
	(void)key;

	if (!st || st->stop_remaining == 0 || !st->wq_ready)
		return 0;

	/* Snapshot the generation which this work item belongs to. */
	st->work_generation = st->generation;

	ret = bpf_wq_start(&st->wq, 0);
	if (ret) {
		/* Timer callbacks are async callbacks. Their return value must be
		 * a verifier-known constant zero; never return bpf_wq_start()'s
		 * error directly. Retry the workqueue kick on the next period. */
		bpf_timer_start(&st->timer, NSL_RUMBLE_PERIOD_NS, 0);
	}

	return 0;
}

/* Send one synthetic neutral report from a sleepable bpf_wq callback. */
static int nsl_rumble_wq_cb(void *map, int *key, void *value)
{
	struct rumble_state *st = (struct rumble_state *)value;
	struct hid_bpf_ctx *hctx;
	__u8 zero[10];
	__u32 generation;
	int ret;

	(void)map;
	(void)key;

	if (!st || st->stop_remaining == 0)
		return 0;

	generation = st->work_generation;

	/* A non-zero rumble or a newer stop request superseded this queued
	 * work item before it got to the HID write. */
	if (generation != st->generation)
		return 0;

	hctx = hid_bpf_allocate_context(st->hid_id);
	if (!hctx)
		return 0;

	nsl_make_neutral(zero, st->rumble_ctr);

	ret = hid_bpf_hw_output_report(hctx, zero, sizeof(zero));

	hid_bpf_release_context(hctx);

	/* Re-check the generation after the potentially blocking HID output.
	 * If a new host rumble arrived meanwhile, never touch its state or arm
	 * another old stop timer. The already-issued zero is the only possible
	 * stale report in that race. */
	if (generation != st->generation)
		return 0;

	if (ret == (int)sizeof(zero)) {
		st->rumble_ctr = (st->rumble_ctr + 1) & 0x0f;
		if (st->stop_remaining > 0)
			st->stop_remaining--;

		if (st->stop_remaining > 0)
			/* Pace the NEXT output from this successful output. */
			bpf_timer_start(&st->timer, NSL_RUMBLE_PERIOD_NS, 0);
	} else if (st->stop_remaining > 0) {
		/* Do not consume a report on failure. Retry on the same 50 ms
		 * cadence instead of turning a transient error into a shortened
		 * stop cascade. */
		bpf_timer_start(&st->timer, NSL_RUMBLE_PERIOD_NS, 0);
	}

	/* bpf_wq callbacks are asynchronous callbacks as well. Their return
	 * value must be the verifier-known constant zero; ret is only used
	 * internally to decide whether the successful report consumed one
	 * stop packet. */
	return 0;
}

/*
 * Setting the workqueue callback is spelled differently across kernels, and
 * the arity differs too:
 *
 *   Linux 7.4 (Fedora):  bpf_wq_set_callback(wq, cb, flags)
 *   Steam Deck 6.16:     bpf_wq_set_callback_impl(wq, cb, flags, aux)
 *
 * Each build resolves exactly one of them, so the wrong name is never
 * referenced and libbpf cannot fail the load with "kfunc ... is referenced but
 * wasn't resolved". Build for the Deck with:  make WQ_CB_IMPL=1
 */
#ifndef SW001_WQ_CB_IMPL
#define SW001_WQ_CB_IMPL 0
#endif

#if SW001_WQ_CB_IMPL
#define nsl_wq_set_callback(wq, cb, flags) \
	bpf_wq_set_callback_impl((wq), (cb), (flags), NULL)
#else
#define nsl_wq_set_callback(wq, cb, flags) \
	bpf_wq_set_callback((wq), (cb), (flags))
#endif

/* Lazily initialize the timer/workqueue the first time a rumble report for
 * the clone passes through. Keeping initialization here avoids requiring a
 * separate userspace SEC("syscall") initializer. */
static __always_inline int nsl_rumble_init(struct rumble_state *st,
					   __u32 hid_id)
{
	int ret;

	st->hid_id = hid_id;

	if (!st->wq_ready) {
		ret = bpf_wq_init(&st->wq, &sw001_rumble, 0);
		if (ret && ret != -EBUSY)
			return ret;

		ret = nsl_wq_set_callback(&st->wq, nsl_rumble_wq_cb, 0);
		if (ret)
			return ret;

		st->wq_ready = 1;
	}

	if (!st->timer_ready) {
		ret = bpf_timer_init(&st->timer, &sw001_rumble,
					     CLOCK_MONOTONIC);
		if (ret && ret != -EBUSY)
			return ret;

		ret = bpf_timer_set_callback(&st->timer, nsl_rumble_timer_cb);
		if (ret)
			return ret;

		st->timer_ready = 1;
	}

	return 0;
}

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
		struct rumble_state *st;
		__u32 key0 = 0;
		__u32 generation = 0;
		bool is_stop;
		int init_ret;
		int send_ret;

		short_rumble[0] = data[0];
		short_rumble[1] = data[1];
		__builtin_memcpy(short_rumble + 2, data + 2, 8);

		is_stop = nsl_is_neutral_rumble(short_rumble + 2);
		st = bpf_map_lookup_elem(&sw001_rumble, &key0);

		if (st) {
			init_ret = nsl_rumble_init(st, hctx->hid->id);

			/* Every host rumble command starts a new generation. A
			 * non-zero command therefore cancels any old stop cascade. A
			 * stop command replaces the old sequence and resets its count. */
			st->generation++;
			generation = st->generation;
			st->rumble_ctr = (data[1] + 1) & 0x0f;

			if (is_stop && init_ret == 0)
				/* Five reports are required. The host packet below is
				 * the first one, and is consumed from the count only if
				 * the short re-send succeeds. */
				st->stop_remaining = NSL_RUMBLE_STOP_PKT_CNT;
			else
				/* A non-zero rumble immediately cancels any old cascade. */
				st->stop_remaining = 0;
		} else {
			init_ret = -1;
		}

		/* SDL's HIDAPI Switch driver sends the BT 0x10 report padded to
		 * 49 bytes. The SW001 only accepts the short 10-byte form. */
		send_ret = hid_bpf_hw_output_report(hctx, short_rumble,
					      sizeof(short_rumble));

		if (st && generation != st->generation)
			/* A newer host command arrived while the output was in flight.
			 * That newer command owns the rumble state now. */
			return hctx->size;

		if (send_ret != (int)sizeof(short_rumble)) {
			/* kfunc failed: preserve the current behavior and let hid-core
			 * forward the original padded report rather than reporting a
			 * short write to SDL. For a stop request, keep all five paced
			 * attempts because this synchronous attempt did not succeed. */
			if (st && init_ret == 0) {
				if (is_stop) {
					st->stop_remaining = NSL_RUMBLE_STOP_PKT_CNT;
					bpf_timer_start(&st->timer, NSL_RUMBLE_PERIOD_NS, 0);
				} else {
					st->stop_remaining = 0;
				}
			}
			return 0;
		}

		if (st && init_ret == 0 && is_stop) {
			/* The just-sent host packet is neutral #1. Consume it from the
			 * count, leaving four reports for the 50 ms timer cascade. */
			if (st->stop_remaining > 0)
				st->stop_remaining--;

			if (st->stop_remaining > 0)
				bpf_timer_start(&st->timer,
						 NSL_RUMBLE_PERIOD_NS, 0);
		}

		return hctx->size;
	}

	if (data[0] != 0x01)
		return 0;

	/* subcmd output report: byte 10 = subcmd id
	 * 0x02 = request device info (the SW001 never answers it; both SDL/Steam
	 * HIDAPI and hid-nintendo classify the controller from this reply and
	 * Steam Input uses it to decide the gyro feature is available -- a type
	 * of 0x03 (Pro Controller) + this unit's BD_ADDR is what they expect).
	 * Reply layout matches both consumers: ack 0x90 at byte 13, subcmd id at
	 * 14, data at 15+ (device type at 17, MAC at 19..24). */
	if (data[10] == 0x02) {
		reply[0] = 0x21;
		reply[13] = 0x90;	/* ACK + data following */
		reply[14] = 0x02;
		reply[15] = 0x02;	/* firmware version */
		reply[16] = 0x40;
		reply[17] = 0x03;	/* device type: Pro Controller */
		reply[19] = 0x9c;	/* BD_ADDR of the connected unit */
		reply[20] = 0x54;
		reply[21] = 0x00;
		reply[22] = 0x4c;
		reply[23] = 0xc9;
		reply[24] = 0x3b;
		hid_bpf_try_input_report(hctx, HID_INPUT_REPORT, reply,
					 sizeof(reply));
		return 0;
	}

	/* 0x10 = SPI flash read */
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

HID_BPF_OPS(sw001) = {
	.hid_rdesc_fixup = (void *)sw001_fix_rdesc,
	.hid_hw_output_report = (void *)sw001_fake_spi,
};
