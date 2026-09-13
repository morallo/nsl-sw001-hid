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

# Steam Deck bundle: a single statically-linked loader + the prebuilt object,
# staged under steamdeck/loader/ for the deck installer.
#
# The loader links libbpf + libelf statically, so the Deck needs NOTHING from
# its userspace: no bundled .so, no RPATH, no glibc symbol-version sensitivity
# (this is what frees it from ABI drift between the build machine and SteamOS).
#
# Dev-machine build requirements (once):
#   sudo dnf install -y glibc-static libbpf-static libzstd-static zlib-ng-compat-static
# libelf/libeu have no Fedora static package, so they are built once from
# elfutils source into .build/ (cached; needs network only when missing).
DECK_LOADER := steamdeck/sw001-bpf-attach.c
DECK_DIR := steamdeck/loader
DECK_BUILD := .build
DECK_ELF := $(DECK_BUILD)/elfutils
DECK_ELF_TAR := $(DECK_BUILD)/elfutils-0.193.tar.bz2
DECK_ELF_LIBS := $(DECK_ELF)/libelf/libelf.a $(DECK_ELF)/lib/libeu.a

$(DECK_BUILD):
	mkdir -p $@

$(DECK_ELF_LIBS): | $(DECK_BUILD)
	@test -f "$(DECK_ELF_TAR)" || curl -sL --max-time 300 \
		-o "$(DECK_ELF_TAR)" https://sourceware.org/elfutils/ftp/0.193/elfutils-0.193.tar.bz2
	rm -rf $(DECK_ELF)
	tar xjf "$(DECK_ELF_TAR)" -C $(DECK_BUILD)
	cd $(DECK_BUILD)/elfutils-0.193 && \
		./configure --prefix=$(abspath $(DECK_ELF)) --disable-debuginfod \
			--disable-libdebuginfod --disable-programs --without-bzlib \
			--without-lzma --without-zstd
	$(MAKE) -C $(DECK_BUILD)/elfutils-0.193/lib libeu.a
	$(MAKE) -C $(DECK_BUILD)/elfutils-0.193/libelf libelf.a
	mkdir -p $(DECK_ELF)/libelf $(DECK_ELF)/lib
	cp $(DECK_BUILD)/elfutils-0.193/libelf/libelf.a $(DECK_ELF)/libelf/
	cp $(DECK_BUILD)/elfutils-0.193/lib/libeu.a $(DECK_ELF)/lib/

deck-bundle: nsl-sw001.bpf.o $(DECK_ELF_LIBS)
	@test -f /usr/lib64/libc.a || { echo "MISSING: glibc-static (sudo dnf install glibc-static)"; exit 1; }
	@test -f /usr/lib64/libbpf.a || { echo "MISSING: libbpf-static (sudo dnf install libbpf-static)"; exit 1; }
	@test -f /usr/lib64/libz.a || { echo "MISSING: zlib-ng-compat-static"; exit 1; }
	@test -f /usr/lib64/libzstd.a || { echo "MISSING: libzstd-static"; exit 1; }
	mkdir -p $(DECK_DIR)
	$(CC) $(CFLAGS) -s -static -no-pie -o $(DECK_DIR)/sw001-bpf-attach $(DECK_LOADER) \
		-lbpf $(DECK_ELF)/libelf/libelf.a $(DECK_ELF)/lib/libeu.a \
		-lz -lzstd -lm
	cp -L nsl-sw001.bpf.o $(DECK_DIR)/
	@file $(DECK_DIR)/sw001-bpf-attach
	@echo "--- deck bundle ready in $(DECK_DIR)/: sw001-bpf-attach (statically linked) + nsl-sw001.bpf.o"

clean:
	rm -f nsl-sw001.bpf.o vmlinux.h tools/gen_vmlinux_h
	rm -rf $(DECK_DIR) $(DECK_BUILD)