# Design

## Context

The ideas come from the CUI component library (github.com/yappologistic/CUI, MIT): its streaming Markdown, reasoning and tool-call blocks, agent plan, command palette, context meter and small interaction rules. CUI is immediate mode, draws with its own renderer and has no accessibility tree, so none of its code fits the native UI (add-native-ui: retained `Node` tree, Direct2D, DirectWrite, UI Automation). This change re-implements the behaviours in the existing toolkit.

What exists today:
- `MarkdownView::set_markdown` reparses the whole streamed text with md4c on every repaint (`client/src/ui/richtext.cpp`); repaints are limited to 30 a second (Live Feed Updates).
- The feed fold (`client/src/shell/feed.cpp`) has tool call and tool result events with millisecond timestamps, the live reasoning text and `LiveText::output_since`; the feed view (`views_feed.cpp`) has a one-shot one-second timer for the speed line (show-generation-speed D3).
- The runtime's `ChatSink` sees each reasoning delta (`runtime.cpp`, `model_turn`) and stores reasoning as `{"text"}`.
- Core runtime tools are built in `runtime.cpp` (`make_ask_user` and siblings) and listed in the control plane's built-in catalogue (`control-plane/src/policy/tool-catalog.ts`), which a test keeps equal to the client's registered ids.
- `threads.context` returns `tokens`, `input_budget`, `tool_tokens`; the status bar draws a ring (`ContextMeter`, `views_frame.cpp`).
- `Ui::layout` → `TextCache::get` creates every wrapped DirectWrite layout outside message text (`canvas.cpp`).
- `Avatar` takes its fill from a `Paint` token.

## Goals / Non-Goals

**Goals:** steadier streaming, visible timing, a visible plan, a quick keyboard route to everything, a context indicator that explains itself, and the small rules; all keyboard-operable and named for screen readers; no new idle CPU.

**Non-Goals:** animation, CUI code, voting or regenerate, prompt suggestions, plans of Child Runs in the feed.

## Decisions

### D1. Close unfinished inline Markdown before parsing a stream

A pure function `close_streaming_markdown(text)` in `client/src/ui/markdown.cpp` looks at the last paragraph of streamed text (after the last blank line, outside a fenced code block) and appends what closes it: a backtick run for an open code span; `~~`, `**`, `*` or `_` for open emphasis, innermost first; an unfinished link `[text](partial` or `[text` is cut back to its text. Emphasis markers inside code spans and backslash escapes are skipped; `_` counts only at a word boundary, as in CommonMark. An unclosed fenced code block is left as is: md4c already runs it to the end. `MarkdownView::set_streaming_markdown` applies it before `parse_markdown`; the stored answer is parsed unchanged, so the final rendering is exactly CommonMark.

Alternatives: patch md4c to treat end of input as closing (rejected: vendored code, and its delimiter matching is the part that would need it); hide the last partial line until it ends (rejected: visible delay and still flicker at line ends); do nothing (the problem stays).

### D2. Durations from stored timestamps; reasoning duration from the runtime

- Tool cards: duration = `tool_result.at − tool_call.at`; while no result, now − `tool_call.at`. Shown as "0.4 s" with one decimal under 10 s, else `format_elapsed`. The header's accessible name includes it. A running card's time is refreshed by the existing one-second timer only while that card is visible; a call of a Run that is no longer active is not running.
- Reasoning: the runtime records the times of the first and the last reasoning delta of the attempt that produced the response and stores `duration_ms` with the reasoning event (agent-runtime Reasoning Duration). The live block counts from `LiveText::reasoning_since`, set by the first reasoning delta of a stream (when no live reasoning is held). Times under one second are not shown, so a block never says "Thought for 0 s".
- Step elapsed time: the current step starts at the newest unfinished tool call's `at`, or at `reasoning_since` while reasoning streams; the status line shows " · N s" from 2 s. The feed view's one-second timer also runs while such a line is shown.
- JSON results: when a result's content parses as a JSON object or array and is under 256 KB, it is shown with `dump(2)`.

Alternatives: measure tool time in the runtime and store it (rejected: event timestamps already give it to the millisecond); reasoning duration from the model call's first-token time (rejected: the answer text streams in the same call, so it overstates); a live seconds counter as a UI Automation live region (rejected: floods screen readers; the name is read on demand).

### D3. Command palette as a modal overlay with a pure ranking function

`CommandPalette` (`client/src/shell/views_palette.cpp`) is a `ui::Box` shown on the overlay layer at the top centre of the window: a `TextField` and a list of buttons (role list item), at most 50, the selected one marked with the selection colour. Items are built when it opens: commands (New Thread, Search, Toggle Activity rail, Settings, Shortcut help, Inbox views, Agenda and Calendar when enabled, Knowledge, Apps, Status, About) and Inbox entries from the shell's loaded list. Ranking is a pure function `rank_palette(items, query)` in `client/src/shell/palette.cpp`: fold case and accents (Latin-1/Latin Extended-A to ASCII, enough for cs and de), then prefix (0) < word start (1) < subsequence (2), then commands before entries, then the shorter name, then the original order. Ctrl+K is a new `Command::palette` in `shortcuts.cpp`, also handled while typing because it has Ctrl. The result count is announced after each change of the query.

Alternatives: extend the Ctrl+F search panel (rejected: it searches message text through the host; mixing commands in would confuse both); a scored fuzzy matcher like fzf (rejected: needs tuning, and three ranks are predictable and testable); a modal dialog (rejected: dims the window; the overlay closes on an outside click like a menu).

### D4. `update_plan` as a reserved core tool; the plan is the latest call's arguments

The tool is built with `make_update_plan()` beside `make_ask_user`, id constant `kUpdatePlan`, `core = true`, `own_data = true`, risk read, replay-safe (added to `recovery_of`, `reserved_id` and `replay_safe_tool`), and unavailable to Child Runs in `available()`. Its `run` validates the steps (1–20, title 1–200 characters, status enum, at most one running) and returns "Plan updated: N steps, M done." The Run plan is not stored separately: the feed folds the stored `tool_call` events of `update_plan` with an ok result; the latest one is the Run plan. The plan item is keyed by the Run (`run:plan`) and placed at the first ok call, so it updates in place (Live Feed Updates keeps its element while the signature is unchanged, and rebuilds only it otherwise). Its view `PlanCard` is a box with a header ("2 of 5 done") and a list; each step has a mark drawn as a small circle (empty: pending, filled accent ring: running, check: done, cross: failed, dash: skipped), its title, and its status word in secondary text. When the Run is not active, pending and running are shown as "Not done". A system prompt sentence asks for a plan for work of three or more steps. The control-plane built-in catalogue gains `update_plan` (owner client/agent-runtime, read), and its test recognises the new constant.

Alternatives: keep the plan in a Run column or a dedicated event (rejected: the tool call is already stored, replayed and shown in work details; a second copy can disagree); let the client infer a plan from tool calls (rejected: not a plan); a plan per Child Run in the feed (rejected: the Activity rail owns Child Runs); offering it to Child Runs (rejected: no place shows it).

### D5. Context parts from the runtime

`ContextUsage` gains `instructions_tokens` (system prompt, run context and the generated-surface and Skill text), `summary_tokens` and `conversation_tokens`, computed with the same estimator and calibration as `tokens`; `threads.context` returns them as `parts`. The status bar's help text and accessible name list instructions, tool definitions, conversation (with the summary) and free, as percentages of the input budget that add up to 100 (the largest remainder takes the rounding). The ring uses `Paint::error` from 90 %, warning from 75 %.

Alternatives: show the breakdown as a popover with bars (rejected: a tooltip and the name carry it with no new control); thresholds 80/95 as in CUI (rejected: Skowork already warns at 75 and compacts automatically near the trigger; 90 marks "compaction is imminent").

### D6. Small rules

- Composer: `rebuild_bar` adds "draft empty" to its state; Send, Queue and Steer are disabled when the draft is blank with no attachments, and Stop becomes `Kind::primary` while a Run is active and they are disabled. `text_changed` and attachment changes call `rebuild_bar` (it returns early when its state string is unchanged).
- Button feedback: `ui::Button::flash(glyph, label, ms)` swaps the glyph and the label (and so the accessible name), widening an icon button to fit, and restores them after 1.5 s through a new `Ui::after` one-shot timer that the window provides; a liveness token stops a late timer after the button is gone. Message Copy, Chat message Copy and code-block Copy use it ("Copied", or "Copy failed"), and still announce.
- Avatars: eight `Paint::avatar_0..7` tokens in the theme, light tints in the light theme and deep tones in the dark theme, so text (`Paint::text`) on them is ≥ 4.5:1 (checked by the palette contrast test). `Avatar::paint_for(name)` hashes the user name (the sign-in name for Chats) with FNV-1a, case-folded. High contrast maps them to the system window colour.
- Balanced wrapping: in `TextCache::get`, for a wrapped layout of text without line breaks inside the last paragraph whose metrics show ≥ 2 lines and whose last line holds one word, the cache tries up to four narrower max widths (each 6 % narrower) and keeps the first whose line count is unchanged and whose last line holds ≥ 2 words. The cache key stays the requested width, so the result is cached once. Message text (`RichText`, `MarkdownView`) does not go through this path.

Alternatives: animations for the Copy state (rejected: no animation infrastructure, nothing else animates); avatar colours from the theme's accent shades (rejected: too similar to tell apart); balancing every line like CSS `text-wrap: balance` (rejected: changes all layouts and costs more; only the orphan is the problem).

## Performance

On the baseline i5:
- D1: one linear scan of the last paragraph per repaint, at most 30 a second; under 0.05 ms for a 10 KB paragraph.
- D2: subtraction of stored timestamps; the timer ticks once a second only while a running card, a counting status line or the speed line is visible, as before.
- D3: ranking 2,000 items with a case-folded subsequence test is under 1 ms per keystroke.
- D4: one small tool result per plan update; the plan card is rebuilt only when its steps change.
- D5: three more estimates per `threads.context` call, which already estimates the whole conversation.
- D6: balanced wrapping creates at most four extra DirectWrite layouts for a text that has an orphan, once per text and width thanks to the cache; text without an orphan costs one `GetLineMetrics` call.

## Risks / Trade-offs

- [D1 closes markup the model meant literally, such as a lone `*` in "5 * 3"] -> Only at the end of the streamed paragraph and only while streaming; a `*` followed by a space never opens emphasis in CommonMark, so it is not closed; the stored answer is rendered as written.
- [The model ignores `update_plan` or overuses it for trivial work] -> One prompt sentence with the three-step threshold; the tool is cheap and the card small; evaluation export shows its use.
- [A plan claims steps that the tool calls do not show] -> The card shows what the agent reports; work details still show every call.
- [Adding a core tool changes the cached prompt prefix and the baseline schema budget] -> The description is one sentence and the schema small; the baseline budget test is rerun; the prefix changes once with this release.
- [Ctrl+K is used by a text field for something else] -> Rich edit has no Ctrl+K binding; the composer keeps its draft.
- [Balanced wrapping changes golden screenshots] -> Goldens are reviewed and refreshed with this change.
- [Avatar colours collide for two people] -> Eight colours plus initials; colour is never the only cue.

## Migration Plan

None. Older Runs have reasoning without `duration_ms` (shown as "Reasoning") and no Run plans. Threads started before the update see `update_plan` from their next Run.

## Open Questions

None.
