# Choices

Decisions that should not silently change. [Home](../Home.md), [design](Design.md).

- **C++17 / CMake / native APIs**, following jptxt. Shared core is independent of windowing, serialization, and model protocols.
- **Compact keyboard-first commander.** User's [Total Commander screenshot](../../totalcmd.png) and Linux mc are the layout reference. Two full-width file panels; small tabs/path bars; no persistent trees, sidebars, big buttons, or toolbar padding.
- **Windows first**, with real shared-core tests before adding other frontends.
- **No WSL.** Linux checks run via `ssh ai` under `/data/devel/`, in an isolated project folder.
- **Use jptxt externally** for editing; do not fork its engine. F3 cannot claim read-only mode before that contract exists.
- **Local Git now; GitHub later.** User will provide a remote. No hosting or deployment is implied.
- **Obsidian vault** has `docs/`, `todos/`, `status/`, and a linked Home note. Mark unverified behavior explicitly.
