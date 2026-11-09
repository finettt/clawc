# clawc - pure C claw-style agent. Builds with any C99 compiler + POSIX libc.
CC      ?= cc
HOSTCC  ?= cc
CROSS   ?=
TARGET  ?= clawc
# Built-in TLS (OpenSSL, dynamic link). TLS=1 auto-detected; force with `make TLS=1`
# or disable with `make TLS=0` (https:// then goes through the curl subprocess).
TLS     ?= $(shell test -e /usr/include/openssl/ssl.h && echo 1 || echo 0)
HARDEN  ?= 1
# -Oz needs GCC12+/clang; use CFLAGS="-Os ..." on older compilers.
CFLAGS  ?= -std=c99 -Oz -Wall -Wextra -ffunction-sections -fdata-sections \
           -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -fomit-frame-pointer -fvisibility=hidden -fno-ident -pthread
# GCC size tuning: disable several growth-prone optimizations.  Probe the
# compiler so unsupported toolchains retain the ordinary portable defaults.
SIZE_TUNE := -fno-tree-sra -fno-jump-tables -fno-caller-saves -fno-tree-tail-merge -fno-if-conversion -fno-tree-ccp
SIZE_CC := $(if $(CROSS),$(CROSS)-gcc,$(CC))
SIZE_TUNE_OK := $(shell printf '' | $(SIZE_CC) $(SIZE_TUNE) -x c -c -o /dev/null - >/dev/null 2>&1 && echo yes)
# TLS has many infrequently called imported functions. Direct GOT calls save
# PLT stubs on toolchains that support this flag; other compilers use defaults.
TLS_PLT_TUNE := $(shell printf '' | $(SIZE_CC) -Werror -fno-plt -x c -c -o /dev/null - >/dev/null 2>&1 && echo -fno-plt)
ifeq ($(SIZE_TUNE_OK),yes)
CFLAGS += $(SIZE_TUNE)
endif
LDFLAGS ?= -Wl,--gc-sections -Wl,--build-id=none -Wl,--hash-style=gnu -Wl,-z,noseparate-code
LDLIBS  ?=
STRIP   ?= strip

# OS-specific tweaks.  Linux keeps the GNU-ld flags above; macOS uses the Mach-O
# linker (the GNU flags break it), needs -lsandbox for the sandbox_init backend,
# and tools/ministrip.c (ELF post-processing) is skipped.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LDFLAGS = -Wl,-dead_strip
LDLIBS  = -lsandbox
endif

# Test hook: SANDBOX_STUBS=1 compiles the no-OS-sandbox stubs on any platform
# (the suite exercises the path-check fallback with it).
ifeq ($(SANDBOX_STUBS),1)
CFLAGS += -DCLAWC_SANDBOX_STUBS
endif

# Binary hardening (on by default; make HARDEN=0 to drop for debugging).
# Stack protector + FORTIFY + full RELRO; sizes stay suite-enforced, so these
# are tuned to fit the 100KB budget alongside the features.
ifeq ($(HARDEN),1)
CFLAGS  += -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat=2 -Wformat-security
ifneq ($(UNAME_S),Darwin)
LDFLAGS += -Wl,-z,relro,-z,now
endif
endif

# On x86-64 GCC, LTO plus restrained interprocedural cloning avoids an ELF
# padding boundary without altering load segments. Probe an actual link with
# the selected linker: GCC LTO is not usable with every linker. Other targets,
# compilers and unsupported linkers retain their normal flags. SIZE_LTO=0
# explicitly disables this optimization; it never disables hardening.
SIZE_LTO ?= auto
ifneq ($(SIZE_LTO),0)
ifeq ($(UNAME_S),Linux)
ifneq ($(filter x86_64%,$(shell $(SIZE_CC) -dumpmachine 2>/dev/null)),)
SIZE_LTO_OK := $(shell tmp=".lto-probe.$$$$"; trap 'rm -f "$$tmp"' 0; \
    printf 'int main(void){return 0;}\n' | $(SIZE_CC) -std=c99 -flto \
    -fno-ipa-cp -fno-ipa-sra -x c - $(LDFLAGS) -o "$$tmp" >/dev/null 2>&1 && echo yes)
ifeq ($(SIZE_LTO_OK),yes)
CFLAGS += -flto -fno-ipa-cp -fno-ipa-sra
LDFLAGS += -flto
endif
endif
endif
endif

ifeq ($(TLS),1)
CFLAGS += -DCLAWC_TLS
LDLIBS += -lssl -lcrypto
endif

# Channels: compile only the ones you actually use.
#   make CHANNELS="telegram"                # just the Telegram bot
#   make CHANNELS="serve"                   # just the TCP bridge
#   make CHANNELS=""                        # no channels (chat/run/config/doctor)
# The agent core - cron, heartbeat, memory, subagents, sandbox, doctor - is
# always included; CHANNELS only gates messaging front ends.
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

# Native build; executable sizes depend on the platform and toolchain.
all: $(TARGET)

$(TARGET): $(SRC) $(HDR) tools/ministrip.c tools/jaild.c tools/jailshim.c
	@set -e; obj="$@.bridge_tls.o"; trap 'rm -f "$$obj"' 0; \
	  $(SIZE_CC) $(CFLAGS) $(TLS_PLT_TUNE) -c src/bridge_tls.c -o "$$obj"; \
	  $(SIZE_CC) $(CFLAGS) -o $@ $(filter-out src/bridge_tls.c,$(SRC)) "$$obj" $(LDFLAGS) $(LDLIBS) $(if $(filter Darwin,$(UNAME_S)),,-s)
	$(if $(filter Darwin,$(UNAME_S)),$(STRIP) $@,	  @set -e; helper=".ministrip.$$$$"; trap 'rm -f "$$helper"' 0; \
	    $(HOSTCC) -std=c99 -O -o "$$helper" tools/ministrip.c; ./"$$helper" "$@")
	$(SIZE_CC) -shared -fPIC -O2 -o $@-jail.so tools/jailshim.c -ldl || echo "[make] jail shim skipped (needs cc -shared + libdl)"
	$(if $(CROSS),$(CROSS)-gcc,$(CC)) $(CFLAGS) -o $@-jaild tools/jaild.c -s || echo "[make] jaild skipped"

# Rebuild the losslessly packed built-in texts whenever their source changes.
# The generator runs on the build host even for cross-compiles. Each invocation
# uses private temporary files, then publishes the header with an atomic rename.
src/packed_text_data.h: src/packed_text.def src/tool_rows.h src/config.h src/util.h tools/textpack.c
	@set -e; exe=".textpack.$$$$"; hdr="$@.$$$$"; \
	  trap 'rm -f "$$exe" "$$hdr"' 0; \
	  $(HOSTCC) -std=c99 -O2 -o "$$exe" tools/textpack.c; \
	  ./"$$exe" > "$$hdr"; \
	  if cmp -s "$$hdr" "$@"; then touch "$@"; else mv -f "$$hdr" "$@"; fi

# Cross builds for the portability targets:
#   make cross CROSS=aarch64-linux-gnu TLS=1
#   make cross CROSS=riscv64-linux-gnu TLS=1
#   make cross CROSS=arm-linux-gnueabihf TLS=1
#   make cross CROSS=i686-linux-gnu TLS=1
cross: $(SRC) $(HDR)
	$(CROSS)-gcc $(CFLAGS) -o $(TARGET)-$(CROSS) $(SRC) $(LDFLAGS) $(LDLIBS) -s
	$(CROSS)-size $(TARGET)-$(CROSS)

# Fully static (musl) - one file that runs anywhere on the arch, ~150KB,
# peak RSS measured at 4KB. Needs a musl-gcc (apt: musl-tools, or musl.cc).
# Static builds use TLS=0 (https via curl); the dynamic build has in-process TLS.
musl: $(SRC) $(HDR)
	musl-gcc $(CFLAGS) -o $(TARGET)-static $(SRC) $(LDFLAGS) -static -no-pie -s
	musl-gcc -shared -fPIC -O2 -o $(TARGET)-static-jail.so tools/jailshim.c -ldl || echo "[make] jail shim skipped"

size: $(TARGET)
	@size $(TARGET)
	@ls -la $(TARGET) | awk '{printf "file size: %s bytes\n", $$5}'

debug: $(SRC) $(HDR)
	$(CC) -std=c99 -O0 -g -Wall -Wextra -o $(TARGET)-debug $(SRC) $(LDLIBS)

test: $(TARGET)
	HOSTCC="$(HOSTCC)" sh test/text_pack_test.sh
	HOSTCC="$(HOSTCC)" sh test/json_unicode_test.sh
	HOSTCC="$(HOSTCC)" python3 test/ministrip_test.py
	@if [ "$(HARDEN)" = 1 ] && [ "$(UNAME_S)" = Linux ]; then python3 test/elf_hardening_test.py "$(TARGET)"; fi
	python3 test/cli_config_test.py "$(TARGET)"
	bash test/run_tests.sh

clean:
	rm -f $(TARGET) $(TARGET)-debug $(TARGET)-static $(TARGET)-*

.PHONY: all cross musl size debug test clean
