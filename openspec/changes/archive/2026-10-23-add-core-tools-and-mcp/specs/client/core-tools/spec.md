# Spec Delta

## Purpose

Defines the general-purpose tools the agent uses to work with local files, run shell commands, fetch web content and download files, all within the limits set by policy.

## ADDED Requirements

### Requirement: File Read Tools
The client SHALL provide these read-class tools:
- files.read reads a text file, optionally a line range;
- files.list lists a directory;
- files.glob matches paths by pattern;
- files.grep searches file contents by regular expression.

Each tool only returns paths that client/policy-enforcement permits after canonicalisation: matched by policy.filesystem.read and not matched by policy.filesystem.deny.

#### Scenario: Read permitted file
- **WHEN** the agent calls files.read on a file under a folder listed in policy.filesystem.read
- **THEN** the file content is returned, with line numbers if a range was requested

#### Scenario: Denied path filtered from results
- **WHEN** files.glob matches files in a folder matched by policy.filesystem.deny
- **THEN** those files are omitted from the result, and the omission count is reported

### Requirement: Document Text Extraction
The client SHALL extract readable text from docx, xlsx, pptx and pdf files when files.read or web.fetch returns them, and when client/mail-calendar reads mail attachments. Extraction MUST run in a worker process under client/tool-sandbox, because document parsers handle untrusted input. client/knowledge-graph uses the same extraction for indexing.

#### Scenario: Read a Word document
- **WHEN** the agent calls files.read on a permitted docx file
- **THEN** the result is the document's text with headings and tables preserved as plain-text structure, and the extraction ran in a sandboxed worker

#### Scenario: Malformed document
- **WHEN** a pdf file crashes or hangs the extractor
- **THEN** the worker is terminated, the tool returns an extraction error, and the client core keeps running

#### Scenario: Mail attachment
- **WHEN** mail.read returns a message with a pdf attachment
- **THEN** the attachment text comes from the same sandboxed extraction

### Requirement: File Write Tools
The client SHALL provide files.write, which creates or overwrites a file, and files.edit, which replaces an exact text span. Both are write-local tools. They apply only to paths permitted by policy.filesystem.write. files.edit MUST fail when the text span is not found exactly once.

#### Scenario: Ambiguous edit
- **WHEN** files.edit is called with a text span that occurs twice in the file
- **THEN** the call fails with an "ambiguous match" error and the file is unchanged

#### Scenario: Write outside permitted folders
- **WHEN** files.write targets a path not covered by policy.filesystem.write
- **THEN** the call is denied and the file is not created

### Requirement: Path Evaluation By Policy Enforcement
Every file tool SHALL pass each path argument to client/policy-enforcement, and SHALL access only the canonical path that enforcement evaluated. File tools MUST NOT implement their own path resolution rules.

#### Scenario: Junction escape
- **WHEN** a permitted folder contains a junction pointing to a denied folder and the agent reads through it
- **THEN** client/policy-enforcement evaluates the junction target, the call is denied, and no data is read

### Requirement: File Size And Type Limits
File read tools SHALL refuse binary files, except the document types handled by Document Text Extraction. They SHALL truncate text results above policy.tools.files.max_read_kb (default 256 KB), and indicate the truncation and how to request further ranges.

#### Scenario: Binary file
- **WHEN** the agent calls files.read on an executable file
- **THEN** the call fails with a "binary file not supported" error and no content is returned

#### Scenario: Large log file
- **WHEN** the agent reads a 5 MB log file
- **THEN** the first 256 KB are returned with a truncation notice stating the total size and how to read further ranges

### Requirement: Shell Tool
The client SHALL provide shell.run, which executes a command in the shell named by policy.tools.shell.executable (powershell = Windows PowerShell 5.1, or pwsh; default powershell). It takes a working directory and a timeout argument (default 120 seconds, maximum 30 minutes), and returns the exit code, stdout and stderr. The command runs in a process created by client/tool-sandbox, which owns policy.tools.shell.mode, the limits, and the rule that the smaller timeout wins.

#### Scenario: Shell disabled
- **WHEN** client/tool-sandbox reports that policy.tools.shell.mode is off
- **THEN** shell.run is not offered to the model

#### Scenario: Timeout
- **WHEN** a command exceeds its effective timeout as determined by client/tool-sandbox
- **THEN** the process tree is terminated and the result reports the timeout and its source, together with the output captured so far

### Requirement: Shell Risk Classification
shell.run SHALL declare risk class write-local by default. It SHALL declare destructive when the command matches policy.tools.shell.destructive_patterns. The default patterns cover recursive deletion, disk formatting, registry deletion and changes to user or security settings, and policy can only add to them.

#### Scenario: Destructive command
- **WHEN** the agent calls shell.run with a command that matches a destructive pattern
- **THEN** the call declares risk class destructive, and client/policy-enforcement decides it accordingly

### Requirement: Web Fetch Tool
The client SHALL provide web.fetch, risk class read. It retrieves a URL over HTTP or HTTPS with GET and returns readable text, or a byte-limited raw body. Every request and redirect is checked by the network egress rules of client/policy-enforcement. Fetched content is untrusted, and client/agent-runtime records its URL as provenance.

#### Scenario: Host not permitted
- **WHEN** the agent calls web.fetch for a host outside policy.network.egress_allow
- **THEN** the call is denied without any network request being made

#### Scenario: Provenance recorded
- **WHEN** web.fetch returns content
- **THEN** the content enters the context marked as untrusted with the fetched URL as its provenance

### Requirement: Intranet Authentication For Fetch
web.fetch SHALL authenticate to intranet hosts listed in policy.network.integrated_auth_hosts (host patterns; default empty) using the user's Kerberos credentials, and MUST NOT send integrated credentials to any other host.

#### Scenario: Intranet wiki
- **WHEN** the agent fetches a page from an intranet host listed in policy.network.integrated_auth_hosts
- **THEN** the request authenticates with Kerberos and the page is returned

### Requirement: Managed Downloads
Files downloaded by any tool SHALL be stored in a quarantine folder under the data root, with the Windows mark-of-the-web set. They MUST NOT be executed or opened by the agent until the user moves them out or approves the action.

#### Scenario: Downloaded installer
- **WHEN** a tool downloads an executable file
- **THEN** it is stored in the quarantine folder with mark-of-the-web, and any attempt by the agent to run it is denied unless approved by the user

### Requirement: Complete Results To Runtime
Every core tool SHALL return its complete result to client/agent-runtime, which applies the tool-result cap and offloading. Core tools MUST NOT apply their own model-context caps. The only exceptions are the file read limit above and the output capture limit of client/tool-sandbox.

#### Scenario: Large command output
- **WHEN** shell.run captures 1 MB of standard output, which is the default capture limit of client/tool-sandbox
- **THEN** the full captured output reaches client/agent-runtime, which stores it and passes the model only the capped preview it defines

### Requirement: Declared Risk Classes
Core tools SHALL declare these Risk classes:
- read: files.read, files.list, files.glob, files.grep, web.fetch;
- write-local: files.write, files.edit, and shell.run unless it is classified destructive.

#### Scenario: Write declares write-local
- **WHEN** the agent calls files.edit
- **THEN** the call declares risk class write-local to client/policy-enforcement
