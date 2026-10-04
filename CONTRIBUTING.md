# Contributing to Skiff

Thanks for helping. This page is for code contributions. To report a problem, open an
[issue](https://github.com/chiva/skiff/issues/new/choose).

## What you need

- **Docker.** Every build and check runs in a container through `scripts/dev.sh`, so you do not
  need the PSP toolchain installed.
- **pre-commit** (`pipx install pre-commit`), for the same formatting and hygiene checks CI runs.
- Optional: a PSP with custom firmware and a USB cable for the hardware test tier.

## Set up

```bash
git clone https://github.com/chiva/skiff.git
cd skiff
pre-commit install          # installs the pre-commit and commit-msg hooks only
scripts/dev.sh test         # host unit tests
scripts/dev.sh psp          # PSP EBOOTs in build/psp/pbp/
```

`pre-commit install` never touches `core.hooksPath` or the `pre-push` hook, so it coexists with
any local push hooks you have.

## Workflow

1. Branch from `main`.
2. Make the change with tests. Portable logic (everything outside `src/platform/`) must have unit
   tests; CI fails under 85% line coverage.
3. Run `scripts/dev.sh test asan lint psp selftest`.
4. Open a PR. Its title must follow [Conventional Commits](https://www.conventionalcommits.org/)
   (`feat: ...`, `fix: ...`, `docs: ...`), because PRs are squash-merged and the title becomes the
   commit that release-please reads to pick the next version and write the changelog.

Do not edit `CHANGELOG.md` or version numbers by hand. release-please owns them.

## Code style

- C11, formatted by clang-format (`.clang-format`), checked by clang-tidy and cppcheck.
- Every fallible function returns `skiff_err` (`include/skiff/error.h`). Add new codes to the table
  there, at the end of their group, with a sentence a player can understand.
- No magic numbers: name constants with an `enum` or `#define` next to where they are used.
- PSP system calls live only in `src/platform/psp/`. Everything else must build on the host.
- Comments explain why, not what.

## Testing tiers

| Tier | Runs in CI | Command |
|---|---|---|
| Unit (host) | yes | `scripts/dev.sh test`, `asan`, `coverage` |
| Emulator (PPSSPPHeadless) | yes | `scripts/dev.sh selftest` |
| Hardware (real PSP over PSPLINK) | no | see [docs/development/testing.md](docs/development/testing.md) |

Changes to networking, storage or `src/platform/psp/` need a hardware run before merging. Say
which PSP model and firmware you used in the PR.

## Test data

Never commit commercial game files, BIOS images, real saves, or your own RomM tokens or keys.
Fixtures must be homebrew, public domain, or synthetic.

## License

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
