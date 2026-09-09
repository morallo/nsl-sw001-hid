CLANG ?= clang
CLANG_STRIP ?= llvm-strip
CFLAGS ?= -O2 -Wall
BPF_CFLAGS ?= -O2 -g -Wall

# Defaults: paced zero stop cascade on (NSL_RUMBLE_STOP_CASCADE=1), 36 zero
# packets (NSL_RUMBLE_ZERO_TOTAL=36) delivered 2 per 0x30 tick
# (NSL_ZEROS_PER_TICK=2): a ~200 ms span of pair-spaced zeros, denser early
# than the proven 1-per-tick baseline. The proven-reliable profile is
# `make NSL_RUMBLE_ZERO_TOTAL=20 NSL_ZEROS_PER_TICK=1`. Override with
# `make NSL_RUMBLE_STOP_CASCADE=0` to disable the cascade entirely.
ifdef NSL_RUMBLE_STOP_CASCADE
BPF_CFLAGS += -DNSL_RUMBLE_STOP_CASCADE=$(NSL_RUMBLE_STOP_CASCADE)
endif
ifdef NSL_ZEROS_PER_TICK
BPF_CFLAGS += -DNSL_ZEROS_PER_TICK=$(NSL_ZEROS_PER_TICK)
endif
ifdef NSL_RUMBLE_ZERO_TOTAL
BPF_CFLAGS += -DNSL_RUMBLE_ZERO_TOTAL=$(NSL_RUMBLE_ZERO_TOTAL)
endif

.PHONY: all clean deps inspect install uninstall deck-bundle

all: nsl-sw001.bpf.o

deps:
	@for c in $(CLANG) $(CLANG_STRIP) gcc; do \
		command -v $$c >/dev/null 2>&1 || \
			{ echo "MISSING: $$c"; exit 1; }; \
	done
	@test -r /sys/kernel/btf/vmlinux || { echo "MISSING: /sys/kernel/btf/vmlinux (CONFIG_DEBUG_INFO_BTF)"; exit 1; }
	@command -v udev-hid-bpf >/dev/null 2>&1 || \
		{ echo "MISSING: udev-hid-bpf — dnf install udev-hid-bpf"; exit 1; }
	@echo "toolchain ok (no bpftool needed)"

tools/gen_vmlinux_h: tools/gen_vmlinux_h.c
	$(CC) $(CFLAGS) -o $@ $< -lbpf

vmlinux.h: tools/gen_vmlinux_h
	./$< > $@

nsl-sw001.bpf.o: nsl-sw001.bpf.c vmlinux.h vendor/hid_bpf.h vendor/hid_bpf_helpers.h vendor/hid_report_descriptor_helpers.h
	$(CLANG) $(BPF_CFLAGS) -target bpf -D__TARGET_ARCH_x86 \
		-Ivendor -I. -c $< -o $@
	$(CLANG_STRIP) -g $@

inspect: nsl-sw001.bpf.o
	udev-hid-bpf inspect nsl-sw001.bpf.o

install: deps nsl-sw001.bpf.o
	sudo udev-hid-bpf install --dry-run nsl-sw001.bpf.o
	@echo "--- dry run ok, real install:"
	sudo udev-hid-bpf install nsl-sw001.bpf.o

uninstall:
	sudo rm -f /etc/udev/rules.d/*nsl-sw001* /etc/udev-hid-bpf/*nsl-sw001*
	@echo "removed installed udev rule + program for nsl-sw001"

# Steam Deck bundle: the self-contained loader + bundled .so deps + the
# prebuilt object, staged under steamdeck/loader/ for the deck installer
# (which copies it to /home/.steamos-nsl-sw001-bpf/ -- no pacman, no /usr).
DECK_LOADER := steamdeck/sw001-bpf-attach.c
DECK_DIR := steamdeck/loader
DECK_LIBS := libbpf.so.1 libelf.so.1 libz.so.1 libzstd.so.1

deck-bundle: nsl-sw001.bpf.o
	mkdir -p $(DECK_DIR)/lib
	$(CC) $(CFLAGS) -o $(DECK_DIR)/sw001-bpf-attach $(DECK_LOADER) \
		$(shell pkg-config --cflags libbpf) -Wl,-rpath,'$$ORIGIN/lib' \
		-lbpf -lelf -lz -lzstd
	@for lib in $(DECK_LIBS); do \
		real=$$(ldconfig -p | awk -v l="$$lib" '$$1==l { print $$NF; exit }'); \
		[ -n "$$real" ] || { echo "MISSING: $$lib (stage sw001-bpf-attach deps)"; exit 1; }; \
		cp -L "$$real" $(DECK_DIR)/lib/$$lib; \
		echo "bundled $$lib <- $$real"; \
	done
	cp -L nsl-sw001.bpf.o $(DECK_DIR)/
	@echo "--- deck bundle ready in $(DECK_DIR)/ (copy the whole steamdeck/ tree to the Deck)"

clean:
	rm -f nsl-sw001.bpf.o vmlinux.h tools/gen_vmlinux_h
	rm -rf $(DECK_DIR)