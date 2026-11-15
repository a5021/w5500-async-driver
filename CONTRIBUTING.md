# Contributing

## Prerequisites

- A C99 compiler and GNU Make for the host build (`gcc` is the default,
  `clang` is required for fuzzing).
- CMake is not used; the Makefile targets are enough.
- FreeRTOS and CMSIS headers are only needed when building the ports, not for
  the host tests.

## Commands

```sh
make test       # three unit-test binaries against the chip emulator
make examples   # syntax-check the firmware examples
make lib        # build/libw5500.a
make fuzz-all   # 13 libFuzzer harnesses (Clang)
```

Fuzzing modes:

- `FUZZ_MODE=fuzzer` (default) - libFuzzer + ASan/UBSan. On Windows this needs
  an MSVC STL for the link step.
- `FUZZ_MODE=asan` - standalone runner + ASan/UBSan; replays the corpus once
  (regression mode). Use it for local verification on Windows.
- `FUZZ_MODE=none` - plain build, corpus replay only.

`FUZZ_TIME` sets seconds per target (default 120).

## Change workflow

1. **Mock first.** Anything the driver reads or writes from the chip must be
   modelled in `tests/mock_w5500.c` (registers, buffer memory, command side
   effects, behaviour knobs).
2. **Unit tests.** Add or extend `tests/test_w5500.c` (core),
   `tests/test_protocols.c` (DHCP/DNS/SNTP) or `tests/test_modbus.c`
   (Modbus). Tests run with `-Wall -Wextra -std=c99 -Werror`.
3. **Fuzz property.** New parsing, framing or state-machine code needs a
   harness in `tests/fuzz/` and a seed in `tests/fuzz/corpus/`, wired into
   the Makefile and `fuzz-all`. Properties are checked with `abort()`.
4. **Docs.** Update the header comments, the relevant document
   (`docs/api.md`, `docs/protocols.md`, `docs/architecture.md`) and
   `CHANGELOG.md` (Unreleased section).
5. **Examples.** User-visible API changes should be reflected in an example
   when one covers the area; `make examples` must stay green.

## Commit style

- Short imperative subject, optional body with bullet points.
- Prefixes in use: `feat(scope):`, `fix(scope):`, `docs:`, `fuzz:`,
  `examples:`, `release:`. Existing stage commits use `Stage N: ...`.
- One logical change per commit; keep CI green on every pushed commit.

## Code rules

- C99, no compiler warnings (`-Wall -Wextra -Werror`).
- No driver globals and no heap in the core/socket layers; caller-owned
  storage for contexts and protocol instances.
- Every fallible function returns `w5500_result_t` with documented codes.
- Bound every wait with the configured timeout.
- Callbacks run with the driver lock released; keep them re-entrant.
- Public headers carry a doc comment for every declaration.

## CI

`.github/workflows/ci.yml` runs `make test`, `make examples` (which executes
the emulator demo) and the fuzz suite on every push/PR to `main`, for both gcc
and clang; the gcc leg also produces a gcov coverage artifact.
`.github/workflows/nightly.yml` runs a 300-second campaign per harness over the
cached corpus. A red CI is a blocker; pull requests should reference the issue
they fix.

## License

By contributing you agree that your changes are licensed under the MIT
License (see `LICENSE`).
