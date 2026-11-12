# Setup & configuration

Configuring `clawc`: the setup wizard, the configuration file and environment,
the daemon, and the workspace layout.

## The setup wizard

`./build/native/clawc setup` launches an interactive wizard that asks for the workspace,
API base/model/key, Telegram token (verified live via `getMe`), allowed chat
IDs, required Telegram user IDs, serve port, heartbeat interval, and
auto-approve. It writes `~/.clawc/config` (mode 600, parent directory created
as needed).

- The API base must end with `/v1` — a bare `host:port` gets `/v1` appended
  automatically.
- It is fully scriptable: pipe answers to it. `clawc setup FILE` writes
  elsewhere.

## Configuration (file + environment)

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

## Workspace layout

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
