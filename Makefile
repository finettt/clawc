# clawc - pure C claw-style agent. Builds with any C99 compiler + POSIX libc.
#
# One build verb, everything else is a variable:
#
#   make                                   -> build/native/clawc
#   make TOOLCHAIN=musl                    -> build/native-musl/clawc   (static)
#   make BUILD_TYPE=debug                  -> build/native-debug/clawc
#   make CROSS=aarch64-linux-gnu           -> build/aarch64-linux-gnu/clawc
#   make CROSS=aarch64-linux-musl          -> build/aarch64-linux-musl/clawc (static)
#   make TLS=0 CHANNELS=telegram           -> build/native/clawc, minimal
#
# Build variables (all optional):
#   CROSS=<triple>          cross toolchain prefix ($(CROSS)-gcc, $(CROSS)-size)
#   TOOLCHAIN=gnu|musl      host libc flavour; musl uses musl-gcc   [gnu]
#   STATIC=0|1              static link            [1 for musl, else 0]
#   BUILD_TYPE=release|debug                                        [release]
#   TLS=0|1                 in-process OpenSSL     [auto: probe the compiler]
#   HARDEN=0|1              stack protector/FORTIFY/RELRO [1 release, 0 debug]
#   CHANNELS="..."          telegram matrix discord serve        [all]
#   MINISTRIP=0|1           ELF section-header strip [1 only for native release]
#   SIZE_LTO=auto|0         size-tuned LTO on x86-64 GCC         [auto]
#   OUTDIR=build            build root
#   TARGET=clawc            binary name; a value with '/' is used as a full path
#   HOSTCC=cc               compiler for build-time tools (textpack/ministrip)
#   STRIP=strip             post-link stripper; "true" disables it   [autodetect]
#
# Other verbs: test, test-asan, test-fuzz, test-fuzz-asan, size, clean, help.
# Changing any variable rebuilds automatically (flags are tracked per binary).

# ---------------------------------------------------------------- axes ----
CROSS      ?=
TOOLCHAIN  ?= gnu
BUILD_TYPE ?= release
OUTDIR     ?= build
TARGET     ?= clawc
# Host-side build tools (tools/textpack.c, tools/ministrip.c) run on the build
# machine, so they must never inherit the target compiler or its sysroot: a
# cross compiler (CROSS=... or CC="clang -target ... -isysroot ...") cannot
# produce a binary runnable here. Override HOSTCC= on device builds (iOS/Android)
# where the host compiler needs its own sysroot.
HOSTCC     ?= $(or $(CC_FOR_BUILD),cc)
# A missing stripper must not fail an otherwise successful link. Apple and
# jailbreak toolchains frequently ship no cctools/strip; llvm-strip works there.
STRIP      ?= $(or $(shell command -v strip >/dev/null 2>&1 && echo strip),\
                   $(shell command -v llvm-strip >/dev/null 2>&1 && echo llvm-strip),\
                   true)

ifneq ($(filter-out gnu musl,$(TOOLCHAIN)),)
$(error TOOLCHAIN must be gnu or musl, got '$(TOOLCHAIN)')
endif
ifneq ($(filter-out release debug,$(BUILD_TYPE)),)
$(error BUILD_TYPE must be release or debug, got '$(BUILD_TYPE)')
endif

IS_MUSL  := $(if $(filter musl,$(TOOLCHAIN))$(findstring musl,$(CROSS)),1)
IS_DEBUG := $(filter debug,$(BUILD_TYPE))

ifneq ($(CROSS),)
BUILD_CC := $(CROSS)-gcc
SIZE     := $(CROSS)-size
else ifeq ($(TOOLCHAIN),musl)
BUILD_CC := musl-gcc
SIZE     ?= size
else
BUILD_CC := $(CC)
SIZE     ?= size
endif

STATIC    ?= $(if $(IS_MUSL),1,0)
HARDEN    ?= $(if $(IS_DEBUG),0,1)
MINISTRIP ?= $(if $(CROSS)$(IS_MUSL)$(IS_DEBUG),0,1)

VARIANT := $(or $(CROSS),native)$(if $(and $(filter musl,$(TOOLCHAIN)),$(if $(CROSS),,1)),-musl)$(if $(and $(filter 1,$(STATIC)),$(if $(IS_MUSL),,1)),-static)$(if $(IS_DEBUG),-debug)
ifeq ($(findstring /,$(TARGET)),/)
BIN := $(TARGET)
else
BIN := $(OUTDIR)/$(VARIANT)/$(TARGET)
endif

TLS ?= $(shell printf '\043include <openssl/ssl.h>\n' | $(BUILD_CC) -E -x c - >/dev/null 2>&1 && echo 1 || echo 0)

ifeq ($(IS_DEBUG),)
CFLAGS  ?= -std=c99 -Oz -Wall -Wextra -ffunction-sections -fdata-sections \
		   -fno-asynchronous-unwind-tables -fno-unwind-tables \
		   -fomit-frame-pointer -fvisibility=hidden -fno-ident -pthread
LDFLAGS ?= -Wl,--gc-sections -Wl,--build-id=none -Wl,--hash-style=gnu -Wl,-z,noseparate-code
LINK_STRIP := -s
else
CFLAGS  ?= -std=c99 -O0 -g -Wall -Wextra -pthread
LDFLAGS ?=
LINK_STRIP :=
endif
LDLIBS  ?=

ifeq ($(IS_DEBUG),)
SIZE_TUNE := -fno-tree-sra -fno-jump-tables -fno-caller-saves -fno-tree-tail-merge -fno-if-conversion -fno-tree-ccp
SIZE_TUNE_OK := $(shell printf '' | $(BUILD_CC) $(SIZE_TUNE) -x c -c -o /dev/null - >/dev/null 2>&1 && echo yes)
ifeq ($(SIZE_TUNE_OK),yes)
CFLAGS += $(SIZE_TUNE)
endif
TLS_PLT_TUNE := $(shell printf '' | $(BUILD_CC) -Werror -fno-plt -x c -c -o /dev/null - >/dev/null 2>&1 && echo -fno-plt)
endif

# OS-specific tweaks. Apple targets use the Mach-O linker (the GNU flags break
# it); sandbox_init lives in libSystem, so no extra library. The OS is the one
# the compiler targets (its triple), so cross builds get the right flags;
# compilers without a triple fall back to the host.
TRIPLE    := $(shell $(BUILD_CC) -dumpmachine 2>/dev/null)
TARGET_OS := $(if $(findstring -apple-,$(TRIPLE)),Darwin,$(if $(findstring linux,$(TRIPLE)),Linux,$(shell uname -s)))
ifeq ($(TARGET_OS),Darwin)
LDFLAGS   = -Wl,-dead_strip
LDLIBS    =
LINK_STRIP :=
MINISTRIP := 0
endif

ifeq ($(STATIC),1)
LDFLAGS += -static -no-pie
endif

ifeq ($(SANDBOX_STUBS),1)
CFLAGS += -DCLAWC_SANDBOX_STUBS
endif

ifeq ($(HARDEN),1)
CFLAGS  += -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat=2 -Wformat-security
ifneq ($(TARGET_OS),Darwin)
LDFLAGS += -Wl,-z,relro,-z,now
endif
endif

SIZE_LTO ?= auto
ifeq ($(IS_DEBUG),)
ifneq ($(SIZE_LTO),0)
ifeq ($(TARGET_OS),Linux)
ifneq ($(filter x86_64%,$(TRIPLE)),)
SIZE_LTO_OK := $(shell tmp=".lto-probe.$$$$"; trap 'rm -f "$$tmp"' 0; \
	printf 'int main(void){return 0;}\n' | $(BUILD_CC) -std=c99 -flto \
	-fno-ipa-cp -fno-ipa-sra -x c - $(LDFLAGS) -o "$$tmp" >/dev/null 2>&1 && echo yes)
ifeq ($(SIZE_LTO_OK),yes)
CFLAGS += -flto -fno-ipa-cp -fno-ipa-sra
LDFLAGS += -flto
endif
endif
endif
endif
endif

ifeq ($(TLS),1)
CFLAGS += -DCLAWC_TLS
LDLIBS += -lssl -lcrypto
endif

CHANNELS ?= telegram matrix discord serve

SRC := $(wildcard src/*.c)
HDR := $(filter-out src/packed_text_data.h,$(wildcard src/*.h)) src/packed_text_data.h $(wildcard src/*.def)
ifneq ($(filter telegram,$(CHANNELS)),telegram)
CFLAGS += -DCLAWC_NO_TELEGRAM
SRC := $(filter-out src/telegram.c,$(SRC))
endif
ifneq ($(filter matrix,$(CHANNELS)),matrix)
CFLAGS += -DCLAWC_NO_MATRIX
SRC := $(filter-out src/matrix.c,$(SRC))
endif
ifneq ($(filter discord,$(CHANNELS)),discord)
CFLAGS += -DCLAWC_NO_DISCORD
SRC := $(filter-out src/discord.c src/ws.c,$(SRC))
endif
ifneq ($(filter serve,$(CHANNELS)),serve)
CFLAGS += -DCLAWC_NO_SERVE
endif

all: $(BIN)

FLAGS_SIG := $(subst ','\'',$(BUILD_CC) $(CFLAGS) $(TLS_PLT_TUNE) $(LDFLAGS) $(LDLIBS) $(LINK_STRIP) MINISTRIP=$(MINISTRIP) $(SRC))
$(BIN).flags: FORCE
	@mkdir -p $(dir $@)
	@echo '$(FLAGS_SIG)' | cmp -s - $@ || echo '$(FLAGS_SIG)' > $@

$(BIN): $(SRC) $(HDR) $(BIN).flags tools/ministrip.c tools/jaild.c tools/jailshim.c
	@mkdir -p $(dir $@)
	@set -e; obj="$@.bridge_tls.o"; trap 'rm -f "$$obj"' 0; \
	  $(BUILD_CC) $(CFLAGS) $(TLS_PLT_TUNE) -c src/bridge_tls.c -o "$$obj"; \
	  $(BUILD_CC) $(CFLAGS) -o $@ $(filter-out src/bridge_tls.c,$(SRC)) "$$obj" $(LDFLAGS) $(LDLIBS) $(LINK_STRIP)
ifeq ($(TARGET_OS),Darwin)
	$(if $(IS_DEBUG),,@$(STRIP) $@ || true)
endif
ifeq ($(MINISTRIP),1)
	@set -e; helper=".ministrip.$$$$"; trap 'rm -f "$$helper"' 0; \
	  $(HOSTCC) -std=c99 -O -o "$$helper" tools/ministrip.c; ./"$$helper" "$@"
endif
ifneq ($(TARGET_OS),Linux)
	@echo "[make] jail shim + jaild skipped for $(TARGET_OS): Linux-only sources (off64_t, seatbelt/seccomp)"
else
	$(BUILD_CC) -shared -fPIC -O2 -o $@-jail.so tools/jailshim.c -ldl || echo "[make] jail shim skipped (needs cc -shared + libdl)"
	$(BUILD_CC) $(CFLAGS) -o $@-jaild tools/jaild.c $(LINK_STRIP) || echo "[make] jaild skipped"
endif
	@echo "[make] built $@ ($(VARIANT), TLS=$(TLS), STATIC=$(STATIC), HARDEN=$(HARDEN))"

src/packed_text_data.h: src/packed_text.def src/tool_rows.h src/config.h src/util.h tools/textpack.c
	@set -e; exe=".textpack.$$$$"; hdr="$@.$$$$"; \
	  trap 'rm -f "$$exe" "$$hdr"' 0; \
	  $(HOSTCC) -std=c99 -O2 -o "$$exe" tools/textpack.c; \
	  ./"$$exe" > "$$hdr"; \
	  if cmp -s "$$hdr" "$@"; then touch "$@"; else mv -f "$$hdr" "$@"; fi

size: $(BIN)
	@$(SIZE) $(BIN)
	@ls -la $(BIN) | awk '{printf "file size: %s bytes\n", $$5}'

test: $(BIN)
	HOSTCC="$(HOSTCC)" python3 test/run_python_tests.py --binary "$(BIN)" $(if $(filter 1,$(HARDEN)),,--skip-harden)

test-asan: $(BIN)
	HOSTCC="$(HOSTCC)" CC="$(CC)" python3 test/run_python_tests.py --binary "$(BIN)" --sanitizers $(if $(filter 1,$(HARDEN)),,--skip-harden)

test-fuzz:
	CC="$(CC)" python3 test/fuzz_json.py 100000

test-fuzz-asan:
	CC="$(CC)" python3 test/fuzz_json.py 100000 --sanitize

test-python: test
test-python-asan: test-asan

help:
	@echo "Build:  make [VAR=value ...]        -> $(OUTDIR)/<variant>/$(TARGET)"
	@echo ""
	@echo "  make                              native release  -> build/native/clawc"
	@echo "  make TOOLCHAIN=musl               static musl     -> build/native-musl/clawc"
	@echo "  make BUILD_TYPE=debug             debug, -O0 -g   -> build/native-debug/clawc"
	@echo ""
	@echo "Variables: CROSS TOOLCHAIN=gnu|musl STATIC=0|1 BUILD_TYPE=release|debug HOSTCC STRIP"
	@echo "           TLS=0|1 HARDEN=0|1 CHANNELS=\"...\" MINISTRIP=0|1 OUTDIR TARGET"
	@echo "Verbs:     all test test-asan test-fuzz size clean help"
	@echo ""
	@echo "Current:   $(BIN)  (CC=$(BUILD_CC) TLS=$(TLS) STATIC=$(STATIC) HARDEN=$(HARDEN))"

clean:
	rm -rf $(OUTDIR)
	rm -f clawc clawc-debug clawc-static clawc-*

FORCE:
.PHONY: all size test test-asan test-fuzz test-fuzz-asan test-python test-python-asan help clean FORCE
