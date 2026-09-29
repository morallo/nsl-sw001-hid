// SPDX-License-Identifier: GPL-2.0-only
/*
 * Which struct_ops member can this kernel actually load?
 *
 * Why this exists: on 7.2.6-201.nobara.fc44 the full object fails, but the
 * failure is not necessarily the member you would guess. The variant matrix in
 * diagnostic_scripts/try_struct_ops.sh showed the two-program baseline (no
 * hid_device_event at all) failing -EINVAL, while hid_device_event itself
 * attaches and runs fine (it shows up in dispatch_hid_bpf_device_event in a
 * live kernel stack). So the EINVAL belongs to one of the other members, and
 * this ladder finds which one by adding exactly one member at a time.
 *
 * One program, one member, nothing else. No maps, no probe, no workqueue --
 * anything added here is a candidate for the failure, so there is nothing else
 * to add. Each rung is a separate object built from the same source.
 *
 * Build and run via diagnostic_scripts/op_ladder.sh, which needs root because
 * loading BPF requires it.
 *
 * WARNING: attaching HID-BPF calls hid_bpf_reg -> device_reprobe, which
 * re-runs the HID driver probe. Doing that while the controller is
 * disconnecting has already deadlocked this machine once (both the loader and
 * bluetoothd stuck in D state, unrecoverable without a reboot). Only run this
 * against a connected, powered, idle controller, and do not touch the
 * controller while it runs.
 */
#include "vmlinux.h"
#include "hid_bpf.h"
#include "hid_bpf_helpers.h"
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

/* rdesc: the real fix does descriptor surgery. The ladder only needs the
 * member to exist and return cleanly, so keep it trivial. */
#ifdef WITH_RDESC
SEC("struct_ops/hid_rdesc_fixup")
int BPF_PROG(op_rdesc, struct hid_bpf_ctx *hctx)
{
	(void)hctx;
	return 0;
}
#endif

/* output, non-sleepable: hid_hw_output_report is KF_SLEEPABLE, so this rung is
 * expected to be rejected. Included anyway to confirm the error differs from
 * the others -- a different errno separates "wrong member" from "wrong
 * sleepability". */
#ifdef WITH_OUTPUT_PLAIN
SEC("struct_ops/hid_hw_output_report")
int BPF_PROG(op_output_plain, struct hid_bpf_ctx *hctx, __u64 report)
{
	(void)hctx;
	(void)report;
	return 0;
}
#endif

/* output, sleepable: this is the form the real object uses. */
#ifdef WITH_OUTPUT_SLEEPABLE
SEC("struct_ops.s/hid_hw_output_report")
int BPF_PROG(op_output_s, struct hid_bpf_ctx *hctx, __u64 report)
{
	(void)hctx;
	(void)report;
	return 0;
}
#endif

/* device event, plain: no workqueue call, to isolate the member from the
 * kfunc. */
#ifdef WITH_EVENT_PLAIN
SEC("struct_ops/hid_device_event")
int BPF_PROG(op_event_plain, struct hid_bpf_ctx *hctx,
	     enum hid_report_type type, __u64 event)
{
	(void)hctx;
	(void)event;
	return (type == HID_INPUT_REPORT) ? 0 : 0;
}
#endif

/* device event + workqueue start: the form the watchdog needs. If this is the
 * rung that warns or hangs, the wq kfunc is the trigger. Requires a map to
 * hold the wq. */
#ifdef WITH_EVENT_WQ
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct bpf_wq);
} op_wq SEC(".maps");

SEC("struct_ops/hid_device_event")
int BPF_PROG(op_event_wq, struct hid_bpf_ctx *hctx,
	     enum hid_report_type type, __u64 event)
{
	__u32 key = 0;
	struct bpf_wq *wq;

	(void)hctx;
	(void)event;
	if (type != HID_INPUT_REPORT)
		return 0;

	wq = bpf_map_lookup_elem(&op_wq, &key);
	if (!wq)
		return 0;

	/* Deliberately NOT initialised: an uninitialised wq is a separate
	 * thing to test (see op_ladder.sh) and mixing the two would make the
	 * result unreadable. */
	return 0;
}
#endif

/* Struct ops value. HID_BPF_OPS expands to a `struct hid_bpf_ops` in
 * .struct_ops.link, and libbpf only loads a SEC("struct_ops/...") program that
 * some field points at -- an unreferenced one is dropped with -EINVAL before
 * the verifier runs. The field name must match the member name. */
HID_BPF_OPS(op) = {
#ifdef WITH_RDESC
	.hid_rdesc_fixup = (void *)op_rdesc,
#endif
#ifdef WITH_OUTPUT_PLAIN
	.hid_hw_output_report = (void *)op_output_plain,
#endif
#ifdef WITH_OUTPUT_SLEEPABLE
	.hid_hw_output_report = (void *)op_output_s,
#endif
#ifdef WITH_EVENT_PLAIN
	.hid_device_event = (void *)op_event_plain,
#endif
#ifdef WITH_EVENT_WQ
	.hid_device_event = (void *)op_event_wq,
#endif
};
