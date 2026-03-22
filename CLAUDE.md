# CLAUDE.md

Interactive Brokers Trading Application — C++20, Dear ImGui (Vulkan), CMake, IB TWS API.

## Rules

Detailed rules live in `.claude/rules/`:

- [`build.md`](.claude/rules/build.md) — CMake commands, binary path, dependencies, platform notes
- [`architecture.md`](.claude/rules/architecture.md) — Directory structure, design principles, main.cpp pattern
- [`ibkr-api.md`](.claude/rules/ibkr-api.md) — IB API quirks, ports, IBKRClient bridge, protobuf, bid_stubs
- [`vulkan-imgui.md`](.claude/rules/vulkan-imgui.md) — ImGui version, valid/removed API calls, Vulkan pitfalls
- [`cpp-style.md`](.claude/rules/cpp-style.md) — Language standard, naming, what to avoid
- [`task-history.md`](.claude/rules/task-history.md) — Completed task list (phases 1–4)
