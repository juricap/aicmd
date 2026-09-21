# aicmd development

- C++17, CMake, native platform APIs; keep the shared core free of GUI types and JSON/model SDK dependencies.
- UI follows Total Commander / Midnight Commander: compact, keyboard first, full-width file panels. No persistent directory trees, sidebar, large toolbar, or navigation buttons. Tabs and a narrow path bar sit directly above each file list.
- Build and test Windows locally with `build.bat`.
- **Do not use WSL.** Linux builds/tests run through `ssh ai`, in an isolated directory under `/data/devel/`. Do not disturb other projects there.
- Keep progress tracked in local Git. Make coherent commits after validation. No GitHub remote or push until the user supplies the repository and requests publishing.
- Keep `obsidian/docs/`, `obsidian/todos/`, and `obsidian/status/` current. Record verified results separately from targets and untested behavior. Vault entry point: `obsidian/Home.md`.
- Keep build products, benchmark fixtures, screenshots, temporary sessions, and machine-specific state in ignored `build/` or `artifacts/`.
