# Contributing to Ortaocak

Thank you for wanting to help. This is a hobby project built in the open; the rules below keep it
healthy. (Türkçe yazmaktan çekinmeyin: issue ve pull request'lerde Türkçe de İngilizce de olur.)

## How changes get in

1. **Talk first** for anything bigger than a small fix: open an issue and describe what you want to
   change and why. It saves you from building something that will not be merged.
2. **Fork** the repository, create a branch from `main`, make your change.
3. **Open a pull request** against `main` and fill in the template.
4. **A maintainer reviews every pull request.** Nothing is merged without an approving review from a
   maintainer, and nobody pushes to `main` directly. Expect questions and change requests; that is
   normal.

## Before you open a pull request

- The tree builds without warnings: `powershell -File build.ps1`.
- The tests pass: `powershell -File build.ps1 -Test`. If you touched the client's UI or the world
  scene, also run the relevant scripted flows: `tools\dev\run-ui-flows.ps1 -Only <names>`.
- New behaviour comes with tests. Server rules get unit or in-process server tests; anything
  player-visible gets a scripted flow with screenshots when practical.
- Code follows [docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md) (`.clang-format` is in the root).
- Keep a pull request to one topic. Small pull requests are reviewed faster.

## Rules that are not negotiable

- **No secrets.** Never commit connection strings, passwords, keys or tokens. Local settings live in
  the git-ignored `config/*.local.toml` and `config/keys/`.
- **Original content only.** Do not copy names, texts, art, sounds, maps, numbers or data from other
  games or from any source whose licence does not allow it here. Mechanics common to the genre are
  fine; someone else's content is not. Every third-party asset or library must be listed in
  [THIRD_PARTY.md](THIRD_PARTY.md) with its source and licence, in the same pull request.
- **The server decides.** Game rules run on the server; the client only presents. A change that
  trusts the client with a game decision will be rejected.
- **Database changes are new migration files** in `db/migrations/` (next number, never edit an old
  one), documented in `docs/DATABASE.md`.
- **Protocol changes** bump `kProtocolVersion` in `shared/net/Opcodes.h` and are documented in the
  matching `docs/PROTOCOL_*.md`.
- Player-visible text is Turkish; code, comments and reference documents are English.

## Reporting bugs

Open an issue with: what you did, what you expected, what happened, your build (Debug or Release,
commit), and the client/server log lines if you have them. Screenshots help.

**Security problems** (anything that lets a player cheat, crash the server or read someone else's
data): please do not open a public issue; use GitHub's private vulnerability reporting on this
repository instead.

## Licence of contributions

By submitting a contribution you agree that it is your own work (or that you have the right to
submit it) and that it is provided under the licence of this repository (see [LICENSE](LICENSE)).
