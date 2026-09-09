#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <bpf/libbpf.h>
#include <bpf/btf.h>

static void printf_cb(void *ctx, const char *fmt, va_list args)
{
	(void)ctx;
	vprintf(fmt, args);
}

int main(void)
{
	struct btf *btf;
	struct btf_dump *d;
	int err, i, n;

	btf = btf__load_vmlinux_btf();
	if (!btf) {
		fprintf(stderr, "failed to load /sys/kernel/btf/vmlinux\n");
		return 1;
	}

	d = btf_dump__new(btf, printf_cb, NULL, NULL);
	if (!d) {
		fprintf(stderr, "btf_dump__new failed\n");
		btf__free(btf);
		return 1;
	}

	printf("#ifndef __VMLINUX_H__\n");
	printf("#define __VMLINUX_H__\n\n");
	printf("#ifndef BPF_NO_PRESERVE_ACCESS_INDEX\n");
	printf("#pragma clang attribute push (__attribute__((preserve_access_index)), apply_to = record)\n");
	printf("#endif\n\n");
	printf("#ifndef __ksym\n#define __ksym __attribute__((section(\".ksyms\")))\n#endif\n\n");
	printf("#ifndef __weak\n#define __weak __attribute__((weak))\n#endif\n\n");
	printf("#ifndef __bpf_fastcall\n#if __has_attribute(bpf_fastcall)\n#define __bpf_fastcall __attribute__((bpf_fastcall))\n#else\n#define __bpf_fastcall\n#endif\n#endif\n\n");

	n = btf__type_cnt(btf);
	for (i = 1; i < n; i++) {
		err = btf_dump__dump_type(d, i);
		if (err) {
			fprintf(stderr, "btf_dump__dump_type(%d) failed: %d\n", i, err);
			return err;
		}
	}

	printf("#ifndef BPF_NO_PRESERVE_ACCESS_INDEX\n");
	printf("#pragma clang attribute pop\n");
	printf("#endif\n\n");
	printf("#endif /* __VMLINUX_H__ */\n");

	btf_dump__free(d);
	btf__free(btf);
	return 0;
}