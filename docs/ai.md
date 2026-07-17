# Local AI start page

`about:start` is a chat window backed by a small language model that runs
entirely on the CPU through [llama.cpp](https://github.com/ggml-org/llama.cpp).
Inference is fully local: once the model is on disk, no network is touched —
the prompt is tokenized, decoded, and detokenized in the same process that
fetches pages.

## How it works

- `src/ai.c` (C) owns a single lazily-loaded `llama_model` + `llama_context`
  guarded by a mutex. Generation runs **asynchronously**: `ns_ai_chat_start()`
  spawns a worker thread and returns a job id immediately; the worker formats
  the conversation with the model's built-in chat template, runs a sampled
  generation, and appends each decoded token to the job's partial text, which
  `ns_ai_chat_poll()` reports — so the start page streams the reply as it is
  produced instead of blocking until the end. A model that sits unused for
  five minutes is unloaded to give the memory back.
- The chat is **multi-turn**: the last 16 turns are kept and replayed through
  the chat template on every request (4096-token context, 640-token reply
  budget), oldest pair dropped first whenever the prompt would not fit the
  context window alongside the reply budget. `ns_ai_chat_reset()` (the start
  page's "New chat" link) clears the transcript and cancels any generation in
  flight.
- **Reasoning models** are handled: `<think>…</think>` spans are stripped
  from both the streamed and the final reply (with a holdback so a partial
  tag never flashes on screen), and Qwen3-architecture models get the
  `/no_think` soft switch appended to the system prompt so they answer
  directly. Models whose chat template rejects a `system` role (Gemma) get
  the system prompt folded into the first user turn before the generic
  ChatML fallback is even considered.
- `src/net.c` exposes the internal endpoints the start page calls with
  `fetch()` — answered by `synthesize_about_response()`:
  - `about:ai-status` → JSON `{state, active, models:[…], …}` where `state` is
    `idle`, `downloading` (with the `downloading` id + `percent`/`received`/
    `total`), `ready`, `error`, or `disabled`, and `models` lists each tier
    with its size and an `installed` flag.
  - `about:ai-download?model=<id>` → selects a model and, if it isn't already
    on disk, starts the background download. Returns status.
  - `about:ai?q=…` → starts a generation job; JSON `{job}` (or
    `{job, busy:true}` while a previous job is still running).
  - `about:ai-poll` → JSON `{job, state, phase, text}` where `state` is
    `running`, `done`, or `error`, `phase` is `starting` / `loading model` /
    `thinking` / `searching`, and `text` is the streamed partial (then final)
    reply.
  - `about:ai-reset` → clears the conversation and cancels the running job.
- The chat UI is the `about:start` HTML/JS template in `src/net.c`. On load it
  polls `about:ai-status`; if no model is installed it shows a picker of the
  available models, downloads the chosen one with a progress bar, then enables
  the chat. The active model is shown in the footer with a "change" control.

## Building

llama.cpp is built **from source** as a Meson CMake subproject (no prebuilt
binaries are vendored), so it works on Linux, macOS, and Windows (MSYS2
MINGW64). The wrap is pinned in `subprojects/llama.cpp.wrap`; Meson fetches
and builds it at configure time. The CPU backend is always built (no BLAS,
OpenMP off); GPU offload is controlled by the `ai_gpu` Meson option (`auto`
by default) — Metal on macOS, Vulkan on Linux/Windows when the Vulkan SDK
and a GLSL compiler are found. `ns_ai_status_json` reports the active GPU
device and offloaded layer count to the start page footer. The official
portable / nightly packages pin `ai_gpu=disabled` (CPU-only) so a download
never hard-requires a host GPU stack; GPU offload is a source-build option
(or set `NS_PACK_AI_GPU=auto` when packaging).

The feature is controlled by the `ai` Meson option (enabled by default):

```sh
meson setup builddir -Dai=enabled   # default
meson setup builddir -Dai=disabled  # build without the local model
```

When disabled, `about:start` uses a simpler search-only page with no AI
controls or model download prompts.

## Models

Model weights are **not** committed — they are downloaded on demand into the
user data directory (`$XDG_DATA_HOME/nordstjernen/models/` on Linux, the
platform-appropriate data dir elsewhere).

The browser offers a small catalog of CPU-friendly chat models (the `k_models[]`
table in `src/ai.c`), all `Q4_K_M` GGUFs from Hugging Face:

| Tier     | Model                  | Size    |
|----------|------------------------|---------|
| Fast     | Llama 3.2 1B           | ~0.8 GB |
| Balanced | Gemma 3 4B             | ~2.5 GB |
| Quality  | Qwen3 4B Instruct 2507 | ~2.4 GB |
| Large    | Qwen3.5 9B             | ~5.4 GB |

Bigger models answer better but download more and run slower. The user picks a
tier on the start page; the loader switches between any installed models on
demand. Add or change tiers by editing `k_models[]`. Any GGUF chat model with a
built-in chat template works.

Two environment variables override the defaults:

- `NORDSTJERNEN_AI_MODEL` — an explicit path to an existing `.gguf` file;
  skips the catalog and download entirely.
- `NORDSTJERNEN_AI_MODEL_URL` — overrides the download source URL (the file is
  still saved under the selected tier's name); handy for mirrors or testing.

The config key **`ai_model_mirror`** (in `nordstjernen.conf`) rewrites the
`https://huggingface.co/` prefix of catalog downloads to a mirror host —
e.g. `ai_model_mirror = https://hf-mirror.com/` for networks where Hugging
Face is unreachable. The rest of the path (and the pinned digest check) is
unchanged, so a faithful mirror still verifies. `NORDSTJERNEN_AI_MODEL_URL`,
when set, takes precedence and bypasses both the mirror and the digest check.

## Download integrity

Downloads stream to a `<file>.part` next to the final path and **resume**
from where they left off if interrupted (HTTP range request; a server that
rejects the range restarts the transfer from scratch). Before a download
starts, the free space on the models filesystem is checked against the
expected size.

After the transfer completes the file's SHA-256 is computed. Each `k_models[]`
entry has a `sha256` field, and all four catalog models are pinned: a
mismatch discards the download and surfaces an error. If a catalog entry is
ever added with a `NULL` digest, the computed digest is written to the
log (`nordstjernen-ai: downloaded … sha256=…`) so it can be pinned — download
once from a trusted network and put the digest in the table. Downloads fetched
through `NORDSTJERNEN_AI_MODEL_URL` are never checked against the catalog
digest.

## Network behaviour of the tools

The model's web tools (DuckDuckGo search, the Wikipedia summary/image API,
and image inlining) and the model downloader all identify with the browser's
own user agent and honour the configured proxy (`ns_net_apply_curl_proxy`),
the same way page fetches do — no spoofed Chrome UA, no forged Referer.

Replies that were generated **after web content entered the prompt** (search
answers, image lookups, Wikipedia summaries) cannot trigger navigation: the
internal `@@NAVIGATE@@` marker is stripped from them, so a hostile search
snippet cannot steer the browser. Only a direct first-turn navigation intent
("go to example.org", or the model's `GO:` tool before any web content is in
play) navigates.
