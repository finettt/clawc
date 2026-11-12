# First steps

Get your agent running and talk to it for the first time.

## 1. Build and configure

```sh
make                               # or: make TLS=0 for a libc-only build
./build/native/clawc setup                      # interactive config wizard (see setup.md)
```

`clawc setup` asks for your workspace, API base/model/key, a Telegram token
(verified live via `getMe`), allowed chat IDs, serve port, heartbeat interval,
and auto-approve. It writes `~/.clawc/config` (mode 600). The API base must end
with `/v1`; it is fully scriptable, and `clawc setup FILE` writes elsewhere.

## 2. Review and talk

```sh
./build/native/clawc config                     # review what will run, and where each setting came from
./build/native/clawc chat                       # interactive REPL (/reset, /quit)
./build/native/clawc run "read /etc/os-release and tell me the distro in one line"
./build/native/clawc daemon                     # all configured gateways (telegram/serve/heartbeat)
```

`clawc run` takes a one-shot prompt, prints the reply, and exits — it reads
stdin when given no args, so it is scriptable. `chat` is the interactive REPL.

## 3. Try something real

Give it a tool-using task and watch it work:

```sh
./build/native/clawc run "list the files in my workspace, then summarize MEMORY.md in three bullets"
```

Risky commands (`rm -rf`, writing to `/etc`, `curl | sh`, …) pause for
approval: `y/N` on the CLI. In Telegram and Discord they use inline buttons.

## Telegram bot (built-in channel)

1. Create a bot with [@BotFather](https://t.me/BotFather), copy the token.
2. Run:

```sh
export CLAWC_TELEGRAM_TOKEN="123456:ABC-DEF..."
export CLAWC_TELEGRAM_USERS="123456789"       # Telegram numeric user ID, NOT bot username
./build/native/clawc telegram -allow <your_chat_id>        # optional additional chat restriction
```

Then text your bot. It answers with full tool access and remembers each
chat's conversation. Commands: `/reset` (clear history), `/help`, plus
`/start` `/status` `/memory` `/model`.

## Local model (zero cost, no TLS needed)

Point clawc at a local OpenAI-compatible or Anthropic API:

```sh
./build/native/clawc -u http://127.0.0.1:11434/v1 -m qwen2.5:32b chat     # Ollama / llama.cpp
./build/native/clawc -a anthropic -u https://api.anthropic.com/v1 -m claude-sonnet-4-5 chat
```

## Where to go next

- Building, cross-compilation, channels, and the systemd service:
  [installation.md](installation.md)
- The setup wizard, the configuration file and environment, the daemon, and the
  workspace layout: [setup.md](setup.md)
