# C++ Style Rules

## Language Standard
- Target: **C++20**, C++23 features welcome where compiler support is confirmed
- Compiler: GCC/Clang on Linux, MSVC on Windows

## Naming
- Classes/Structs: `PascalCase`
- Functions/Methods: `camelCase`
- Member variables: `m_camelCase` (or plain `camelCase` for POD structs)
- Global/static: `g_camelCase` or `s_camelCase`
- Constants/Enums: `UPPER_SNAKE_CASE` or `PascalCase` enum class values
- Files: `PascalCase.h` / `PascalCase.cpp` matching the class name

## General Rules
- Prefer `std::variant` + message queues over raw callbacks crossing thread boundaries
- Prefer POD structs in `core/models/` — no business logic in data models
- Use `[[nodiscard]]` on functions whose return value must not be ignored
- Avoid raw owning pointers — use `std::unique_ptr` / `std::shared_ptr`
- Cross-platform code: isolate platform-specific paths in adapters, not scattered inline

## What to Avoid
- Do not call ImGui functions from non-UI threads
- Do not put IB API logic in UI window files — route through the service layer
- Do not use `using namespace std` in headers
