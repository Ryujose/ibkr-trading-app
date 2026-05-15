# AI Integration — Streaming Chat Foundation (Claude / OpenAI / DeepSeek / Ollama)

**Status:** Draft (not yet started).
**Goal:** Build the foundational service layer for AI integration in this app — streaming chat completions across four providers (Anthropic Claude, OpenAI, DeepSeek, Ollama for local models), async dispatch matching the existing `IBKRClient` worker-thread pattern, env-var-only API keys (no plaintext on disk), and a Settings UI to pick provider + model with a curated dropdown **plus** a free-text override so future models can be used without recompiling. Wires the existing ReplayWindow `AI Analysis` buttons (Task #72) and unlocks future per-window features (ChartWindow "Explain Chart", natural-language order construction, news sentiment classification).

This file is the source of truth across sessions — update **Status** and per-task checkboxes as work progresses.

---

## 1. Approved scope (from conversation)

| Decision | Choice | Notes |
|---|---|---|
| Streaming vs one-shot | **Streaming** | SSE, deltas rendered as they arrive. Async by definition. |
| Dispatch model | **Worker thread → message queue → UI-thread drain** | Mirrors `IBKRClient` (`std::variant` queue + `ProcessMessages()` each frame). |
| API key storage | **Env vars only** | `ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `DEEPSEEK_API_KEY`. Read at startup; Settings panel shows masked-tail (`sk-...4f2c`) + green/red present indicator. **No on-disk key storage.** |
| Provider + model selection | **Dropdown with curated catalogue + free-text override** | Each provider has a hardcoded model list (table below), but the Settings UI also exposes a "Custom model" text input so brand-new model ids work without a code change. |
| Local models | **Ollama** | Discover installed models via `GET /api/tags`. Falls through OpenAiCompatProvider (Ollama exposes an OpenAI-compatible endpoint). |

## 2. C++ dependencies

| Need | Choice | Where it comes from |
|---|---|---|
| HTTP + HTTPS + SSE streaming | **libcurl** | System package: `libcurl4-openssl-dev` (Linux), brew (macOS), vcpkg or system (Windows). `find_package(CURL REQUIRED)` in CMake. |
| JSON parse + build | **nlohmann/json** | FetchContent (single header) — same pattern as ImGui/ImPlot. |
| SSE framing | **In-house** | A 20-line state machine over libcurl's write callback. SSE format (`data: {json}\n\n`) is trivial. |

## 3. Architecture

### Layer 1 — `HttpClient` (libcurl wrapper)

`src/core/services/HttpClient.h/.cpp` — thin libcurl wrapper. Single class, no provider knowledge.

```cpp
namespace core::services {

struct HttpRequest {
    std::string url;
    std::string method = "POST";                  // "GET" for Ollama /api/tags
    std::vector<std::string> headers;
    std::string body;                             // JSON string for POST
    int timeoutSeconds = 60;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Synchronous (blocks). For streaming, pass onChunk — body bytes arrive as
    // they're received from the wire. Returns HTTP status code; -1 on transport
    // error (errMsg populated). Caller runs this on a worker thread.
    int Send(const HttpRequest&,
             std::function<void(std::string_view)> onChunk,
             std::string* errMsg = nullptr);
};

}
```

Implementation: `curl_easy_init`, `CURLOPT_WRITEFUNCTION` → invokes `onChunk` per receive, `curl_easy_perform`, cleanup. No global state, no shared session — one HttpClient per worker thread, one libcurl handle per request.

### Layer 2 — `AiProvider` interface + concrete classes

`src/core/services/AiProvider.h` — abstract interface.

```cpp
namespace core::services {

struct AiMessage {
    std::string role;       // "user", "assistant", "system" (handled per-provider)
    std::string content;
};

struct AiRequest {
    std::string          model;         // provider-specific id — caller-supplied, no validation
    std::string          systemPrompt;
    std::vector<AiMessage> messages;
    int                  maxTokens   = 1024;
    double               temperature = 0.7;
};

struct AiCallbacks {
    std::function<void(std::string_view)> onChunk;   // partial text delta
    std::function<void()>                 onDone;
    std::function<void(std::string)>      onError;
};

class AiProvider {
public:
    virtual ~AiProvider() = default;
    virtual const char* Name() const = 0;
    // Curated catalogue. UI also allows arbitrary user-typed model ids — the
    // Send path passes `request.model` through verbatim, no whitelist check.
    virtual std::vector<std::string> AvailableModels() const = 0;
    virtual void Send(const AiRequest&, AiCallbacks) = 0;
};

}
```

**Free-text override design**: `AvailableModels()` is a *suggestion list*, not a whitelist. `AiClient` and the Settings UI never reject a user-typed model id. The string lands in `AiRequest::model` and is passed straight to the provider's API. If the model id is invalid the LLM API returns its own error (typically a 400 with an error message like `"model: <id> not found"`); we surface that through `onError`. No client-side gatekeeping — keeps the catalogue maintenance-free as providers ship new models.

Concrete classes:

```cpp
class AnthropicProvider : public AiProvider {
    // POST https://api.anthropic.com/v1/messages
    // Headers: x-api-key, anthropic-version: 2023-06-01
    // Schema: { model, max_tokens, system, messages[] }
    //   - "system" is top-level (NOT a message role)
    //   - SSE event: content_block_delta with text deltas
};

class OpenAiCompatProvider : public AiProvider {
    // POST {baseUrl}/v1/chat/completions
    // Schema: { model, messages[{role,content}], stream:true, max_tokens, temperature }
    //   - system prompt encoded as messages[0].role="system"
    //   - SSE event: "data: {json}" with choices[0].delta.content
    //
    // One class instance per provider (OpenAI, DeepSeek, Ollama), parameterised
    // by Config { baseUrl, apiKeyEnvVar, staticModels, isLocal }.
public:
    struct Config {
        std::string name;
        std::string displayName;
        std::string baseUrl;       // "https://api.openai.com" / "https://api.deepseek.com" / "http://localhost:11434"
        std::string apiKeyEnvVar;  // "OPENAI_API_KEY" / "DEEPSEEK_API_KEY" / "" (Ollama no auth)
        std::vector<std::string> staticModels;
        bool isLocal = false;      // true for Ollama; enables /api/tags discovery
    };
};
```

`OpenAiCompatProvider` is the workhorse — same body schema, same SSE format, same chunk parser. The only per-provider differences are URL, auth header, and the *suggestion* model list.

### Layer 3 — `AiClient` (orchestrator + message queue)

`src/core/services/AiClient.h` — service held by `main.cpp`, owns one worker thread per active request and a thread-safe message queue.

```cpp
class AiClient {
public:
    AiClient();
    ~AiClient();

    void RegisterProvider(std::unique_ptr<AiProvider>);
    std::vector<AiProvider*> Providers() const;
    AiProvider* FindProvider(const std::string& name) const;

    // Drained from UI thread once per frame (same pattern as IBKRClient).
    void ProcessMessages();

    // Cancels an in-flight stream. cbId returned from provider->Send via
    // AiClient internal handle.
    void Cancel(int cbId);

    // Dynamic model discovery (Ollama only).
    void RefreshModels(AiProvider*);

private:
    struct Msg { /* variant: Chunk{cbId, text} | Done{cbId} | Error{cbId, msg} | ModelsRefreshed{providerName, list} */ };
    std::mutex                           m_qMutex;
    std::queue<Msg>                      m_queue;
    std::unordered_map<int, AiCallbacks> m_callbacks;
    std::unordered_map<int, std::atomic<bool>> m_cancelFlags;
    std::vector<std::unique_ptr<AiProvider>> m_providers;
    int m_nextCbId = 1;
};
```

Flow per request:
1. Window code calls `provider->Send(request, callbacks)`.
2. Provider allocates a `cbId`, stores callbacks + cancel flag in `AiClient`.
3. Short-lived worker thread spawned, joined on completion.
4. Worker calls `HttpClient::Send` with a write callback that parses SSE.
5. For each SSE `data: {json}` line, worker extracts delta text and pushes `Msg::Chunk{cbId, text}` onto the queue.
6. On stream end, pushes `Msg::Done{cbId}`.
7. On HTTP error / parse failure / cancellation, pushes `Msg::Error{cbId, msg}`.
8. UI thread's frame loop calls `AiClient::ProcessMessages()` — drains queue, invokes callbacks, erases on Done/Error.

### Layer 4 — Configuration & env-var resolution

`src/core/services/AiConfig.h`:

```cpp
struct AiConfig {
    struct Provider {
        std::string name;
        std::string displayName;
        bool        keyPresent     = false;
        std::string maskedKeyTail;        // last 4 chars
        bool        isLocal        = false;
        std::vector<std::string> models;  // curated suggestion list
    };
    std::vector<Provider> providers;
    std::string defaultProviderName;      // persisted
    std::string defaultModelName;         // persisted — may be a custom user-typed id
    int         maxTokens   = 1024;
    double      temperature = 0.7;
};

AiConfig LoadAiConfig();
```

Env vars consulted at startup:
- `ANTHROPIC_API_KEY` → Claude
- `OPENAI_API_KEY` → OpenAI
- `DEEPSEEK_API_KEY` → DeepSeek
- (Ollama: no key; checked via `GET /api/tags` probe)

Persisted: `~/.config/ibkr-trading-app/ai-prefs.cfg` (atomic `.tmp`+`rename`):
```
DEFAULT_PROVIDER:deepseek
DEFAULT_MODEL:deepseek-v4-pro
TEMPERATURE:0.7
MAX_TOKENS:1024
```

The model field is **any string** — could be from the curated dropdown OR user-typed. Persists across sessions. No validation on load.

## 4. Model catalogue (Jan 2026 — curated suggestions, user-extensible)

These are the **starter dropdown contents**. Users can type any model id in the "Custom model" field next to the dropdown to use models not listed here (Settings UI §5).

### Anthropic
```
claude-opus-4-7              # best, most expensive
claude-sonnet-4-6            # balanced default
claude-haiku-4-5-20251001    # fast + cheap
claude-3-5-sonnet-20241022   # legacy
claude-3-5-haiku-20241022    # legacy fast
```

### OpenAI
```
gpt-5
gpt-5-mini
gpt-5-nano
o3
o3-mini
o4-mini
gpt-4o
gpt-4o-mini
gpt-4-turbo
```

### DeepSeek
```
deepseek-v4-pro              # flagship
deepseek-v4-flash            # fast tier
deepseek-chat                # V3 chat
deepseek-reasoner            # R1 reasoning
deepseek-coder               # code-focused
```

### Ollama (dynamic — discovered via `GET /api/tags`)
Populated at startup from the local Ollama instance's installed models. Response shape:
```json
{ "models": [
    { "name": "llama3.3:70b", "modified_at": "...", "size": 42500000000 },
    { "name": "qwen2.5-coder:32b", ... }
]}
```

If Ollama is unreachable, the provider is flagged offline in Settings; user sees *"Start Ollama (`ollama serve`) and click Refresh"*. Manual Refresh button re-runs the probe.

## 5. Settings UI

`main.cpp::RenderSettingsWindow()` gains an **AI Providers** section:

```
┌─ AI Providers ─────────────────────────────────────────────┐
│  Default provider: ( ) Claude  ( ) OpenAI                  │
│                    (●) DeepSeek ( ) Ollama (local)         │
│                                                            │
│  Model:            [deepseek-v4-pro          ▼]            │
│                    or custom: [                       ]    │
│                                                            │
│  Max tokens:       [1024]    Temperature: [0.7]            │
│                                                            │
│  ── Provider status ──                                     │
│  Claude    ✓ key set (sk-...4f2c)   5 suggested models     │
│  OpenAI    ✗ OPENAI_API_KEY not set                        │
│  DeepSeek  ✓ key set (sk-...8a1b)   5 suggested models     │
│  Ollama    ✓ reachable              7 installed   [Refresh]│
└────────────────────────────────────────────────────────────┘
```

- **Provider radio**: only enabled providers selectable; others greyed with tooltip naming the missing env var.
- **Model combo**: populated from the selected provider's curated `models` list.
- **Custom model input**: free-text field below the combo. When non-empty, **overrides** the combo selection — the typed string is what gets sent. Tooltip: *"Use any model id this provider supports — handy for new models not yet in the dropdown. Invalid ids return an error from the provider."*
- **Refresh button**: only on Ollama row.
- **Selection persistence**: both the dropdown pick AND the custom-typed value persist to `ai-prefs.cfg`. Custom takes precedence when set.

## 6. ReplayWindow wire-up (existing UI)

Task #72 shipped greyed-out provider buttons for Anthropic / OpenAI / DeepSeek + a `Copy day summary to clipboard` button. Replace with:

1. **Provider combo** (uses `AiClient::Providers()`).
2. **Model combo** (curated list for selected provider) **+ custom-model input** mirroring the Settings UI pattern.
3. **Send button** — builds the request from the existing day-summary Markdown template.
4. **Streaming output panel** — renders incoming text chunks in a `BeginChild` scroll region.
5. **Cancel button** — visible during streaming, invokes `AiClient::Cancel(cbId)`.
6. **Copy to clipboard** — already exists, untouched.

Defaults inherit from Settings; per-window override allowed via the combo + custom-input.

## 7. Build system

`CMakeLists.txt`:

```cmake
find_package(CURL REQUIRED)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

target_link_libraries(ibkr-trading-app PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    # ... existing deps
)
```

CI: add `libcurl4-openssl-dev` to the Ubuntu install step. macOS uses system curl. Windows: `vcpkg install curl[ssl]` or rely on the SDK.

`tests-core` doesn't link libcurl. JSON request-builder tests can live in `tests-core` since `nlohmann/json` is header-only.

## 8. Plumbing dependencies

| Need | Source |
|---|---|
| Message queue + drain pattern | `IBKRClient` — same shape, copy the idea |
| Settings file persistence | `daily-risk.cfg` / `chart-modes.cfg` (atomic `.tmp`+`rename`) |
| Notification toasts | `NotificationService` |

New `NotificationEvent` entries (appended after `OrderBlocked` from `small-caps.md`):
- `AiResponseDone` — request completed normally (Info)
- `AiError` — request failed (Warning/Error severity by code)

Tones + voice WAVs added to `tools/gen_tones.cpp` + `tools/voice_phrases.txt`.

## 9. Tasks

- [ ] **Task 1** — `HttpClient` wrapper (libcurl). CMake `find_package(CURL)`. Implement `Send()` with sync + streaming-callback paths. Smoke-test against `https://httpbin.org/post` (one-shot) and `https://httpbin.org/stream/3` (streaming). ~85 lines.

- [ ] **Task 2** — `AiProvider` interface + `AnthropicProvider` + `OpenAiCompatProvider`. JSON request builders, SSE chunk parser, Anthropic-specific event parser. Curated model catalogue (Jan 2026 lists from §4). Unit-test the request JSON shapes in `tests-core` (no network). ~260 lines.

- [ ] **Task 3** — `AiClient` orchestrator. Message queue, worker-thread dispatch, `ProcessMessages()` drain, cancellation, `RegisterProvider`, `FindProvider`, `RefreshModels`. ~160 lines.

- [ ] **Task 4** — `AiConfig` + env-var resolution + `ai-prefs.cfg` persistence. `LoadAiConfig()`, `SaveAiPrefs()`. Mirrors `daily-risk.cfg` pattern. The `defaultModelName` field accepts arbitrary strings (no whitelist). ~90 lines.

- [ ] **Task 5** — Ollama model discovery. `GET /api/tags` via `HttpClient`, parse response, populate `Provider::models`. Manual `Refresh` button wired to `AiClient::RefreshModels`. ~45 lines.

- [ ] **Task 6** — Settings panel **AI Providers** section. Radio + model combo + **custom model text input** + max-tokens / temperature inputs + per-provider status rows + masked-key display + Refresh button. The custom-input field, when non-empty, overrides the combo selection on save and on Send. ~140 lines in `main.cpp::RenderSettingsWindow`.

- [ ] **Task 7** — ReplayWindow wire-up. Replace greyed-out buttons with real provider/model combos + custom-input + Send + streaming render + Cancel. Reuses existing day-summary Markdown template. ~90 lines.

- [ ] **Task 8** — `NotificationEvent::AiResponseDone` / `AiError`, tone + voice WAVs, wire toast hooks in `AiClient`. ~20 lines + 2 new WAVs.

After this lands, ChartWindow "Explain Chart" + future AI features become small additions calling `provider->Send(...)` with a different prompt template. Plan that as a separate `chart-ai.md` once Tasks 1–8 ship.

## 10. Edge cases & gotchas

- **libcurl thread safety**: `curl_global_init` must be called once at app startup, before any threads. Add to `main()` before `glfwInit`. `curl_global_cleanup` at app exit.
- **SSL certs on Windows**: libcurl on Windows needs `CURLOPT_CAINFO` pointed at a CA bundle. vcpkg ships one; document the path in `build.md`.
- **Streaming back-pressure**: queue is unbounded — typical chunks <100 bytes, 30fps drain keeps up. Add a soft cap (e.g. 10k queued chunks) that triggers an Error and aborts rather than ballooning memory.
- **Ollama not running**: probe is async — UI doesn't block. 2s timeout flags it offline; user-clicked Refresh re-probes.
- **Env var changes**: not detected at runtime. Document "restart after `export ANTHROPIC_API_KEY=...`" in the Settings panel tooltip.
- **Rate limits**: `429` or specific error JSON. Surface clearly: `"Anthropic rate limit hit — retry in 30s"`. No auto-retry — user decides.
- **Anthropic SSE quirk**: events come in pairs (`event: content_block_delta\ndata: {json}\n\n`). Parser must read the `event:` line. OpenAI-compat skips `event:` and just sends `data: {json}\n\n`.
- **Custom model id typos**: a typo'd model id returns a provider error (`"model: claud-sonet-4-6 not found"`). We surface verbatim — no client-side spell-check. Costs zero developer time and handles every new model that ships.
- **Cancellation race**: SSE parser checks `cancel` flag between chunks. If a chunk is mid-parse when cancelled, that chunk completes but no further deltas are emitted. Acceptable.
- **Local-vs-cloud privacy hint**: when Ollama is the selected provider, render a small green `LOCAL — data stays on your machine` chip near the model dropdown.

## 11. Out of scope (deferred)

- **Tool calling / function calling** — Anthropic and OpenAI both support it; great for natural-language orders, but bigger scope. Defer to `chart-ai.md`.
- **Vision** — sending chart screenshots to multimodal models. Powerful but expensive and tangential. Defer.
- **Conversation history persistence** — v1 is single-turn (each request independent). Multi-turn chats need a transcript store. Defer.
- **Per-window provider override beyond model** — v1 uses default-provider/default-model from Settings everywhere, with per-window model override via the combo+custom-input. Per-window provider override (Chart→Claude, Replay→DeepSeek) is easy to layer on later.
- **Cost dashboards / budget caps** — like the daily-loss limit but for AI spend. Defer.
- **Prompt template editor in Settings** — v1 uses hardcoded templates per feature. Defer.
- **Custom-model autocomplete** — typing in the custom field could fetch the live model list from each provider's `/v1/models` endpoint. Nice-to-have, not v1.

## 12. Acceptance per task

**Task 1** — `HttpClient::Send` POST to `httpbin.org/post` returns 200 + echoed payload. Streaming `GET httpbin.org/stream/5` invokes onChunk 5 times.

**Task 2** — Build `AiRequest`, serialise via `AnthropicProvider::BuildBody(req)` and `OpenAiCompatProvider::BuildBody(req)`. Compare against expected JSON shapes in Catch2 `[ai-providers]` tag (~6 cases). Custom-model-id case: `request.model = "claude-future-model-9"` round-trips through `BuildBody` unchanged.

**Task 3** — Mock provider emits 3 chunks + done; `Send` → drain via `ProcessMessages()` → assert all 3 chunks delivered. Cancellation case: cancel mid-stream, assert no further chunks delivered.

**Task 4** — Set `ANTHROPIC_API_KEY=test-key-abcd`, call `LoadAiConfig()`, assert Claude `keyPresent=true` with `maskedKeyTail="abcd"`. Round-trip `defaultModelName="my-experimental-model"` through save/load — survives intact.

**Task 5** — Run `ollama serve` locally with `llama3.1:8b` pulled. Refresh populates list. Ollama not running: provider marked offline.

**Task 6** — Settings panel renders 4 providers. Disabled providers grey + tooltip naming env var. Typing `gpt-5-experimental` in custom-model and saving persists; survives restart; Send uses the custom id. Clearing custom field reverts to dropdown selection.

**Task 7** — ReplayWindow `AI Analysis` tab: pick DeepSeek + `deepseek-v4-pro` (from dropdown) → Send → streaming text within 1-2s. Same flow with custom-model `deepseek-v5-preview` (typed) hits the provider with that string; if invalid, error toast fires.

**Task 8** — Stream-completed fires `AiResponseDone` toast (Info). Network failure fires `AiError` toast (Warning). Both honour per-event mute settings.

End-to-end: each of the four providers can complete a 100-token summary of a hardcoded test message within 30s. Build clean, tests-core green.

## 13. Open questions

1. **Default provider** when multiple keys are set? Probably Anthropic Claude (best chart-reasoning per dollar). User overrides in Settings.
2. **Should the model dropdown be sorted alphabetically or "best first"?** Best-first feels more useful (top suggestion is the default for new users). Alphabetical is more findable for power users. I'd default to best-first matching the §4 catalogue order, with a future "sort A-Z" toggle if needed.
3. **Custom-model field — global only, or per-feature?** v1: global in Settings. Per-feature override (custom model just for ChartWindow "Explain Chart") could come later.
4. **Ollama probe timing**: probe on every Settings-panel open vs only on Refresh? Probe on first open per session (so stale "offline" doesn't linger), then user-driven Refresh. `m_ollamaProbedThisSession` flag.
