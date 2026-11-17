# Design

## Context

The client streams answers with `stream_options.include_usage`, so exact output token counts arrive only with the last chunk of each model call (`client/src/model/wire.cpp`). `ChatSink` has no usage callback. The runtime emits transient `text.delta` and `reasoning.delta` events and, after each call, a cumulative `usage` event (`client/src/runtime/runtime.cpp`). The feed builds the streaming answer item from live text (`client/src/shell/feed.cpp`) and adds a usage line only after the Run ends, hidden as a work detail (`views_feed.cpp`). Repaints of streamed text are already throttled by `RepaintTimer`.

## Goals / Non-Goals

**Goals:** a live, honest, low-cost speed indicator under every answer; exact figures once known; visible waiting time before the first token.

**Non-Goals:** benchmarking, cost, per-call detail in the default view, server-side metrics.

## Decisions

### D1. Measure per model call, aggregate per Run

For each model call the runtime records the request start, the first output token (first answer or reasoning delta), the end, and the output token count. The output token count is the provider's completion figure, which in OpenAI-compatible usage already includes reasoning tokens (`completion_tokens_details.reasoning_tokens` is a subset); task 1.2 confirms this against the gateway. Without usage, the count is estimated from the response's text, reasoning and tool-call arguments and marked not exact. A response that streamed no text or reasoning (tool calls only) records no first-token time and does not count towards the speed, because its generation time cannot be separated from the wait.

The event is stored before the answer message it produced, so the feed sees the measured call before the stored answer replaces the streamed text and never counts a call twice. Implementation: `record_model_call` in `client/src/runtime/runtime.cpp`; the retry hook restarts the measurement. It emits one stored `model.call` Run event at the end of each call: `{started_ms, first_token_ms, ended_ms, output_tokens, exact}`. `exact` is false when the provider sent no usage.

The Run's speed is total output tokens divided by total generation time, where generation time of a call is `ended - first_token`. Waiting time before the first token and Tool execution time are excluded, so a Run with long Tool calls does not look slow.

Alternatives: wall-clock speed over the whole Run (rejected: Tool time and queueing make it meaningless); show only the last call (rejected: answers composed from several calls would jump).

### D2. Live estimate from streamed text

Usage is unknown while streaming. The feed estimates output tokens from streamed answer and reasoning characters with the client's shared token estimator (`estimate_text_tokens` in `client/src/base/strings.h`, also behind the context budget). The calibration from provider prompt usage (improve-skowork-and-knowledge-control task 6.1) is not applied: it is a per-role ratio for prompts held by the runtime, and the estimate is replaced by the measured value seconds later anyway. The estimate starts at the first streamed delta's event time and ends when the call's stored model call event arrives. The estimate is shown with an approximate marker (for example "≈ 38 t/s") and is replaced by the measured value when `model.call` arrives. The rate is recomputed on each feed repaint, which the existing Live Feed Updates limit caps at 30 a second, and the line is rebuilt only when the displayed whole number changes. It is shown only after at least 1 second or 20 estimated tokens of output, to avoid wild early values.

Alternatives: ask the gateway for usage on every chunk (rejected: not supported by OpenAI-compatible streaming); count SSE chunks (rejected: chunk size varies by server).

### D3. Visible by default, outside work details

The speed line is shown under every answer of a top-level Run, also when work details are hidden. Detailed token usage (prompt, completion, model calls) remains a work detail. Child Runs show no speed line in the feed; their output tokens count towards the parent only through their own Run, not the parent's line. Before the first output token of a call the line shows the waiting time ("Waiting for the model · 4 s"), counted from the Run's latest `progress` event with phase `model` (improve-proactive-durable-work D4) and updated every second by a one-shot feed timer that is re-armed only while a line is waiting or estimating. When an earlier call of the Run already has a speed, the line shows both ("52 t/s · Waiting for the model · 3 s"). After the Run ends, the line keeps the final value; a Run that failed before any output shows no speed.

Work Details Visibility (owned by client/work-inbox, last modified by fix-feed-live-updates) lists what the feed shows by default. This change adds a requirement that names the speed line as an exception; at archive, Work Details Visibility is rebased to list it, so the two requirements do not disagree in the merged spec.

Alternatives: a Settings switch to show speed (rejected: one more setting for a cheap, unobtrusive line; can be added later); show speed only in work details (rejected: defeats the purpose of a progress cue).

### D4. Accessibility and localization

The line is plain text with a UI Automation name; it is not a live region, so screen readers are not flooded. Numbers use the user's locale. Strings: en "{n} t/s", "≈ {n} t/s", "Waiting for the model · {s} s"; cs and de equivalents.

## Performance

Per repaint the feed does one division and one integer format for each streaming answer; the estimator is already run for context budgets. On the baseline i5 this is far below 0.1 ms per update. The added stored event is about 80 bytes per model call.

## Risks / Trade-offs

- [The estimate differs from the real token count, especially for Czech or code] -> Mark it approximate, use the calibrated estimator, replace it with the measured value at the end of each call.
- [Providers that omit usage] -> Keep the estimate, marked approximate, as the final value (`exact: false`).
- [Extra repaints cost CPU on battery] -> Reuse the existing repaint throttle; the waiting counter updates once per second only while a call is in flight and the Thread is visible.
- [The line conflicts with Work Details Visibility] -> Explicit exception now, rebase at archive (D3).

## Migration Plan

Additive Run event type; older Runs have no `model.call` events and show no speed line. No schema migration beyond the existing event store.
