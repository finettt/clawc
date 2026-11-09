# Python tests

Run `make test` for the functional, bridge and swarm regression suite.
`make test-asan` additionally compiles a temporary ASan/UBSan build and runs
its smoke tests. `make test-fuzz` runs the deterministic JSON mutational
fuzzer; `make test-fuzz-asan` instruments the parser with ASan/UBSan. These
use Python's standard library; compiling the product, OpenSSL command-line
fixtures and the native test probes still require the relevant C toolchain
and OpenSSL development files. Sanitizer mode requires compiler runtime
support, including a discoverable `libasan.so` for the fuzzer.

The functional cases are grouped into Python unittest methods, with multiple
assertions and subtests per method; their count is not directly comparable to
the old shell suite's individual check count. The migrated cases cover builds
and size, TLS and A2A, sandbox/jail, CLI and configuration, Telegram/Matrix/
Discord flows and requester authorization, history and compaction, daemon,
cron, self-improvement and subagents. Existing focused Python tests cover ELF,
CLI, bridge workers and the swarm index. Swarm smoke and load are also run.

Native `rawsys_escape.c` and `jail_once.c` are fixtures, not shell test
runners: they exercise raw syscalls and the C jail exit contract. The packed
text probe generated temporarily by `test_core_python.py` exposes the actual C
literals and sizes. All test assertions and orchestration for those probes are
in Python. Tests use private workspaces, ephemeral ports and owned processes;
they do not use broad process-name kills. A short port-release/rebind race
remains for child processes whose interfaces accept only a port number.

The sandbox checks exercise the implemented fallback behavior; they do not
establish that the fallback securely confines hostile native code. Test success
is not a security proof. The native `LD_PRELOAD` bridge fsync-failure injection
is skipped for custom `--binary` targets, as those may be QEMU guests; the
runner reports that omission explicitly.
