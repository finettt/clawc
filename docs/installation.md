# Installation

Building, installing, cross-compiling, and running `clawc` as a service.

## Building

Any C99 compiler + POSIX libc (Linux, macOS, BSD). If OpenSSL headers are
present, built-in TLS is enabled automatically (`TLS=1`):

```sh
make            # -> ./clawc (TLS auto-detected)
make musl       # fully static ~150KB, 4KB peak RSS (needs musl-gcc)
make TLS=0      # no OpenSSL: https:// goes through the curl subprocess
make size       # show binary size
make test       # end-to-end checks (mock LLM + Telegram + Matrix + Discord)
bash test/asan_check.sh   # ASan/UBSan smoke checks; run make test first (TLS fixtures)
make debug      # unoptimized debug build
```

The Telegram budget build (`make CHANNELS=telegram`) ships under 100 KB; the
all-channels build (telegram + matrix + discord + serve + a2a) is ~124 KB.
Channels use `-Oz` (GCC 12+/clang; pass `CFLAGS="-Os ..."` on older compilers).

### Build only the channels you use

Messaging front ends are compile-time optional — no dead weight, no channels
you never asked for:

```sh
make CHANNELS="telegram"    # just the Telegram bot
make CHANNELS="serve"       # just the TCP bridge
make CHANNELS=""            # no channels: chat/run/config/doctor/setup
```

Channels: `telegram`, `matrix`, `discord`, `serve` (all on by default). Everything else — cron,
heartbeat, memory, subagents, sandboxing, doctor — is agent core and always
included. A build without a channel says so honestly (`clawc doctor` prints a
`[features]` line; `clawc telegram` explains `'telegram' is not compiled into
this build`). The setup wizard skips Telegram prompts and the daemon never
plans a gateway that isn't compiled in.

### Cross-compilation (x86 / ARM / RISC-V)

No architecture-specific code exists; any cross toolchain works. Point the
compiler at your target's OpenSSL or pass `TLS=0`:

```sh
make cross CROSS=aarch64-linux-gnu TLS=1     # ARM 64-bit (Pi 3/4/5, Graviton)
make cross CROSS=arm-linux-gnueabihf TLS=1   # ARM 32-bit (Pi 1/2, embedded)
make cross CROSS=riscv64-linux-gnu TLS=1     # RISC-V 64 (VisionFive, QEMU, HiFive)
make cross CROSS=i686-linux-gnu TLS=1        # x86 32-bit
```

For a fully static single-file binary (musl; pair with static OpenSSL or `TLS=0`):

```sh
make musl       # musl-gcc -> ./clawc-static
```

## Running as a service

`deploy/clawc.service` is a systemd unit with a crash-restart watchdog. Install:

```sh
sudo cp clawc /usr/local/bin/
sudo mkdir -p /etc/clawc && sudo cp deploy/clawc.service /etc/systemd/system/
sudo $EDITOR /etc/clawc/config     # or: CLAWC_CONFIG=/etc/clawc/config clawc setup
sudo systemctl daemon-reload && sudo systemctl enable --now clawc
journalctl -u clawc -f             # logs
```

The unit runs `clawc daemon` under `CLAWC_CONFIG=/etc/clawc/config`, so only the
gateways you configured come up (telegram if `telegram_token` is set, serve if
`serve_port` is nonzero, heartbeat if `heartbeat_interval` is nonzero).
