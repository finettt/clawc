# AGENTS.md

This file contains instructions for AI coding agents (Codex, Claude Code, Cursor, Copilot, Gemini CLI, Windsurf, Aider, and others) working in this repository. It is the canonical agent instruction file for this project.

This is a universal standards file. It requires no per-project customization to work. Project-specific commands, architecture, or stack notes may optionally be appended below the final section marker.

## Overview

This repository follows a shared set of coding and collaboration standards. Read this entire file before making changes. Rules in this file take precedence over general conventions unless explicitly overridden by project-specific notes at the bottom.

## Commands

Project-specific build, test, lint, and run commands should be added in an optional `## Project Commands` section appended below the marker at the end of this file. If no such section exists:
- Discover the build system from common files (`package.json`, `pyproject.toml`, `Cargo.toml`, `build.gradle*`, `Makefile`, `CmakeLists.txt`, `go.mod`, etc.).
- Use the scripts and commands defined in those configuration files; do not invent new flags or workflows.
- Run `build`, `test`, and `lint` (or their closest equivalents) after making code changes and before finishing.

## Conventions

### General

1. **Match existing style.** When editing a file, follow the code style already present there. Do not reformat unrelated code in the same change.
2. **No commented-out code in merged work.** Delete dead code; rely on git history for retrieval.
3. **Self-contained comments.** Do not reference internal planning documents, spec section numbers, phases, or workstream IDs in code comments (e.g. "spec §7", "Phase 3", "B1", "item B9") — these are dangling pointers that do not exist in the repository. If a comment needs a reference, link to a real file in the repo (source, docs under `docs/`, test assets), or state the rule in prose.
4. **No silent errors.** Do not swallow exceptions. Wrap errors with context about the failed operation, or handle them explicitly.
5. **Clarity over cleverness.** Prefer simple, explicit, readable code over clever abstractions. Optimize for the next person reading the code.
6. **Pattern consistency.** Follow existing architectural patterns in the codebase. Do not introduce new patterns or dependencies without explicit direction from the user; if you believe a new pattern is warranted, propose it in the PR description.
7. **TODOs must be actionable.** `TODO` comments are acceptable only for in-progress work and must reference a tracking issue/PR number or be removed before merge.
8. **No new public functionality without tests.** Add appropriate tests for new public-facing code. Do not commit failing tests and do not skip tests without documenting the reason in the PR description.

### Filesystem

9. **Root-relative paths.** All file paths referenced in tool calls are relative to the repository root unless explicitly noted otherwise.
10. **Path traversal protection.** Validate all user-controlled or agent-controlled file paths against traversal attacks before performing filesystem operations.
11. **Generated files.** Do not manually edit build artifacts or generated files (e.g. `dist/`, `build/`, `*.gen.*`, lockfiles unless the user explicitly asks to update dependencies).

### Security

12. **No secrets in code.** Never commit API keys, credentials, tokens, or personal user data. Never log secrets. Use environment variables or a secrets manager.
13. **No security workarounds.** Do not suggest or commit code that disables certificate validation, weakens authentication, bypasses sandboxes, or otherwise works around security controls.

## Boundaries

The following items stay out of the repository entirely:
- **Planning documents, session notes, and drafts.** Keep them local (untracked, e.g. in a `plans/` directory or files named `PLAN.md` / `*-plan.md` covered by `.gitignore`), or in a private gist. Never commit them and never force-add ignored plans. If a plan is already tracked, remove it from the Git index with `git rm --cached` while preserving the local file. This rule applies to planning documents, not application source code or tests.

## Workflow

1. After making edits, run the relevant tests and linters before declaring work complete.
2. Do not commit unless explicitly asked — stage changes and provide a summary instead.
3. Update documentation and comments when changing behavior.
4. Do not modify dependency versions or add new dependencies without explicit direction.

## PR & Issue Communication

Public comments and PR descriptions are for reviewers, not for narration.

### PR and issue comments
- Max ONE structured comment per task. Follow-up edits go into that same comment, or as a short new comment — never duplicate the full text again.
- Format: findings as a flat list — `file:line — issue — suggestion`. Max 30 lines.
- Forbidden in comments: plans, checklists, roadmaps, "next steps", TODOs, internal reasoning, task decomposition, copies of documentation, pasted build/test logs (link to the CI run instead).
- No comment-editing churn: do not rewrite or delete your comments repeatedly.

### PR description
- Sections exactly: `What` / `Why` / `Tests` / `Limitations`. Max 40 lines total. The diff speaks for itself.

Violation of the communication policy is a defect — a comment that needs cleanup is treated as a bug, same as a failing test.

## Project Commands
Based on the `Makefile` (C99, POSIX libc). Every target except `clean` needs
the binary built first, so run `make` before the rest.

- Build:    `make` (TLS auto-detected) · `make TLS=0` (no OpenSSL; https via the
  `curl(1)` subprocess) · `make musl` (fully static) · `make debug`
- Sizes:    `make size`
- Cross:    `make cross CROSS=aarch64-linux-gnu TLS=1` (also `arm-linux-gnueabihf`,
  `riscv64-linux-gnu`, `i686-linux-gnu`)
- Channels: `make CHANNELS="telegram"` / `make CHANNELS="serve"` /
  `make CHANNELS=""` (the agent core is always compiled)
- Test:     `make test` (full regression via `test/run_python_tests.py`) ·
  `make test-asan` · `make test-fuzz` (JSON mutational fuzzer)
- Sanitize: `bash test/asan_check.sh` (run `make test` first for the TLS fixtures)
- Run:      `./clawc run "<msg>"` (one-shot) · `./clawc chat` (REPL) ·
  `./clawc daemon` (gateways) · `./clawc setup` (wizard) · `./clawc config`
- Clean:    `make clean`

## Project Overview
`clawc` is a ~98 KB–127 KB claw-style autonomous personal agent written in C99 +
POSIX. It is local-first: memory lives in plain Markdown, conversations survive
restarts, replies stream live, risky actions are confirmed, tools cover
shell/files/skills/web, and it runs on a timer (heartbeat + cron). It speaks
HTTPS with built-in TLS over Telegram, a TCP `serve` bridge, Matrix, and
Discord. See [README.md](README.md) and [docs/](docs/).

## Architecture
Three layers — channel / brain / body — all under `src/`:

- Entry / process: `main.c` (CLI dispatch), `daemon.c` (gateway supervisor +
  crash restart), `config.c` (file/env/flag resolution), `util.c` (buffers,
  files, process runner with timeout).
- Brain: `agent.c` (ReAct loop prompt→LLM→tool_calls), `llm.c` (OpenAI-compatible
  / Anthropic adapters + token streaming), `history.c` (append-only JSONL + audit
  log + turn-aware trimming), `cron.c` (schedules).
- Body (tools): `tools.c` (`run_shell`, `read_file`, `write_file`, `list_dir`,
  `remember`, `search_memory`, `load_skill`, `fetch_url`, `notify`, `subagent`),
  `packed_text*` (losslessly packed built-in strings), `jail.c`/`sandbox.c`
  (OS sandbox + path checks).
- Channels: `telegram.c`, `matrix.c`, `discord.c` + `ws.c` (Discord WebSocket),
  `bridge_tls.c`/`tls.c` (serve/A2A TLS), `http.c` (HTTP(S) transport, `curl`
  fallback when `TLS=0`).
- Primitives: `json.c` (DOM parser + writer), `tls.c` (OpenSSL client).

## Technology Stack
- Language: C99, POSIX libc only. No third-party runtime dependencies.
- Deps: OpenSSL (`libssl`/`libcrypto`) only when `TLS=1`; `TLS=0` needs just libc
  and uses the `curl(1)` subprocess for https.
- Toolchain: any C99 compiler + POSIX libc, `-Oz` + link-time GC for the size
  budget. Cross-compile for x86/ARM/RISC-V (no architecture-specific code); musl
  for a fully static binary.
- Tests: one Python entry point (`test/run_python_tests.py`) drives the
  functional suite; `test/fuzz_json.py` mutational fuzzing;
  AddressSanitizer/UBSan via `test-asan` and `test/asan_check.sh`.

## Project-Specific Conventions
- Compile-what-you-use channels: gate messaging front ends with `CHANNELS=` (see
  the Makefile). A build missing a channel must say so honestly — `clawc doctor`
  prints a `[features]` line and `clawc telegram` errors `'telegram' is not
  compiled into this build`. Never claim a channel that wasn't compiled in.
- The size budget is suite-enforced (`make test` asserts 98,312 B budget,
  126,984 B all-channels, ~150 KB static). Keep builds fitting; don't raise the
  budget without explicit direction.
- TLS is on by default for `serve` and A2A (`serve_tls`/`a2a_tls`). Do not weaken
  certificate or hostname verification to pass a test — plaintext is loopback
  only.
- Sender allowlists are mandatory per channel (`telegram_users`, `matrix_users`,
  `discord_users`). An empty or missing list denies all input and the gateway
  refuses to start.

## Safety
- Do not run production deploys or `systemctl enable --now` / copy into
  `/etc/systemd/system` from an agent session (see `deploy/clawc.service`).
- Do not disable hardening (`HARDEN=0`) or TLS verification to make a test pass.
- Do not commit the binary, build artifacts, or generated headers — `make clean`
  handles `./clawc`, `*-static`, `*-debug`, `*.o`, `*.so`, and
  `src/packed_text_data.h`.

## Troubleshooting
- `-Oz` build error on an older compiler: pass `CFLAGS="-Os ..."`.
- `make TLS=0` https calls fail: they use the `curl(1)` subprocess, which must be
  installed.
- OpenSSL headers missing: TLS auto-disables (`TLS=0`). Install OpenSSL headers
  for built-in TLS.
- `bash test/asan_check.sh` before `make test`: run `make test` first for the TLS
  fixtures.
- Cross/musl binary size differs from the x86-64 figures: sizes are
  platform- and toolchain-dependent; the suite enforces the dynamic x86-64 values.
