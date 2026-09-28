CLANG ?= clang
CLANG_STRIP ?= llvm-strip
CFLAGS ?= -O2 -Wall
BPF_CFLAGS ?= -O2 -g -Wall

.PHONY: all clean deps inspect install uninstall deck-bundle check-kfuncs

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

# Is a kfunc callable on THIS kernel? See the comment at the top of the source
# for why a declared kfunc can still be unloadable.
tools/protodump: tools/protodump.c
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
	rm -f nsl-sw001.bpf.o vmlinux.h tools/gen_vmlinux_h tools/protodump
	rm -rf $(DECK_DIR) $(DECK_BUILD)

# Every kfunc this object calls, in one place, so a kernel that exports a
# placeholder instead of a real prototype is caught here rather than as an
# unexplained EINVAL at load time.
KFUNCS := bpf_wq_init bpf_wq_set_callback bpf_wq_start \
	  bpf_wq_set_callback_impl hid_bpf_get_data hid_bpf_hw_output_report \
	  hid_bpf_try_input_report hid_bpf_input_report \
	  hid_bpf_allocate_context hid_bpf_release_context \
	  bpf_timer_init bpf_timer_set_callback bpf_timer_start

check-kfuncs: tools/protodump
	@./tools/protodump $(KFUNCS) | tee /tmp/.sw001-kfuncs; \
	grep -q "not callable" /tmp/.sw001-kfuncs && { \
		echo "FAIL: a kfunc used by this object is not callable on this kernel"; \
		rm -f /tmp/.sw001-kfuncs; exit 1; }; \
	rm -f /tmp/.sw001-kfuncs; echo "all kfuncs callable"
