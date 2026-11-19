# Spec Delta

## ADDED Requirements

### Requirement: Markdown Rendering
The Thread feed SHALL render model output as Markdown (CommonMark with tables, strikethrough and task lists), also while it streams. Code blocks use a monospace font and offer Copy. Model output is untrusted: raw HTML in it is shown as text, images in it are never fetched and are shown as their alternative text with the link, and a link opens in the default browser only when the user clicks it, after the target URL is shown.

#### Scenario: Formatted answer
- **WHEN** the model answers with a heading, a bulleted list, a table and a code block
- **THEN** the feed shows them formatted, and the Markdown markup itself is not visible

#### Scenario: Remote image in output
- **WHEN** model output contains a Markdown image with a remote URL
- **THEN** no request is made to that URL, and the feed shows the alternative text and the link

#### Scenario: Raw HTML in output
- **WHEN** model output contains an HTML tag such as a script element
- **THEN** the feed shows it as literal text
