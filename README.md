# clawc

**A claw-style autonomous personal agent in C99.**

`clawc` is a local-first AI agent in the spirit of OpenClaw: it runs on your
machine, keeps memory in plain Markdown, **remembers every conversation across
restarts**, streams replies live, asks before doing anything dangerous, calls
tools (shell, files, skills, web), wakes itself on a timer (heartbeat), speaks
**HTTPS with built-in TLS**, and lives in **Telegram** as a first-class channel.
Compiled binary: **98,312 bytes** (Telegram budget build) — under the 100 KB
budget even with TLS, streaming, and the multi-gateway daemon. Runs on x86, ARM,
and RISC-V.

```
$ ls -la build/native/
-rwxr-xr-x 1 user user 126984 clawc          # all channels
-rwxr-xr-x 1 user user  98312 clawc-budget   # make CHANNELS=telegram — under 100 KB
$ ldd clawc
    libssl.so.3 => ...    # only libc + OpenSSL (TLS=0 build needs just libc)
    libc.so.6 => ...
```

## Why

OpenClaw proved the "personal agent that actually does things" pattern — but it
is a ~300k-line Node.js/TypeScript platform. `clawc` distills the *pattern* into
something you can read in an afternoon, cross-compile for a $15 board, audit
line-by-line, and run anywhere a C compiler exists.

## Feature map vs OpenClaw

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

## Size & startup

Budget build is 98,312 B (`CHANNELS=telegram`); all-channels ~126,984 B;
≈150 KB static musl. Peak RSS ~4 KB static / 2.8 MB dynamic. Startup is ~4 ms
(budget), almost entirely OpenSSL's one-time constructor init — the `make
TLS=0` build starts at 1.8 ms. Runtime deps: libc + OpenSSL (TLS=0 build: libc
only).

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

```sh
make            # -> build/native/clawc (TLS auto-detected)
make TLS=0      # no OpenSSL: https:// goes through the curl subprocess
make size       # show binary size
make test       # end-to-end checks (mock LLM + Telegram + Matrix + Discord)
```

For cross-compilation (x86/ARM/RISC-V), building only the channels you use, and
running as a systemd service, see **[docs/installation.md](docs/installation.md)**.

## Quick start

```sh
make
./build/native/clawc setup          # interactive config wizard
./build/native/clawc chat           # talk to your agent
./build/native/clawc run "read /etc/os-release and tell me the distro in one line"
./build/native/clawc daemon         # all configured gateways (telegram/serve/heartbeat)
```

For the wizard, the configuration file & environment, the daemon, the workspace
layout, Telegram setup, and local models, see **[docs/first-steps.md](docs/first-steps.md)** and
**[docs/setup.md](docs/setup.md)**.

## Commands

| command | what it does |
|---|---|
| `init` | create workspace: `SOUL.md`, `MEMORY.md`, `HEARTBEAT.md`, example skill |
| `run <msg>` | one-shot prompt, prints reply, exits (scriptable; reads stdin if no args) |
| `chat` | interactive REPL (`/reset`, `/quit`) |
| `telegram` | Telegram bot: `-token T` `-allow id,id` `-poll secs` `--once` |
| `heartbeat` | one proactive cycle from `HEARTBEAT.md`; replies HEARTBEAT_OK or acts/notifies |
| `daemon` | supervisor: run every **configured** gateway and restart crashes |
| `serve [-p PORT]` | line-based TCP bridge |
| `config` | print effective settings and the daemon gateway plan |
| `fetch <url>` | GET a URL with the built-in HTTP(S) client |

Flags: `-c config_file` `-w workspace` `-m model` `-u api_url` `-k api_key`
`-a openai|anthropic` `-s max_steps` `-t timeout` `-x sandbox_dir` `-q quiet`
`--yes` (auto-approve risky tools) `--insecure`.

## Channels & features

A short tour of what the agent can do.

**Channels & feedback.** Telegram is first-class (long polling, per-chat
sessions). The `serve` channel is a JSON TCP bridge (TLS by default); Matrix and
Discord are additional gateways with the same confirm/deny design. Replies
stream token by token, show typing/status placeholders, and use inline-button
confirmations (`[✅ Approve] [❌ Cancel]`).

**Safety.** Risky tools (`rm -rf`, writing to `/etc`, `curl | sh`, …) pause for
approval — `y/N` on the CLI, inline buttons in chat. `heartbeat` refuses risky
tools outright, and sender allowlists are mandatory. See [Security](#security).

**Autonomy.** Heartbeat on a schedule, cron jobs (`clawc cron`), parallel
`subagent` tasks, and autonomous mode (`GOALS.md` + `/goal`) with unattended
safety. **Self-improvement:** `write_skill`, `reflect` (lessons), and
`clawc improve`. **Persistence:** conversations are append-only JSONL logs;
`search_memory` also searches LESSONS.md, GOALS.md, and session history.
`clawc doctor` checks the whole installation in one shot. HTTPS is used
end-to-end (LLM API, Telegram, `fetch_url`) via the in-process TLS client.

## Persistence & audit

Each session is an append-only **JSONL** log of every message — including tool
calls and results at full fidelity:

```json
{"role":"user","content":"summarize my inbox"}
{"role":"assistant","content":"","tool_calls":[{"id":"call_1","name":"run_shell","args":{"command":"ls ~/inbox"}}]}
{"role":"tool","content":"[exit 0]\nmail1.txt","tool_call_id":"call_1"}
{"role":"assistant","content":"You have one mail: mail1.txt"}
```

Turn-aware trimming drops whole oldest turns (never splitting a tool call from
its result); corrupt logs are sanitized on load; `/reset` clears both memory and
the session file; `history/audit.jsonl` records every tool execution.

## Security

**Remote-access warning:** the `serve`/A2A bridge auto-approves tools and has
no per-user authorization — keep the shared bearer token and private key secret
and expose it only to trusted clients. **Sender allowlists are mandatory:**
configure comma-separated exact IDs (`telegram_users`, `matrix_users`,
`discord_users`) or the matching `CLAWC_*_USERS` env vars; empty or missing
lists deny all input and the gateway refuses to start. IDs must be the
platform's stable sender IDs, not usernames.

An agent with `run_shell` and a Telegram inbox is powerful — treat it that way:
use an explicit Telegram allowlist, verify certificates by default
(`--insecure` is opt-in), confine file tools with `-x DIR`, start heartbeat
actions with a read-only checklist, and keep secrets out of the JSON config.
Prompt injection from web content (`fetch_url`) or forwarded messages is a real
threat — least-privilege the workspace and allowlist chats.

## Testing

`make test` runs an end-to-end suite (a mock LLM and mock Telegram Bot API over
HTTP and HTTPS): tool round-trips on both API dialects, memory, sandbox
rejection, `fetch_url`, heartbeat suppression, TLS round-trip, the Telegram
loop, confirmation via inline buttons, `--yes` auto-approval, history
persistence/restoration/reset, corrupt-log sanitization, per-chat isolation, the
audit log, the serve bridge, config precedence, the daemon supervisor, and a
binary size assertion. `test/asan_check.sh` runs the hot paths under
AddressSanitizer/UBSan.

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

## Docs

- [docs/installation.md](docs/installation.md) — building, cross-compilation, channels, service
- [docs/setup.md](docs/setup.md) — setup wizard, configuration file & env, daemon, workspace
- [docs/first-steps.md](docs/first-steps.md) — quick start, talking to your agent, local model

## License

MIT — see `LICENSE`.
