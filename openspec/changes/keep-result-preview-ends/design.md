# Design

## Context

The runtime builds a preview of an offloaded result as the first `tool_result_preview_bytes` (4,096) bytes, cut at a UTF-8 boundary, followed by a note that says how many bytes follow and how to read them with `read_attachment`. The marker below replaces that note, and its offset tells `read_attachment` where the omitted part starts. When the attachment cannot be stored, it inserts the first `cap` bytes with a note. Both keep only the start.

## Decisions

### D1: Half start, half end

The preview is the first 2,048 and the last 2,048 bytes of the result, with the marker between them:

```
[... 1,843,200 bytes left out, from offset 2,048. Read them with read_attachment (attachment_id "<id>"). ...]
```

When the attachment cannot be stored, the same split is applied to the cap instead of the preview size, and the marker says that the rest could not be stored.

Alternatives considered:
- Start only (today): loses the error and summary at the end.
- End only for shell.run, start for everything else: needs a per-tool rule, and documents and JSON also end with useful parts (closing totals, last records).
- One quarter for the end: the end of shell output often holds a multi-line stack trace; half is simpler and fits it.

### D2: Cut at line breaks when near

Each cut moves to the nearest line break within 256 bytes, towards the omitted part, so that no half line is shown. Without a line break nearby (minified JSON, one long line), the cut is at a UTF-8 character boundary. A result that is only a few bytes over the preview size still gets a marker, so the model never mistakes a preview for the whole result.

Alternatives considered: cut at exact byte offsets (shows broken lines), or at token boundaries (the runtime has no tokenizer).

## Risks

- [The model treats the two parts as continuous text] -> The marker sits on its own line between them and states the number of bytes left out and the attachment id.
- [The middle holds the only relevant part] -> The attachment reference is unchanged, and the model can read any range of it.
