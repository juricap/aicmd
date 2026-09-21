# Development log

## 2026-09-21

- Studied Double Commander source and local jptxt conventions; wrote shared-core/native-frontend design.
- Built C++17 core, Windows filesystem provider, Win32 GUI, CLI, persistence, and regression tests.
- Fixed long-path fixture setup and session JSON indentation after tests exposed both issues.
- User clarified compact Total Commander / mc layout: removed tree/sidebar/buttons and added dense columns and keyboard marking.
- User specified Linux testing through `ssh ai` under `/data/devel/`; WSL is prohibited going forward.
- Initialized local Git and reorganized the vault into docs, todos, and status. GitHub remote is pending.
- Verified revised full-width UI visually and by native smoke, including keyboard routing and persistence.
- Built the headless core/CLI on `ssh ai` in `/data/devel/aicmd-check-20260921`; aligned POSIX ENOTDIR with Windows `invalid_path` errors. Windows and Linux pass 43 checks.
- Measured final Windows build and 10k/100k directory fixtures; results and remaining gaps are in Status.
- Initial local source checkpoint includes code, tests, scripts, dependency license, compact UI reference, and vault. Use `git log --oneline` for commit IDs.

See [Status](Status.md) for verified results and [M1 todos](../todos/M1-Windows.md) for open work.
