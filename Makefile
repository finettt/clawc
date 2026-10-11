# clawc - pure C claw-style agent. Builds with any C99 compiler + POSIX libc.
CC      ?= cc
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
HDR := $(wildcard src/*.h)
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

# Native build (x86-64 host). Binary stays far under 100 KB.
all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(if $(CROSS),$(CROSS)-gcc,$(CC)) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS) $(if $(filter Darwin,$(UNAME_S)),,-s)
	$(if $(filter Darwin,$(UNAME_S)),$(STRIP) $@,$(CC) -std=c99 -O -o .ministrip tools/ministrip.c && ./.ministrip $@ && rm -f .ministrip)
	$(CC) -shared -fPIC -O2 -o $@-jail.so tools/jailshim.c -ldl || echo "[make] jail shim skipped (needs cc -shared + libdl)"
	$(if $(CROSS),$(CROSS)-gcc,$(CC)) $(CFLAGS) -o $@-jaild tools/jaild.c -s || echo "[make] jaild skipped"

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
	bash test/run_tests.sh

clean:
	rm -f $(TARGET) $(TARGET)-debug $(TARGET)-static $(TARGET)-*

.PHONY: all cross musl size debug test clean
