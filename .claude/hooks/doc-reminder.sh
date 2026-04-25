#!/bin/bash
# PostToolUse hook — reminds Claude to update documentation when source files change.
#
# Fires after Edit or Write tool calls. Reads the modified file path from stdin
# and injects an additionalContext reminder if the file belongs to a documented area.
#
# Project: ibkr-trading-app
# Docs:    CLAUDE.md, README.md, .claude/rules/*.md

INPUT=$(cat)

FILE_PATH=$(python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    print(d.get('tool_input', {}).get('file_path', ''))
except Exception:
    print('')
" <<< "$INPUT" 2>/dev/null)

if [ -z "$FILE_PATH" ]; then
    exit 0
fi

# Skip if the modified file IS documentation (avoid infinite loops)
if [[ "$FILE_PATH" =~ \.claude/ || "$FILE_PATH" =~ README\.md || "$FILE_PATH" =~ CLAUDE\.md ]]; then
    exit 0
fi

DOC=""

# src/core/services/ — IBKRClient bridge, IBKRUtils
if [[ "$FILE_PATH" =~ src/core/services/ ]]; then
    DOC=".claude/rules/architecture.md (IBKRUtils / IBKRClient sections), .claude/rules/ibkr-api.md, .claude/rules/task-history.md, and README.md"

# src/core/models/ — data model structs
elif [[ "$FILE_PATH" =~ src/core/models/ ]]; then
    DOC=".claude/rules/architecture.md (Models / Multi-Instance Windows section), .claude/rules/task-history.md, and README.md"

# src/ui/UiScale.h — em() / FlexRow helpers
elif [[ "$FILE_PATH" =~ src/ui/UiScale\.h ]]; then
    DOC=".claude/rules/architecture.md (UiScale section) and README.md"

# src/ui/windows/ — per-window UI files
elif [[ "$FILE_PATH" =~ src/ui/windows/ ]]; then
    DOC=".claude/rules/architecture.md (One window = one file / Multi-Instance Windows), .claude/rules/task-history.md, and README.md"

# src/main.cpp — Vulkan init, login state machine, top-level dispatch
elif [[ "$FILE_PATH" =~ src/main\.cpp ]]; then
    DOC="CLAUDE.md, .claude/rules/architecture.md (Main Entry Point / Connection State Machine / Settings / Windows Menu / Window Groups), .claude/rules/task-history.md, and README.md"

# src/bid_stubs/ — Intel BID64 stubs
elif [[ "$FILE_PATH" =~ src/bid_stubs/ ]]; then
    DOC=".claude/rules/ibkr-api.md (bid_stubs section) and .claude/rules/build.md"

# tests/ — Catch2 test suite
elif [[ "$FILE_PATH" =~ tests/ ]]; then
    DOC=".claude/rules/testing.md and .claude/rules/task-history.md"

# CMakeLists.txt — build config, options, dependencies
elif [[ "$FILE_PATH" =~ CMakeLists\.txt ]]; then
    DOC=".claude/rules/build.md (Commands / Dependencies / CMake Options) and README.md"

# .github/ — CI workflows
elif [[ "$FILE_PATH" =~ \.github/ ]]; then
    DOC=".claude/rules/testing.md (CI Jobs section)"

# twsapi_macunix*/ — IB TWS API sources
elif [[ "$FILE_PATH" =~ twsapi_macunix ]]; then
    DOC=".claude/rules/ibkr-api.md"
fi

if [ -n "$DOC" ]; then
    python3 -c "
import json
msg = 'Modified: $FILE_PATH — check if $DOC needs updating to reflect this change.'
print(json.dumps({
    'hookSpecificOutput': {
        'hookEventName': 'PostToolUse',
        'additionalContext': msg
    }
}))
"
fi

exit 0
