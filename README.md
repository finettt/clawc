# clawc

**A claw-style autonomous personal agent in C99.**

`clawc` is a local-first AI agent in the spirit of OpenClaw (Clawdbot/Moltbot): it
runs on your machine, keeps memory in plain Markdown files, **remembers every
conversation across restarts**, streams replies live, asks before doing anything
dangerous, calls tools (shell, files, skills, web), wakes itself on a timer
(heartbeat), speaks **HTTPS with built-in TLS**, and lives in **Telegram** as a
first-class channel. Compiled binary: **98,312 bytes** (Telegram budget build;
126,984 with all channels) — under the 100 KB budget
even with TLS, streaming, and the multi-gateway daemon. Runs on x86, ARM, and RISC-V
(see [PORTABILITY.md](PORTABILITY.md) for the full audit of what is
Linux-only vs portable).

```
$ ls -la clawc /tmp/clawc-budget
-rwxr-xr-x 1 user user 126984 clawc          # all channels (telegram+matrix+discord+serve)
-rwxr-xr-x 1 user user  98312 clawc-budget   # make CHANNELS=telegram — under 100 KB
$ ldd clawc
    libssl.so.3 => ...      # only libc + OpenSSL (TLS=0 build needs just libc)
    libcrypto.so.3 => ...
    libc.so.6 => ...
```

## Why

OpenClaw proved the "personal agent that actually does things" pattern — but it's
a ~300k-line Node.js/TypeScript platform. `clawc` distills the *pattern* into
something you can read in an afternoon, cross-compile for a $15 board, audit
line-by-line, and run anywhere a C compiler exists.

## Feature map vs OpenClaw

Full evidence-based table in [COMPARISON.md](COMPARISON.md). The short version:

| | **clawc** | **OpenClaw** |
|---|---|---|
| Stack | C99 + POSIX, one ~96–124 KB binary | TypeScript/Node.js platform, ≈200 MB+ |
| Codebase | 8,971 lines, auditable in an afternoon | ≈ 300–500k+ lines, 70+ deps |
| Channels | Telegram + TCP bridge + CLI | 20+ (WhatsApp, Slack, Discord, Signal, …) |
| Safety | inline ✅/❌ confirm on risky actions, audit log, allowlist | host access by default, optional Docker sandbox |
| Runs on | RISC-V / ARM / x86 boards ($15 class) | desktops & servers |
| Shared | markdown memory · skills · heartbeat autonomy · OpenAI+Anthropic backends · MIT | |

OpenClaw is the ecosystem play (voice, canvas, mobile nodes, ClawHub registry);
clawc is the opposite trade — the same personal-agent loop distilled into code
you can read line-by-line and cross-compile anywhere.

## Size & startup (measured)

Historical measurements on one x86-64 host; startup and RSS are not remeasured for the hardened build. Startup = process
spawn → exit for the `version`/`--version` flag, median of 21 runs. NullClaw
built from source (`zig build -Doptimize=ReleaseSmall`, rev `5f1cade`, its
README's 678 KB claim does not reproduce). OpenClaw rows are research figures,
not measured locally — even an empty Node script takes ~41 ms to start on this
host, before any application code loads.

Startup footnote: the full build's ~4 ms is almost entirely **OpenSSL's
one-time constructor init** (~2 ms, measured against a hello-world that merely
calls libcrypto), not clawc's code (0.05 ms) — a `make TLS=0` build starts at
1.8 ms, at spawn-floor speed and faster than NullClaw. A daemon pays this once
per process, then runs for days.

| | **clawc 0.3.0** | **NullClaw** | **OpenClaw** |
|---|---|---|---|
| Binary size | **98,312 B** budget (`CHANNELS=telegram`) · 126,984 B all-channels · ≈150 KB static musl | 5,505,104 B | no single binary; ≈200 MB+ install (Node + npm tree) |
| Peak RSS | **4 KB** static / 2.8 MB dynamic (`/proc` VmHWM) | 1.1 MB (`/proc` VmHWM) | hundreds of MB (research) |
| Startup | **4.1 ms** budget · 4.8 ms all-channels | 2.3 ms | seconds to boot (research) |
| Source | 8,971 lines C99 | 289,734 lines Zig | ≈300–500k lines TypeScript |
| Runtime deps | libc + OpenSSL (TLS=0 build: libc only) | libc (+SQLite) | Node ≥22 + 70+ npm deps |

Full feature-level comparison and methodology notes: [COMPARISON.md](COMPARISON.md). See [CHANGELOG.md](CHANGELOG.md) for the chronological feature and fix history.
For scale: NullClaw's README logo alone (`nullclaw.png`, 1,332,664 B) is 11×
the entire clawc binary.

## Architecture

Three layers, same shape as the big claw agents (channel / brain / body):

```
   channel:   telegram bot | chat REPL | daemon supervisor | TCP serve bridge
      |            |            |            |
   brain:     agent.c  -- ReAct loop: prompt -> LLM -> tool_calls -> tools -> ...
      |            |            |            |
   body:      tools.c (run_shell, read_file, write_file, list_dir,
                       remember, search_memory, load_skill, fetch_url)
      |
   llm.c      -- normalized Msg format, adapters: OpenAI-compatible / Anthropic
   history.c  -- append-only JSONL session logs + audit log
   tls.c      -- in-process TLS 1.2+ client (OpenSSL), cert + hostname verify
   http.c     -- HTTP(S) transport: raw sockets + TLS, curl(1) fallback (TLS=0)
   json.c     -- minimal DOM parser + writer (no dependencies)
   util.c     -- buffers, files, process runner with timeout
```

The agent keeps conversation history in memory (trimmed per chat), re-anchors a
fresh system prompt each turn (identity + live `MEMORY.md` + skill list + current
time), and loops until the model stops emitting tool calls or `-s max_steps` is hit.

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

**Release 0.3.0** — everything since 0.2.0: Matrix + Discord (buttons, slash
commands, media/vision) channels and A2A between instances; cron with
wall-clock schedules and parallel subagents; LLM-summarized compaction;
autonomous mode with `GOALS.md` + `/goal`; self-improvement (skills, lessons,
auto-learn); OS sandbox (Landlock on Linux, sandbox_init on macOS, honest
path-check fallback elsewhere) and `clawc doctor`; `CHANNELS=` compile-what-
you-use builds (Telegram budget build 98,312 B) and static musl binaries.
End-to-end regression checks.

Earlier: **0.2.0** — `clawc setup` wizard with live bot-token verification,
persistent Telegram update offsets (restarts never replay messages), and a
systemd unit (`deploy/clawc.service`).

## Quick start

```sh
make                               # or: make TLS=0 for a libc-only build
./clawc setup                      # interactive config wizard (see below)
./clawc config                     # review what will run
./clawc chat                       # talk to your agent
./clawc run "read /etc/os-release and tell me the distro in one line"
./clawc daemon                     # all configured gateways (telegram/serve/heartbeat)
```

`clawc setup` asks workspace, API base/model/key, Telegram token (verified live
via `getMe`), allowed chat IDs, required Telegram user IDs, serve port, heartbeat interval, and auto-approve;
writes `~/.clawc/config` (mode 600, parent directory created as needed). The API
base must end with `/v1` — a bare `host:port` gets `/v1` appended automatically.
It is fully scriptable: pipe answers to it. `clawc setup FILE` writes elsewhere.

### Running as a service

`deploy/clawc.service` is a systemd unit with a crash-restart watchdog. Install:

```
sudo cp clawc /usr/local/bin/
sudo mkdir -p /etc/clawc && sudo cp deploy/clawc.service /etc/systemd/system/
sudo $EDITOR /etc/clawc/config     # or: CLAWC_CONFIG=/etc/clawc/config clawc setup
sudo systemctl daemon-reload && sudo systemctl enable --now clawc
journalctl -u clawc -f             # logs
```

The unit runs `clawc daemon` under `CLAWC_CONFIG=/etc/clawc/config`, so only the
gateways you configured come up (telegram if `telegram_token` is set, serve if
`serve_port` is nonzero, heartbeat if `heartbeat_interval` is nonzero).

## Telegram bot (built-in channel)

1. Create a bot with [@BotFather](https://t.me/BotFather), copy the token.
2. Run:

```sh
export CLAWC_TELEGRAM_TOKEN="123456:ABC-DEF..."
export CLAWC_TELEGRAM_USERS="123456789"       # Telegram numeric user ID, NOT bot username
./clawc telegram -allow <your_chat_id>        # optional additional chat restriction
```

Text your bot on Telegram; it answers with full tool access and remembers each
chat's conversation. Commands: `/reset` (clear history), `/help`. With no
chat allowlist, private chats and mention-gated group messages from authorized
users may be accepted. With `-allow`, the chat must also be listed. Replies longer than Telegram's 4096
char limit are chunked at line boundaries. Network failures back off
exponentially; HTTP 429 honors `retry_after`.

### Matrix channel

`matrix_homeserver` + `matrix_token` + required `matrix_users`
(comma-separated full Matrix IDs, e.g. `@alice:example.org`) (+ optional
`matrix_user`, `matrix_rooms` allowlist) and the daemon runs a Matrix gateway — same design
as Telegram: HTTPS long-poll (`/sync`), persisted `next_batch` cursor
(at-most-once), per-room sessions, `/reset` `/help`. For confirmation, the requester must
**reply to the specific prompt** with `/yes` or `/no`.
Text rooms only; encrypted rooms are out of scope. Get a token from any
Matrix client (Element: Settings -> Help -> Access token).

Size note: channels are pay-for-what-you-use — `make CHANNELS=telegram`
holds the <100 KB budget build (99 KB); the all-channels build (telegram +
matrix + discord + serve + a2a) is ~124 KB, ceiling-tested at 128 KiB. Builds
use `-Oz` (GCC 12+/clang; pass `CFLAGS="-Os ..."` on older compilers).

### A2A — clawc instances working together

Set `a2a_peers` to configured aliases such as `"buddy=agent.example:8848"`.
The `a2a` tool contacts **configured aliases only**, so model-supplied
arbitrary addresses cannot receive the credential. A2A uses TLS by default
and verifies the peer certificate chain **and** the configured hostname (or IP
address). Set `a2a_ca` / `CLAWC_A2A_CA` to a PEM CA file for a private CA, or
omit it for the system trust store. An explicit CA file replaces, rather than
extends, system trust for A2A. The general `--insecure` option does not disable
bridge certificate verification. Both peers need the same `bridge_token`.
TLS failures never fall back to plaintext.

The token is a shared bearer secret: any holder can impersonate any `from`
identity, and the receiving bridge auto-approves tools. Compiled with the
`serve` channel.

### Discord channel

`discord_token` + required `discord_users` (comma-separated numeric user IDs;
not display names) (+ optional `discord_guilds`/`discord_channels` allowlists)
runs a Discord bot via the gateway WebSocket (`ws.c`, pure C) with REST
replies — text, `/reset` `/help`, per-channel sessions, at-most-once cursor
via gateway seq + resume. **v2: inline buttons on confirmations** (edit-in-
place approvals like Telegram, stale-press guard) and **slash commands**
(`/claw <task>`, `/goal [task]`; `CLAWC_DISCORD_REGISTER=1` registers them).
`/yes` `/no` text stays as fallback. **Media:** message attachments are saved
to `workspace/media/` (binary-safe); images are sent to the model as vision
input, exactly like Telegram photos.

### `/goal` command

Manage goals straight from chat — no LLM round-trip, instant replies:

```
/goal                list goals (GOALS.md)
/goal add <task>     add a goal  (also: /goal done <id>, /goal rm <id>)
```

Works in Telegram (mention-safe in groups) and the `chat` REPL. The same
`goal_add`/`goal_done` tools let the agent manage goals itself.

### Autonomous mode

`GOALS.md` is the agent's own task list (`goal_add`/`goal_done`). `clawc
autonomous` runs one work cycle - advance the most important open goal with
real tool action, mark it done, report progress. Set `autonomous_interval` and
the daemon runs cycles on schedule, delivering reports to Telegram/notify like
heartbeat. Unattended safety applies: risky tools are refused.

### Self-improvement

The agent can improve itself across sessions:

- **`write_skill`** — saves a reusable procedure as `skills/<name>/SKILL.md`
  (name sanitized, 8KB cap); future sessions see it in the skill list and can
  `load_skill` it.
- **`reflect`** — records a durable lesson in `LESSONS.md`, which is injected
  into every future prompt (capped at 8KB; the agent consolidates when full).
- **`clawc improve`** — runs a self-improvement review turn: reflect on what to
  do better, record sharp lessons, save procedures as skills.

The system prompt tells the agent these exist and when to use them — learning
is part of the job, not an add-on.

Three mechanisms follow Hermes Agent's learning loop (Nous Research):

- **auto-learn** — after a complex task (5+ tool calls), a quiet learning pass
  extracts a skill via `write_skill` or a lesson via `reflect` while the
  experience is fresh. `"auto_learn": false` disables it.
- **skill refinement nudge** — `load_skill` results remind the agent to update
  the skill with `write_skill` when it finds a better approach mid-use.
- **session recall** — `search_memory` now searches LESSONS.md, GOALS.md and
  all session history (`history/*.jsonl`), not just MEMORY.md.

### Media, groups & model resilience

Telegram photos/voice notes/documents are saved to `workspace/media/` and
photos flow to the model as **vision input** (base64, both API dialects) with
their caption. Group chats use **mention gating**: the bot only responds when
@mentioned or replied to. The agent retries one transient LLM failure
(connect/5xx/429), asks the model to re-issue malformed tool calls. Long conversations are
**compacted**: when history outgrows `context_chars`, older turns are
summarized by the model into a `[compacted earlier conversation]` message
(persisted to the session) instead of being dropped — the model keeps the
thread. If summarization itself fails, old turns fall back to dropping with a
note.

### Doctor

`clawc doctor` checks the whole installation in one shot: config + workspace,
a real 1-token LLM round trip (with latency), Telegram `getMe`, landlock
availability, cron schedule state, and the daemon gateway plan — `ok`/`FAIL`
per component, non-zero exit on problems.

### Cron & subagents

`clawc cron` keeps scheduled jobs in `$WORKSPACE/cron.json`:

```sh
clawc cron add "check the deploy and message me if it failed" --in 1800
clawc cron add "standup summary" --daily 09:00          # wall clock, no epochs
clawc cron add "trash out" --weekly thu 20:00           # every Thursday 20:00
clawc cron add "call mom" --at 19:30                    # one-shot tonight
clawc cron list ; clawc cron rm j2
clawc cron --run-due          # fire what's due now (wire into system cron)
```

Time specs are local wall clock (`HH:MM`, or `DAY HH:MM` with `--weekly`); raw
epochs still work (`--at 1762000000`). Recurring jobs keep their anchor — a
daily 09:00 job fires at 09:00 every day, not N seconds after each run.

One-shots (`--at`/`--in`) fire once; `--every N` re-arms N seconds after each
run. The `daemon` picks up any enabled jobs as a **cron gateway** (checked every
15 s). Jobs run as unattended prompts — like `heartbeat`, risky tools are
refused — and their reports go to the Telegram allowlist and/or the `notify`
command.

The `subagent` tool runs focused nested agents (isolated context, 6 steps,
read-only tools by default — pass `"tools": [...]` to widen) and returns just
their final reports, keeping tool chatter out of your context. Pass
`"tasks": [...]` to run up to 4 **in parallel** (one thread each). Sub-agents
cannot nest.

### Update offsets (no replays)

The bot persists its `getUpdates` offset per token at
`$WORKSPACE/history/tg-offset-<hash>` and updates it the moment a batch is
pulled — **at-most-once** delivery: a restart or crash never re-processes a
message (and therefore never double-executes one). If the process dies while a
reply is still being produced, that one message is dropped rather than replayed.

### HTTPS everywhere

- The **LLM API**, **Telegram API**, and the **`fetch_url` tool** all use the
  in-process TLS client (TLS 1.2+, certificate *and* hostname verification,
  system trust store) — no subprocess, no curl required.
- `--insecure` disables verification for self-signed setups (e.g. a local
  Telegram Bot API server or an internal LLM gateway).
- A `TLS=0` build keeps the curl(1) fallback for https:// instead.

```sh
./clawc fetch https://api.telegram.org        # debug: GET with built-in client
./clawc run "fetch https://example.com and summarize it"   # fetch_url tool
```

### Local model (zero cost, no TLS needed)

```sh
./clawc -u http://127.0.0.1:11434/v1 -m qwen2.5:32b chat     # Ollama / llama.cpp
./clawc -a anthropic -u https://api.anthropic.com/v1 -m claude-sonnet-4-5 chat
```

## Commands

| command | what it does |
|---|---|
| `init` | create workspace: `SOUL.md`, `MEMORY.md`, `HEARTBEAT.md`, example skill |
| `run <msg>` | one-shot prompt, prints reply, exits (scriptable; reads stdin if no args) |
| `chat` | interactive REPL (`/reset`, `/quit`) |
| `telegram` | Telegram bot: `-token T` `-allow id,id` `-poll secs` `--once` |
| `heartbeat` | one proactive cycle from `HEARTBEAT.md`; replies HEARTBEAT_OK or acts/notifies |
| `daemon` | supervisor: run every **configured** gateway (below) and restart crashes |
| `serve [-p PORT]` | line-based TCP bridge (see below) |
| `config` | print effective settings (file/env/flags) and the daemon gateway plan |
| `fetch <url>` | GET a URL with the built-in HTTP(S) client |

Flags: `-c config_file` `-w workspace` `-m model` `-u api_url` `-k api_key`
`-a openai|anthropic` `-s max_steps` `-t timeout` `-x sandbox_dir` `-q quiet`
`--yes` (auto-approve risky tools) `--insecure`.

### Live feedback & trust

The agent feels alive instead of silent:

- **Streaming**: replies arrive token by token (SSE, OpenAI dialect) — live in
  the CLI; in Telegram the reply message grows as edits to a single message.
- **Status line + typing**: `⏳ thinking…` / `⏳ tool: run_shell…` placeholders and
  Telegram `typing` actions while tools execute; when the reply starts, the
  **same message is edited into it** — one message from thinking to answer.
- **Confirmations**: potentially destructive tools (`rm -rf`, `mkfs`, `dd`,
  writing to `/etc`, `curl | sh`, ...) pause for approval — `y/N` on the CLI,
  and in Telegram **inline buttons** on the confirmation message:
  `[✅ Approve] [❌ Cancel]` (callback queries, acked and stripped once decided;
  `/yes` `/no` text still works as a fallback). Telegram and Discord presses
  must match both the requester and the current prompt; text approvals must
  come from the requester after the prompt. Matrix text approvals must reply
  to the prompt event. Stale or other-user presses do not decide. 2-minute window (`CLAWC_CONFIRM_SECS` overrides);
  messages from other chats keep queuing normally.
  Policy: `chat`/`telegram` ask; `run`/`serve` auto-approve (explicit action,
  audit-logged); `heartbeat` **refuses** risky tools outright. `--yes` disables
  asking everywhere.
- **Commands**: `/start` `/help` (capabilities), `/status` (config + session),
  `/memory`, `/model`, `/reset` — in Telegram and the chat REPL.

### Bridge protocol (`serve`)

**TLS is the default for serve and A2A.** A listener requires an OpenSSL-enabled
build, a PEM certificate chain and matching private key. Set JSON keys
`serve_cert` and `serve_key`, or the environment variables `CLAWC_SERVE_CERT`
and `CLAWC_SERVE_KEY`. Keep the private key readable only by the service
account. The listener defaults to `127.0.0.1`; use `serve_bind` or `-b` to
explicitly bind another address.

For a local test, create a self-signed certificate whose SAN matches
`localhost` and `127.0.0.1` (use a real CA-issued certificate for production):

```sh
umask 077
openssl req -x509 -newkey rsa:2048 -nodes -days 30 \
  -keyout bridge-key.pem -out bridge-cert.pem -subj /CN=localhost \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'
export CLAWC_BRIDGE_TOKEN="$(openssl rand -hex 32)"
export CLAWC_SERVE_CERT="$PWD/bridge-cert.pem"
export CLAWC_SERVE_KEY="$PWD/bridge-key.pem"
./clawc serve -p 8848
```

For an A2A client connecting to that local test server, use the same token and
point it at the test certificate and configured alias:

```sh
export CLAWC_A2A_CA="$PWD/bridge-cert.pem"
export CLAWC_A2A_PEERS="buddy=localhost:8848"
```

On other machines, the certificate must contain the actual configured peer
hostname or IP in its SAN; distribute the CA/certificate securely. Never
distribute the server private key. `serve_tls` and `a2a_tls` default to
`true`.

A fresh 64-hex-character shared secret is required as `bridge_token` in JSON
or `CLAWC_BRIDGE_TOKEN` in the environment. Protect the config file (mode 600)
and environment; `clawc config` shows only whether the token is set. Use the
same token on both ends of an A2A link. Existing plaintext clients must be
updated: one JSON request and one response are exchanged over TLS:

```json
{"token":"<64-hex-character secret>","text":"what's in my memory?","from":"alice"}
```

An invalid or absent token is rejected before invoking the model. `from` is a
**client-supplied label, not an authenticated user identity**. The listener
processes one connection at a time and has no per-client rate limiter.

For a local-only legacy bridge or a trusted external encrypted tunnel,
explicitly set `serve_tls` to `false` and `a2a_tls` to `false` on the respective
ends (`CLAWC_SERVE_TLS=0` / `CLAWC_A2A_TLS=0`). Plaintext listeners and A2A
connections are restricted to loopback addresses; the token remains required.
A `TLS=0` build cannot provide bridge TLS and fails closed if TLS is requested.

### Heartbeat & notify

`heartbeat` feeds `HEARTBEAT.md` to the agent once; the `daemon` runs it on a
schedule (`heartbeat_interval`). If the model replies anything other than
`HEARTBEAT_OK`, the message is printed, delivered to the **Telegram allowlist
chats** when the daemon also runs the Telegram gateway, and piped to the
`notify` command (`CLAWC_NOTIFY`, e.g. `curl -s -d @- ntfy.sh/mytopic`).

### Configuration (file + environment)

Everything can live in one config file so the launch command is just
`clawc daemon`. Resolution order, highest wins:

```
CLI flags  >  environment  >  config file  >  built-in defaults
```

The file is **JSON**: one object mapping key names to values (strings, numbers,
`true`/`false`; keys starting with `_` are ignored notes). The old
`key = value` line format is **not** accepted. Looked up at `-c FILE`,
then `$CLAWC_CONFIG`, then `~/.clawc/config` (which `clawc setup` writes for
you). `clawc config` prints every effective value with its source — the first
thing to run when something acts weird.

```json
{
  "_note": "written by clawc setup",
  "workspace": "/home/user/.clawc/workspace",
  "api_url": "http://127.0.0.1:8900/v1",
  "model": "qwen2.5-1.5b",
  "api_key": "",

  "serve_port": 8848,
  "heartbeat_interval": 1800,
  "telegram_token": "123456:ABC-DEF...",
  "telegram_allow": "987654321",
  "telegram_users": "123456789",
  "auto_approve": false,
  "notify": "curl -s -d @- ntfy.sh/mytopic"
}
```

Key meanings: `api_url` = llama.cpp / Ollama / any OpenAI-compatible base
(always ends in `/v1`); `serve_port` enables the TCP bridge gateway
(**binds 127.0.0.1 only** — set `serve_bind` to `"0.0.0.0"` or pass `-b ADDR`
to expose it); `context_chars` = rough prompt budget (default 16384; older
turns are compacted into a summary, tool chains kept intact — 0 disables);
`"sandbox": "os"` (also `"landlock"`) prefers an available OS sandbox:
Landlock on Linux, seatbelt on macOS, or pledge/unveil on OpenBSD. When no OS
backend is available it uses a **best-effort fallback**, not a security
boundary: workspace path checks, a dynamically loaded libc interposer, and,
where available, a Linux seccomp-notification helper. The helper depends on
kernel seccomp and checks only selected syscalls. Its check-then-continue
policy is vulnerable to races; untrapped operations, static programs, direct
syscalls, and other bypasses can defeat the fallback. Do not run hostile or
untrusted commands relying on it. If the helper is unavailable, the weaker
shim-only mode is announced. For untrusted workloads use independently tested
OS isolation. A directory path in `sandbox`/`-x DIR` enables only file-tool
path checks.

Every key has an env twin: `CLAWC_WORKSPACE`, `CLAWC_API_URL`, `CLAWC_API_KEY`
(or `OPENAI_API_KEY` / `ANTHROPIC_API_KEY`), `CLAWC_MODEL`, `CLAWC_MAX_STEPS`,
`CLAWC_TIMEOUT`, `CLAWC_SANDBOX`, `CLAWC_INSECURE`, `CLAWC_AUTO_APPROVE`,
`CLAWC_NOTIFY`, `CLAWC_TELEGRAM_TOKEN`, `CLAWC_TELEGRAM_API`,
`CLAWC_TELEGRAM_ALLOW`, `CLAWC_TELEGRAM_POLL`, `CLAWC_SERVE_PORT`,
`CLAWC_HEARTBEAT_INTERVAL`, `CLAWC_CONFIRM_SECS` (confirmation window,
default 120s).

### Daemon mode (all gateways, one process tree)

`clawc daemon` reads the config (or environment) and starts one child process
per **configured** gateway — nothing configured, nothing runs (and it tells
you so):

- **telegram** — if a bot token is set (long polling, per-chat sessions)
- **serve** — if `serve_port` is set (TCP JSON bridge)
- **heartbeat** — if `heartbeat_interval` is set (feeds `HEARTBEAT.md`,
  forwards non-OK replies to the Telegram allowlist and `notify`)

The supervisor logs each gateway (`[daemon] gateway X: started (pid N)`),
restarts any that die — exponential backoff 2s..60s for crash loops — and
stops everything cleanly on SIGINT/SIGTERM (`[daemon] stopped`). Stop it with
SIGTERM rather than SIGKILL (a killed supervisor can't reap children).
Old style still works: `clawc daemon -i 1800` is a heartbeat-only daemon.

```sh
clawc config              # what will run, and where each setting came from
clawc daemon              # telegram + serve + heartbeat, per config
clawc daemon -p 8848      # ...plus the TCP bridge, even without a config file
```

### Workspace layout

```
~/.clawc/workspace/
├── SOUL.md        # identity + rules (like OpenClaw's SOUL.md)
├── MEMORY.md      # long-term memory; the remember tool appends here
├── HEARTBEAT.md   # proactive checklist
├── skills/
│   └── example/SKILL.md   # loadable with the load_skill tool
└── history/
    ├── chat.jsonl       # chat REPL session
    ├── srv-alice.jsonl  # serve bridge, one per "from"
    ├── tg-42.jsonl      # telegram chat 42
    └── audit.jsonl      # every tool call (ts, session, tool, args, result)
```

Everything is plain text: edit by hand or let the agent maintain it.
File tools (`read_file`, `write_file`, `list_dir`) resolve **relative paths
against the workspace**, so "list my files" works without knowing absolute
paths; with `-x DIR` everything is confined to the sandbox instead.

## Persistence & audit

Conversations survive restarts. Each session is an append-only **JSONL** log of
every message — including tool calls and results at full fidelity:

```json
{"role":"user","content":"summarize my inbox"}
{"role":"assistant","content":"","tool_calls":[{"id":"call_1","name":"run_shell","args":{"command":"ls ~/inbox"}}]}
{"role":"tool","content":"[exit 0]\nmail1.txt","tool_call_id":"call_1"}
{"role":"assistant","content":"You have one mail: mail1.txt"}
```

- `chat`, `serve` (per `from` identity), and `telegram` (per chat id) sessions
  are loaded on start and appended to as the conversation flows.
- **Turn-aware trimming** (`CLAWC_HISTORY_CHARS`, default 48000, and
  `CLAWC_HISTORY_MSGS`, default 60) drops whole oldest turns — never splitting
  a tool call from its result — and compacts the file to match memory.
- **Sanitization on load**: orphaned tool results and unanswered tool calls are
  dropped/stripped so a corrupt log can never break the OpenAI/Anthropic APIs.
- `/reset` in chat and Telegram clears both memory and the session file.
- `run` and `heartbeat` are intentionally ephemeral (scriptable one-shots);
  their tool calls still land in the audit log.
- **`history/audit.jsonl`** records every tool execution with timestamp,
  session name, tool, arguments, and result (truncated) — a tamper-evident-ish
  local trail of what your agent actually did.

## Security

**Remote-access warning:** The TCP `serve`/A2A bridge uses TLS by default and
requires a shared bearer token, but it auto-approves tools and has no per-user
authorization or rate limiter. Keep the token and private key secret and expose
the bridge only to trusted clients. Explicit plaintext mode is loopback-only.
**Breaking change: sender allowlists are mandatory.** Configure comma-separated
exact IDs as **JSON strings** (`telegram_users`, `matrix_users`, `discord_users`) or the
corresponding `CLAWC_TELEGRAM_USERS`, `CLAWC_MATRIX_USERS`,
`CLAWC_DISCORD_USERS` environment variables. Empty or missing user lists deny
all input; a configured gateway refuses to start without one. The daemon also
fails immediately rather than repeatedly restarting a misconfigured gateway.
IDs must be the platform's stable sender IDs, not usernames/display names:
Telegram numeric `from.id`, full Matrix `@user:server`, and numeric Discord
user ID. Gather IDs through your platform's trusted administrative tooling;
verify them independently before granting access. Existing chat/room/guild/
channel lists are **additional** restrictions, not substitutes for user IDs.

Buttons can be decided only by the original requester on the active prompt.
Telegram and Discord text confirmations must also be newer than the prompt;
Matrix confirmations must be replies referencing the prompt event. `--yes` or
`auto_approve` disables confirmations and should not be used when approval is
required. These controls trust the configured platform/server to authenticate
and report sender identities. Sessions and responses are shared per chat,
room or channel: allowlisted users and other participants may see shared
history, prompts or outputs, and allowed users can operate the agent. Use
private, trusted conversations; these checks do not provide confidentiality or
per-user data isolation.

An agent with `run_shell` and a Telegram inbox is powerful — treat it that way:

- **Telegram allowlist** (`-allow`): only listed chat ids can command the agent.
  Without it, private chats and mention-gated group traffic from listed users
  may be accepted. Use an explicit allowlist and trusted groups.
- **TLS**: certificates and hostnames are verified against the system trust
  store by default; `--insecure` is opt-in. No home-grown crypto — OpenSSL.
- **`-x DIR` sandbox** confines `read_file`/`write_file`/`list_dir`/`load_skill`
  to a directory (canonicalized via `realpath`, parent-checked for new files).
  `run_shell` is intentionally unrestricted; wrap clawc in a container/VM for
  hard isolation.
- **Heartbeat actions**: start with a read-only `HEARTBEAT.md` checklist and a
  notify command before granting anything destructive.
- **Secrets**: environment variables avoid storing credentials in the JSON config;
  `clawc setup` can write keys and tokens to a mode-0600 config file. Telegram
  uses bot tokens in API URLs. Protect config, logs, and process environments.
- Prompt injection from web content (`fetch_url`) or forwarded messages is a
  real threat — least-privilege the workspace and allowlist chats.

## Testing

The latest local `make test` run passed **277 checks**. The separate sanitizer
smoke check disables leak detection and is not exhaustive. The end-to-end suite
uses `test/mock_server.py`, which
serves a scripted OpenAI-compatible + Anthropic LLM **and** a mock Telegram Bot
API over both HTTP and HTTPS (self-signed): tool round-trips on both API
dialects, memory writes, sandbox rejection, `fetch_url`, heartbeat suppression,
TLS round-trip + certificate rejection, the Telegram loop (HTTP and TLS) with
the single-message reply UX (working message edited into the answer, no
checkmark), confirmation via **inline buttons** (approve + decline) and `/yes` text
fallback with `answerCallbackQuery` acks, heartbeat refusal of risky tools,
`--yes` auto-approval, `/status`, history persistence/restoration/reset,
corrupt-log sanitization, per-chat session isolation, the audit log, the serve
bridge, config precedence (file < env < flags, `clawc config`, bad-file
errors), the daemon supervisor (all gateways up, serve-through-daemon,
crash+restart, heartbeat→Telegram notify, clean shutdown), and a
binary size assertion. Every request is captured under `/tmp/clawc-test-reqs/`
for inspection. `test/asan_check.sh` runs the hot paths under
AddressSanitizer/UBSan. The mock enforces exact request paths and current-turn
tool results, so routing regressions fail loudly.

## Limitations (honest list)

- POSIX only (Linux on x86/ARM/RISC-V, macOS, BSD). No Win32 port (patches welcome).
- TLS builds link `libssl`/`libcrypto` dynamically (or use `TLS=0` + curl).
- Native tool-calling APIs only; models without tool support need a proxy.
- Telegram uses long polling (works behind NAT); webhook mode not implemented.
- Token streaming covers the OpenAI dialect; Anthropic replies arrive in one
  piece (callback fires once). `TLS=0` curl fallback is not incrementally live.
- No voice, no browser automation — `serve` and `fetch_url` are the extension points.
- `run`/`heartbeat` don't share history with chat sessions (by design; audit
  still records their tool calls). Two processes sharing one session file may
  interleave appends — don't run two daemons on the same workspace.

## License

MIT — see `LICENSE`.
