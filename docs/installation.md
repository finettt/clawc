# Installation

Building, installing, cross-compiling, and running `clawc` as a service.

## Building

Any C99 compiler + POSIX libc (Linux, macOS, BSD). If OpenSSL headers are
present, built-in TLS is enabled automatically (`TLS=1`):

```sh
make            # -> build/native/clawc (TLS auto-detected)
make TOOLCHAIN=musl # fully static ~150KB, 4KB peak RSS (needs musl-gcc)
make TLS=0      # no OpenSSL: https:// goes through the curl subprocess
make size       # show binary size
make test       # end-to-end checks (mock LLM + Telegram + Matrix + Discord)
bash test/asan_check.sh   # ASan/UBSan smoke checks; run make test first (TLS fixtures)
make BUILD_TYPE=debug # unoptimized debug build -> build/native-debug/clawc
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
make CROSS=aarch64-linux-gnu TLS=1   # ARM 64-bit (Pi 3/4/5, Graviton) -> build/aarch64-linux-gnu/clawc
make CROSS=arm-linux-gnueabihf TLS=1 # ARM 32-bit (Pi 1/2, embedded) -> build/arm-linux-gnueabihf/clawc
make CROSS=riscv64-linux-gnu TLS=1   # RISC-V 64 (VisionFive, QEMU, HiFive) -> build/riscv64-linux-gnu/clawc
make CROSS=i686-linux-gnu TLS=1      # x86 32-bit -> build/i686-linux-gnu/clawc
```

A `CROSS=<triple>` build expects a `<triple>-gcc` toolchain. Any other cross
compiler (for example clang with `-target`) goes in `CC` for the default
target; the OS-specific linker flags follow the compiler's target triple, not
the build host.

For a fully static single-file binary (musl; pair with static OpenSSL or `TLS=0`):

```sh
make TOOLCHAIN=musl # musl-gcc -> build/native-musl/clawc (static)
```

### iOS (jailbroken, arm64)

On a jailbroken device `clawc` runs as an ordinary command-line tool. Build it
with a compiler that targets iOS (the version in `-target` is the oldest iOS
the binary has to run on), then fake-sign it, since a jailbreak still only runs
signed binaries:

First an SDK: jailbreak repositories ship `clang`/`ld64`/`llvm` but **no** SDK, and
iOS has no `/usr/include`, so `clang` cannot even find `stdio.h` without one. A
Theos SDK cache (`/var/theos/sdks/iPhoneOS*.sdk`) or any extracted `iPhoneOS.sdk`
works — pass it with `-isysroot`.

`HOSTCC` must be the **same** iOS-targeted driver: the build-time tools
(`tools/textpack.c`) run on the machine that builds, which on a device is the
device itself. Leaving `HOSTCC` at the default `cc` fails at
`src/packed_text_data.h` with `'stdio.h' file not found`.

```sh
export SDK=/var/theos/sdks/iPhoneOS12.4.sdk     # or /usr/share/SDKs/iPhoneOS.sdk
export CC_IOS="clang -isysroot $SDK -target arm64-apple-ios12.0"

make HOSTCC="$CC_IOS" CC="$CC_IOS" TLS=0 STRIP=llvm-strip
ldid -Sentitlements.xml build/native/clawc   # platform-application, com.apple.private.security.no-container
```

`-fuse-ld=lld` is a *cross*-build flag; on-device, Procursus clang linking with
`ld64` is correct. On a rootless jailbreak every path above gains a `/var/jb`
prefix.

- `TLS=0` needs `curl` on the device. For built-in TLS, OpenSSL from the
  bootstrap lives under `/usr/include` and `/usr/lib`, which `-isysroot` hides:
  pass `-idirafter /usr/include` (not `-I`, which would outrank the SDK) and, in
  `LDLIBS`, the library paths — `ld64` prefixes `-isysroot` to every `-L`, so
  `-lssl` is not found and `LDLIBS` from the command line replaces the
  Makefile's `-lssl -lcrypto`:

  ```sh
  make HOSTCC="$CC_IOS -idirafter /usr/include" CC="$CC_IOS -idirafter /usr/include" \
       TLS=1 LDLIBS="/usr/lib/libssl.dylib /usr/lib/libcrypto.dylib"
  ```
- iOS ships no PEM trust store: point `SSL_CERT_FILE` at a CA bundle (for
  example curl's `cacert.pem`), otherwise `https://` fails certificate
  verification.
- iOS has no OS sandbox backend: `"sandbox": "os"` runs the announced
  best-effort fallback (see [setup.md](setup.md)). The jail shim and `jaild`
  are Linux extras, so the two `[make] ... skipped` lines are expected.

## Proxy

Every outbound connection (LLM, Telegram, Matrix, Discord REST + gateway,
`fetch`) goes through one setting:

```json
{ "proxy": "socks5h://127.0.0.1:1080", "proxy_no": "127.0.0.1,localhost,.corp" }
```

Accepted forms: `http://`, `https://` (not implemented yet, rejected with an
explicit error), `socks5://`, `socks5h://`. Credentials go inline
(`http://user:pass@host:port`). `socks5h://` resolves DNS at the proxy (no leak);
`socks5://` uses the domain form too, so the local resolver is never consulted.

Resolution order (later wins, like every other key):

```
1. proxy key in the config file
2. CLAWC_PROXY
3. HTTPS_PROXY / https_proxy
4. ALL_PROXY   / all_proxy
```

`proxy_no` / `NO_PROXY` is a comma-separated bypass list of exact hosts,
`.suffix`es, bare suffixes and `host:port` entries.

TLS is unaffected: the tunnel carries the connection, while SNI and certificate
verification still target the origin host, so a proxy can never weaken
verification. `clawc doctor` gains a `[net]` line that opens a real CONNECT
through the proxy rather than only printing the setting.

**Build without TLS (`TLS=0`)**: https goes through the `curl` subprocess, which
honours `ALL_PROXY`/`HTTPS_PROXY` itself. The env variables therefore work in
that build even though `clawc` does not read them — do not be surprised that
`TLS=1` behaves differently. `clawc` does not pass `--proxy` to curl, so curl's
own rules (including `NO_PROXY`) apply.

## Running as a service

`deploy/clawc.service` is a systemd unit with a crash-restart watchdog. Install:

```sh
sudo cp build/native/clawc /usr/local/bin/
sudo mkdir -p /etc/clawc && sudo cp deploy/clawc.service /etc/systemd/system/
sudo $EDITOR /etc/clawc/config     # or: CLAWC_CONFIG=/etc/clawc/config clawc setup
sudo systemctl daemon-reload && sudo systemctl enable --now clawc
journalctl -u clawc -f             # logs
```

The unit runs `clawc daemon` under `CLAWC_CONFIG=/etc/clawc/config`, so only the
gateways you configured come up (telegram if `telegram_token` is set, serve if
`serve_port` is nonzero, heartbeat if `heartbeat_interval` is nonzero).
