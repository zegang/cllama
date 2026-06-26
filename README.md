# CLLaMA — Local LLM Inference Server

CLLaMA is a C++17 AI inference service that wraps [llama.cpp](https://github.com/ggml-org/llama.cpp) directly, using a child-process runner architecture for model isolation. It provides an OpenAI-compatible REST API, interactive CLI, and supports Linux, macOS, Windows, iOS, and Android.

## Architecture

```
+-----------+
|   CLI     |  cllama serve|runner|model ...
+-----------+
      | HTTP
      v
+-----------+     spawns      +----------------+
|  Server   | ──────────────> | cllama_runner  |
|  (oatpp / |   fork+exec     |  (per-model)   |
|   cpprest)|                 |                |
+-----------+                 +----------------+
      |                              |
      v                              v
  RunnerMgr                    llama.cpp
  (health poll,                (inference)
   request proxy)
```

- **Server** (`cllama serve start`): REST API server, proxies inference to runners
- **Runner** (`cllama_runner`): Standalone child process, one per model, loaded via `fork+exec`
- **IPC**: localhost HTTP + NDJSON streaming, free port discovery + health polling
- **Two router implementations**: Oatpp (default) and CppRESTSDK

## Quick Start

```bash
# Prerequisites
git clone --recursive https://github.com/your/cllama.git
cd cllama

# Build
cmake --preset linux
cmake --build build -j$(nproc)
```

## Demos

### `serve start`

```
$ ./build/bin/cllama serve start -n myai --models-folder aimodelscache -d
[2026-06-26 19:11:46.102] [serve] [info] Daemonizing...
```

### `serve list`

```
$ ./build/bin/cllama serve list
PID     HOST:PORT             NAME              MODELS                              DAEMON  STARTED_AT
----------------------------------------------------------------------------------------------------------
886455  0.0.0.0:8080          myai              aimodelscacheyes                           2026-06-26T11:11:46Z
```

### `model list`

```
$ ./build/bin/cllama model list
"Qwen3.5-0.8B-IQ4_XS"
"qwen2.5-1.5b-instruct-q2_k"
```

### `runner start & list`

```
$ ./build/bin/cllama runner start -n myairunner qwen2.5-1.5b-instruct-q2_k
Runner started: myairunner
$ ./build/bin/cllama runner list
PID     PORT    TYPE        NAME                MODEL                       STARTED_AT
------------------------------------------------------------------------------------------------
886737  51567   cllama      myairunner          qwen2.5-1.5b-instruct-q2_k.gguf2026-06-26T11:12:22Z
```

### `runner chat`

```
$ ./build/bin/cllama runner chat myairunner
Chat with myairunner started. Type /bye to exit.

You: who are you
AI: I am an AI assistant. Please, ask me what to type and I can try to help.

You: how many disks on this pc
AI: The number of disks or storage on a computer can vary greatly depending on what type of computer or what operating system it is running. For example, a desktop computer might have 1 hard drive, while a server or mainframe might have many. If you're looking at a specific computer, I'd need more information to be able to give you a more accurate answer. Are you trying to install software or check your computer's specifications?

You: /bye

Goodbye!
```

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/health` | Health check |
| GET | `/v1/models` | List available GGUF models |
| GET | `/v1/runners` | List active runners |
| POST | `/v1/completions` | Text completion |
| POST | `/v1/chat/completions` | Chat completion |
| POST | `/v1/embeddings` | Generate embeddings |
| POST | `/v1/run` | Start a runner by name + model |
| POST | `/v1/stop` | Stop a runner by name |
| POST | `/v1/stop-all` | Stop all runners |

### OpenAI Compatibility

`/v1/completions`, `/v1/chat/completions`, and `/v1/embeddings` follow the OpenAI API schema. Streaming via SSE is supported.

## Chat Template Support

CLLaMA uses llama.cpp's `llama_chat_apply_template()` to format chat messages. The template is detected automatically from the GGUF model metadata (`tokenizer.chat_template`). Fallback is ChatML (`<|im_start|>`, `<|im_end|>` format). To override, pass `--chat-template <path>` to the runner directly.

Supported template families: ChatML, Llama, Mistral, Gemma, Command-R, DeepSeek, and others supported by llama.cpp.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CLLAMA_LOG_LEVEL` | `info` | Log verbosity (trace/debug/info/warn/err/critical/off) |
| `CLLAMA_LOG_FILE` | (none) | Path to rotating log file |

## Platform Support

| Platform | Process Management | Temp Dir | Daemonization | Build Preset |
|----------|-------------------|----------|---------------|--------------|
| Linux    | `/proc` + signals | `/tmp`   | fork+setsid   | `linux` |
| macOS    | sysctl + libproc  | `TMPDIR` / `/tmp` | fork+setsid | `macos` |
| Windows  | Toolhelp32 + TerminateProcess | `GetTempPathA` | (none) | `windows` |
| Android  | Same as Linux     | Same as Linux | fork+setsid | `android` |
| iOS      | Sandbox stubs     | NSTemporaryDirectory | (none) | `ios` |
