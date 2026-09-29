/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Is a kfunc actually callable here?
 *
 * libbpf resolves a call by name to a BTF_KIND_FUNC entry, then checks the
 * call against that function's prototype. If the kernel only registered a
 * placeholder, the call fails as "kfunc 'X' is referenced but wasn't
 * resolved" -- an EINVAL at load, before the verifier runs, with no verifier
 * log to explain it. The tell is the prototype: real kfuncs take pointers,
 * stubs are a row of raw integers.
 *
 *   make protodump OBJ=/path/to/kernel
 *   protodump bpf_timer_init bpf_wq_init ...
 *
 * Reads $VMLINUX_BTF (default /sys/kernel/btf/vmlinux) and needs no root.
 * Kernel BTF_KIND numbers: 0 void, 1 int, 4 struct, 8 int(64), 13 func_proto.
 */
#include <stdio.h>
#include <string.h>
#include <bpf/btf.h>

static const char *name_of(struct btf *btf, int id)
{
	const struct btf_type *t = btf__type_by_id(btf, id);

	return (t && t->name_off) ? btf__name_by_offset(btf, t->name_off) : NULL;
}

static int show(struct btf *btf, int id, const char *want)
{
	const struct btf_type *f = btf__type_by_id(btf, id);
	const struct btf_type *fp;
	const struct btf_param *ps;
	int i, nargs;

	if (!btf_is_func(f))
		return 0;
	if (!name_of(btf, id) || strcmp(name_of(btf, id), want))
		return 0;

	fp = btf__type_by_id(btf, f->type);
	if (!fp || !btf_is_func_proto(fp)) {
		printf("  %-24s FUNC  id=%-6d proto missing -> not callable\n",
		       want, id);
		return 1;
	}

	ps = btf_params(fp);
	nargs = (int)btf_vlen(fp);
	printf("  %-24s FUNC  id=%-6d nargs=%d ", want, id, nargs);
	for (i = 0; i < nargs; i++) {
		const struct btf_type *p = btf__type_by_id(btf, ps[i].type);

		if (p && btf_is_ptr(p)) {
			const struct btf_type *pt = btf__type_by_id(btf, p->type);
			const char *n = name_of(btf, p->type);

			printf("ptr ");
			if (pt && btf_is_func_proto(pt))
				printf("(callback) ");
			else if (n)
				printf("%s* ", n);
			else
				printf("void* ");
		} else {
			printf("int ");
		}
	}
	/* NOTE: this resolves kfuncs by NAME against vmlinux BTF, which finds
	 * whatever prototype the kernel publishes under that name -- not the
	 * bpf_kfunc id the program actually references. The two can differ, so
	 * the signature below is informational only.
	 *
	 * It is deliberately NOT a pass/fail signal. An earlier version of this
	 * tool called an all-scalar prototype "STUB, not callable" and failed
	 * the build on it. That is wrong: on 7.2.6-201.nobara.fc44 it flags
	 * bpf_timer_init as "nargs=5 int int int int int", yet the timer
	 * resolves, loads, and drives a working 50 ms rumble-stop cascade
	 * (10/10 verified on the wire). A false positive here caused working
	 * code to be deleted on the strength of a tool's opinion.
	 *
	 * Only the "proto missing" case above is a real failure, and that is
	 * what the build greps for. The load itself is the ground truth. */
	printf("-> present (name lookup; advisory)\n");
	return 1;
}

int main(int argc, char **argv)
{
	struct btf *btf;
	int i, cnt;

	if (argc < 2) {
		fprintf(stderr, "usage: %s KFUNC...\n", argv[0]);
		return 2;
	}

	btf = btf__load_vmlinux_btf();
	if (!btf) {
		fprintf(stderr, "no vmlinux BTF (CONFIG_DEBUG_INFO_BTF=y?)\n");
		return 1;
	}

	cnt = (int)btf__type_cnt(btf);
	for (i = 1; i < argc; i++) {
		int j, found = 0;

		for (j = 1; j < cnt; j++)
			found |= show(btf, j, argv[i]);
		if (!found)
			printf("  %-24s absent from BTF -> NOT CALLABLE\n", argv[i]);
	}

	btf__free(btf);
	return 0;
}
