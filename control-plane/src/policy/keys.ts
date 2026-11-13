// Policy keys of every capability (control-plane/policy-service Policy Schema Ownership; design D9, D10).
// Generated from the capability specs; each entry names its owning spec. Keep it in step with the specs.
import type { KeyDef } from './schema.ts';

export const SCHEMA_VERSION = 1;

/** A host pattern of the network lists: a host name or address with * and ? wildcards, no scheme, path or spaces. */
const HOST_PATTERN = '^[A-Za-z0-9*?._:\\[\\]-]+$';

export const KEYS: KeyDef[] = [
  // specs: add-agent-runtime/specs/client/agent-runtime/spec.md
  { path: 'llm.max_concurrent_runs', owner: 'client/agent-runtime', description: 'Maximum simultaneous attended and unattended Runs; interrupted Runs do not occupy a slot.', type: { kind: 'integer', min: 1, max: 64 }, default: 3, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.max_parallel_tools', owner: 'client/agent-runtime', description: 'Maximum tool calls a Run may execute in parallel.', type: { kind: 'integer', min: 1 }, default: 4, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.max_tool_result_kb', owner: 'client/agent-runtime', description: 'Maximum tool-result size inserted directly into a conversation; larger results become attachments with a preview.', type: { kind: 'integer', min: 1 }, default: 64, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.attended.model_calls', owner: 'client/agent-runtime', description: 'Maximum model calls per attended Run.', type: { kind: 'integer', min: 1 }, default: 60, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.attended.tool_calls', owner: 'client/agent-runtime', description: 'Maximum tool calls per attended Run.', type: { kind: 'integer', min: 1 }, default: 200, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.attended.wall_minutes', owner: 'client/agent-runtime', description: 'Maximum active duration in minutes per attended Run; interrupted time is excluded.', type: { kind: 'integer', min: 1 }, default: 60, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.unattended.model_calls', owner: 'client/agent-runtime', description: 'Maximum model calls per unattended Run.', type: { kind: 'integer', min: 1 }, default: 30, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.unattended.tool_calls', owner: 'client/agent-runtime', description: 'Maximum tool calls per unattended Run.', type: { kind: 'integer', min: 1 }, default: 80, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.unattended.wall_minutes', owner: 'client/agent-runtime', description: 'Maximum active duration in minutes per unattended Run; interrupted time is excluded.', type: { kind: 'integer', min: 1 }, default: 30, merge: 'scalar', guardrail: 'cap' },

  // specs: add-agent-runtime/specs/client/model-access/spec.md
  { path: 'llm.max_images_per_request', owner: 'client/model-access', description: 'Maximum images included in one model request.', type: { kind: 'integer', min: 1 }, default: 4, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.timeouts.connect_seconds', owner: 'client/model-access', description: 'Connection timeout in seconds for model requests.', type: { kind: 'integer', min: 1 }, default: 10, merge: 'scalar' },
  { path: 'llm.timeouts.idle_seconds', owner: 'client/model-access', description: 'Maximum idle time in seconds between received model-response data.', type: { kind: 'integer', min: 1 }, default: 60, merge: 'scalar' },

  // specs: add-agent-runtime/specs/control-plane/llm-gateway/spec.md
  { path: 'llm.models', owner: 'control-plane/llm-gateway', description: 'Model mapping for each gateway role, including main, vision, small, embedding, reranking and image generation.', type: { kind: 'map', value: { kind: 'string' }, keys: ['main', 'vision', 'small', 'embed', 'rerank', 'image'] }, default: { main: 'acme-llm-primary', vision: 'acme-llm-primary', small: 'acme-llm-secondary', embed: 'acme-llm-embedding', rerank: 'acme-llm-reranker', image: 'acme-llm-image' }, merge: 'whole-map', guardrail: 'force-entries' },
  { path: 'llm.budget.tokens', owner: 'control-plane/llm-gateway', description: 'Token budget applied by the gateway for the configured budget period.', type: { kind: 'integer', min: 0 }, default: 1000000, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.budget.period', owner: 'control-plane/llm-gateway', description: 'Period over which the gateway token budget is measured.', type: { kind: 'enum', values: ['day', 'week', 'month'] }, default: 'day', merge: 'scalar' },
  { path: 'llm.budget.embed_daily_tokens', owner: 'control-plane/llm-gateway', description: 'Daily gateway token budget for embedding requests.', type: { kind: 'integer', min: 0 }, default: 12000000, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.budget.images_daily', owner: 'control-plane/llm-gateway', description: 'Daily gateway limit for generated images.', type: { kind: 'integer', min: 0 }, default: 50, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.rate_limits.requests_per_minute', owner: 'control-plane/llm-gateway', description: 'Gateway request rate limit per minute.', type: { kind: 'integer', min: 1 }, default: 60, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.rate_limits.concurrent_requests', owner: 'control-plane/llm-gateway', description: 'Maximum concurrent gateway requests.', type: { kind: 'integer', min: 1 }, default: 4, merge: 'scalar', guardrail: 'cap' },

  // specs: add-policy-and-sandbox/specs/client/policy-enforcement/spec.md
  { path: 'tools.rules', owner: 'client/policy-enforcement', description: 'Set allow, approve or deny for individual tools or tool patterns. A matching rule takes precedence over the risk-class default.', type: { kind: 'decision-map' }, default: {}, merge: 'decision-map', guardrail: 'stricter' },
  { path: 'tools.risk', owner: 'client/policy-enforcement', description: 'Choose the fallback decision for each risk class when no tool rule matches.', type: { kind: 'decision-map', keys: ['read', 'write-local', 'external', 'destructive'] }, default: { read: 'allow', 'write-local': 'approve', external: 'approve', destructive: 'deny' }, merge: 'decision-map', guardrail: 'stricter' },
  { path: 'unattended.allowed_risk', owner: 'client/policy-enforcement', description: 'Risk classes that may run unattended without an additional approval solely because of their risk class.', type: { kind: 'list', item: { kind: 'enum', values: ['read', 'write-local', 'external', 'destructive'] } }, default: ['read', 'write-local'], merge: 'union', guardrail: 'restrict' },
  { path: 'unattended.rules', owner: 'client/policy-enforcement', description: 'Additional decisions for tool patterns in unattended runs. An explicit allow can lift an unattended risk or tool approval floor where the tool permits it.', type: { kind: 'decision-map' }, default: {}, merge: 'decision-map', guardrail: 'stricter' },
  { path: 'filesystem.read', owner: 'client/policy-enforcement', description: 'Path patterns the agent may read, subject to deny rules and protected locations.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },
  { path: 'filesystem.write', owner: 'client/policy-enforcement', description: 'Path patterns the agent may write, subject to deny rules and protected locations.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },
  { path: 'filesystem.deny', owner: 'client/policy-enforcement', description: 'Path patterns denied to the agent even when another rule allows access.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'filesystem.grantable', owner: 'client/policy-enforcement', description: 'Path patterns within which users may grant additional agent filesystem access.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },
  { path: 'network.egress_allow', owner: 'client/policy-enforcement', description: 'Host patterns the agent and its sandboxed tools may contact; unmatched requests are denied.', type: { kind: 'list', item: { kind: 'string', pattern: HOST_PATTERN } }, default: [], merge: 'union' },
  { path: 'network.egress_deny', owner: 'client/policy-enforcement', description: 'Host patterns the agent and its sandboxed tools may not contact, including after redirects.', type: { kind: 'list', item: { kind: 'string', pattern: HOST_PATTERN } }, default: [], merge: 'union', guardrail: 'add' },

  // specs: add-policy-and-sandbox/specs/client/tool-sandbox/spec.md
  { path: 'tools.shell.mode', owner: 'client/tool-sandbox', description: 'Choose whether shell execution is off, sandboxed, or allowed by policy.', type: { kind: 'enum', values: ['off', 'sandboxed', 'allow'] }, default: 'off', merge: 'scalar', guardrail: 'max-level' },
  { path: 'tools.shell.limits.memory_mb', owner: 'client/tool-sandbox', description: 'Maximum memory in megabytes for the shell tool process.', type: { kind: 'integer', min: 1 }, default: 1024, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.shell.limits.cpu_percent', owner: 'client/tool-sandbox', description: 'Maximum CPU percentage for the shell tool process.', type: { kind: 'integer', min: 1, max: 100 }, default: 50, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.shell.limits.processes', owner: 'client/tool-sandbox', description: 'Maximum number of processes in the shell tool job.', type: { kind: 'integer', min: 1 }, default: 32, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.shell.limits.wall_minutes', owner: 'client/tool-sandbox', description: 'Maximum wall-clock duration in minutes for a shell tool call.', type: { kind: 'integer', min: 1 }, default: 15, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.shell.limits.output_kb', owner: 'client/tool-sandbox', description: 'Maximum shell output size in kilobytes.', type: { kind: 'integer', min: 1 }, default: 1024, merge: 'scalar', guardrail: 'cap' },

  // specs: add-core-tools-and-mcp/specs/client/core-tools/spec.md (and control-plane/mcp-catalog for mcp.* below)
  { path: 'tools.shell.executable', owner: 'client/core-tools', description: 'Select the PowerShell executable used by the shell tool.', type: { kind: 'enum', values: ['powershell', 'pwsh'] }, default: 'powershell', merge: 'scalar' },
  { path: 'tools.shell.destructive_patterns', owner: 'client/core-tools', description: 'Command patterns classified as destructive by the shell tool.', type: { kind: 'list', item: { kind: 'string' } }, merge: 'union', guardrail: 'add' },
  { path: 'tools.files.max_read_kb', owner: 'client/core-tools', description: 'Maximum file content size in kilobytes returned by a file read tool call.', type: { kind: 'integer', min: 1 }, default: 256, merge: 'scalar', guardrail: 'cap' },

  // specs: add-work-inbox/specs/client/work-inbox/spec.md
  { path: 'notifications.completed', owner: 'client/work-inbox', description: 'Show a notification when a Run completes.', type: { kind: 'boolean' }, default: true, merge: 'scalar' },
  { path: 'notifications.failed', owner: 'client/work-inbox', description: 'Show a notification when a Run fails.', type: { kind: 'boolean' }, default: true, merge: 'scalar' },
  { path: 'notifications.action', owner: 'client/work-inbox', description: 'Show a notification when user action is needed.', type: { kind: 'boolean' }, default: true, merge: 'scalar' },

  // specs: add-core-tools-and-mcp/specs/client/mcp-client/spec.md
  { path: 'mcp.idle_timeout_minutes', owner: 'client/mcp-client', description: 'Idle time in minutes before an MCP client connection is closed.', type: { kind: 'integer', min: 1 }, default: 10, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.mcp.limits.memory_mb', owner: 'client/mcp-client', description: 'Maximum memory in megabytes for a local MCP server process.', type: { kind: 'integer', min: 1 }, default: 1024, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.mcp.limits.cpu_percent', owner: 'client/mcp-client', description: 'Maximum CPU percentage for a local MCP server process.', type: { kind: 'integer', min: 1, max: 100 }, default: 50, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.mcp.limits.processes', owner: 'client/mcp-client', description: 'Maximum number of processes in a local MCP server job.', type: { kind: 'integer', min: 1 }, default: 32, merge: 'scalar', guardrail: 'cap' },

  // specs: add-computer-use/specs/client/computer-use/spec.md
  { path: 'tools.computer.mode', owner: 'client/computer-use', description: 'Enable computer-use tools such as click, type and screenshot. When off, these tools are not offered.', type: { kind: 'enum', values: ['off', 'allow'] }, default: 'off', merge: 'scalar', guardrail: 'max-level' },
  { path: 'tools.computer.perception', owner: 'client/computer-use', description: 'Choose accessibility-tree-only interaction or allow both the tree and visual screenshots. Tree-only mode also disallows coordinate actions.', type: { kind: 'enum', values: ['tree_only', 'tree_and_vision'] }, default: 'tree_and_vision', merge: 'scalar', guardrail: 'max-level' },
  { path: 'tools.computer.stop_hotkey', owner: 'client/computer-use', description: 'Global keyboard shortcut that stops computer actions and cancels the controlling run. It requires at least two modifiers.', type: { kind: 'string' }, default: 'Ctrl+Alt+End', merge: 'scalar' },
  { path: 'tools.computer.app_allowlist', owner: 'client/computer-use', description: 'Executable names that computer use may target. An empty list allows applications unless denied elsewhere.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },
  { path: 'tools.computer.app_denylist', owner: 'client/computer-use', description: 'Executable names that computer use may never target. Protected application and security windows are also denied.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'tools.computer.retain_screenshots', owner: 'client/computer-use', description: 'Allow screenshots captured by computer use to be retained as Thread attachments; otherwise they are kept only in memory during the run.', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },

  // specs: add-browser-use/specs/client/browser-use/spec.md
  { path: 'tools.browser.retain_screenshots', owner: 'client/browser-use', description: 'Allow browser screenshots to be retained in the data root after the Run.', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'tools.browser.allow_eval', owner: 'client/browser-use', description: 'Offer the destructive browser.eval tool for running JavaScript in a page.', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'tools.browser.headless_unattended', owner: 'client/browser-use', description: 'Allow the agent browser to run without a visible window during unattended Runs.', type: { kind: 'boolean' }, default: false, merge: 'scalar' },
  { path: 'tools.browser.idle_timeout_minutes', owner: 'client/browser-use', description: 'Minutes without browser tool calls before the agent browser closes; it stays open while the user interacts.', type: { kind: 'integer', min: 1 }, default: 15, merge: 'scalar' },

  // specs: add-skills/specs/client/skills/spec.md
  { path: 'skills.locked', owner: 'client/skills', description: 'Organization Skill names that cannot be shadowed by higher-precedence Skills.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'skills.publish', owner: 'client/skills', description: 'Scopes to which users may publish Skills; private Skills remain available.', type: { kind: 'list', item: { kind: 'enum', values: ['shared', 'group', 'org'] } }, default: ['shared'], merge: 'union', guardrail: 'restrict' },
  { path: 'skills.install_from', owner: 'client/skills', description: 'Skill registry sources from which users may install Skills.', type: { kind: 'list', item: { kind: 'string', pattern: '^(org|group|shared|group:.+)$' } }, default: ['org', 'group', 'shared'], merge: 'union', guardrail: 'restrict' },
  { path: 'skills.script_runtimes', owner: 'client/skills', description: 'Runtime names allowed for scripts invoked by Skills through the shell tool.', type: { kind: 'list', item: { kind: 'string' } }, default: ['powershell'], merge: 'union' },
  { path: 'skills.auto_update', owner: 'client/skills', description: 'Automatically update installed Skills when updates are available.', type: { kind: 'boolean' }, default: true, merge: 'scalar' },

  // specs: add-knowledge-graph/specs/client/knowledge-graph/spec.md (on|off strings, D9)
  { path: 'knowledge.sources.threads', owner: 'client/knowledge-graph', description: 'Allow knowledge indexing from Threads.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.sources.files', owner: 'client/knowledge-graph', description: 'Allow knowledge indexing from user-selected files and folders that filesystem policy permits.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.sources.mail', owner: 'client/knowledge-graph', description: 'Allow knowledge indexing from permitted mail.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.sources.calendar', owner: 'client/knowledge-graph', description: 'Allow knowledge indexing from permitted calendar items.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  // specs: add-knowledge-sharing/specs/client/knowledge-graph/spec.md (Knowledge Notes, Sharing Knowledge Items)
  { path: 'knowledge.sources.notes', owner: 'client/knowledge-graph', description: 'Allow knowledge indexing from notes.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.sharing.groups', owner: 'client/knowledge-graph', description: 'Groups with which users may choose to share eligible documents and notes. An empty list disables sharing; * permits the user’s Groups.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'restrict' },
  { path: 'knowledge.sharing.derived_sources', owner: 'client/knowledge-graph', description: 'Source kinds whose derived knowledge users may publish to their Groups. Every Source a published statement depends on must be one of them.', type: { kind: 'list', item: { kind: 'enum', values: ['threads', 'files', 'mail', 'calendar', 'meetings', 'notes'] } }, default: ['files', 'notes'], merge: 'union', guardrail: 'restrict' },
  { path: 'knowledge.categorization', owner: 'client/knowledge-graph', description: 'Allow automatic assignment of existing categories to extracted knowledge. Manual categories remain available when off.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.consistency', owner: 'client/knowledge-graph', description: 'Allow automatic checks of new or changed knowledge for possible inconsistencies with existing knowledge.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.sources.meetings', owner: 'client/knowledge-graph', description: 'Allow knowledge indexing from meeting transcripts. Meeting capture has separate controls.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.extraction', owner: 'client/knowledge-graph', description: 'Allow model-based extraction of entities and relations from indexed passages. Search indexing remains available when extraction is off.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.directory_enrichment', owner: 'client/knowledge-graph', description: 'Allow internal people found in knowledge to be enriched from the Microsoft Graph user directory.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.max_file_mb', owner: 'client/knowledge-graph', description: 'Skip files larger than this many megabytes during knowledge indexing.', type: { kind: 'integer', min: 1 }, default: 25, merge: 'scalar', guardrail: 'cap' },
  { path: 'knowledge.embedding_daily_tokens', owner: 'client/knowledge-graph', description: 'Daily per-user token budget for background knowledge embeddings. Query embeddings are separate.', type: { kind: 'integer', min: 0 }, default: 10000000, merge: 'scalar', guardrail: 'cap' },
  { path: 'knowledge.extraction_daily_calls', owner: 'client/knowledge-graph', description: 'Daily per-user limit on model calls for knowledge extraction.', type: { kind: 'integer', min: 0 }, default: 2000, merge: 'scalar', guardrail: 'cap' },
  { path: 'knowledge.max_threads', owner: 'client/knowledge-graph', description: 'Maximum background threads for knowledge indexing and extraction.', type: { kind: 'integer', min: 1 }, default: 2, merge: 'scalar', guardrail: 'cap' },
  { path: 'knowledge.exclusions', owner: 'client/knowledge-graph', description: 'Path or file-type glob patterns excluded from knowledge indexing.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  // specs: add-save-as-note/specs/client/knowledge-graph/spec.md (Save As Note)
  { path: 'knowledge.save_as_note', owner: 'client/knowledge-graph', description: 'Allow users to open a note draft from indexed mail or selected Chat messages.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },

  // specs: add-meeting-capture/specs/client/meeting-capture/spec.md
  { path: 'meeting_capture.enabled', owner: 'client/meeting-capture', description: 'Enable meeting capture for the user. When disabled, no capture stream is opened.', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.apps', owner: 'client/meeting-capture', description: 'Applications used to detect meetings: process names may include a window-title pattern after |.', type: { kind: 'list', item: { kind: 'string' } }, merge: 'union' },
  { path: 'meeting_capture.allow_manual', owner: 'client/meeting-capture', description: 'Allow the user to start capture manually from an application, system audio or microphone.', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.allow_series_auto', owner: 'client/meeting-capture', description: 'Allow users to opt in to automatic capture for a meeting series. Required participant notices still apply.', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.live_transcript', owner: 'client/meeting-capture', description: 'Allow transcription during capture instead of waiting until capture ends.', type: { kind: 'boolean' }, default: false, merge: 'scalar' },
  { path: 'meeting_capture.keep_audio', owner: 'client/meeting-capture', description: 'Allow raw meeting audio to be retained after transcription.', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.max_pending_days', owner: 'client/meeting-capture', description: 'Maximum age in days of audio awaiting transcription before it is deleted.', type: { kind: 'integer', min: 1 }, default: 7, merge: 'scalar', guardrail: 'cap' },
  { path: 'meeting_capture.require_notice', owner: 'client/meeting-capture', description: 'Require the user to confirm the participant notice was given before capture starts.', type: { kind: 'boolean' }, default: true, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.notice_text', owner: 'client/meeting-capture', description: 'Participant notice text by language: Czech, German and English.', type: { kind: 'map', value: { kind: 'string' }, keys: ['cs', 'de', 'en'] }, merge: 'per-key-map' },

  // specs: add-telemetry-and-admin/specs/client/telemetry/spec.md
  { path: 'telemetry.level', owner: 'client/telemetry', description: 'Select off, metadata or content telemetry. Content level includes prompts, responses and tool arguments.', type: { kind: 'enum', values: ['off', 'metadata', 'content'] }, default: 'metadata', merge: 'scalar', guardrail: 'max-level' },
  { path: 'telemetry.crash_reports', owner: 'client/telemetry', description: 'Allow crash reports when telemetry is not off.', type: { kind: 'boolean' }, default: true, merge: 'scalar' },
  { path: 'telemetry.buffer_max_mb', owner: 'client/telemetry', description: 'Maximum size in megabytes of the encrypted offline telemetry buffer.', type: { kind: 'integer', min: 1 }, default: 20, merge: 'scalar', guardrail: 'cap' },
  { path: 'telemetry.redaction_patterns', owner: 'client/telemetry', description: 'Additional regular expressions for secret redaction in telemetry.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },

  // specs: add-scheduled-and-remote-tasks/specs/client/scheduler/spec.md
  { path: 'scheduler.max_concurrent_runs', owner: 'client/scheduler', description: 'Maximum concurrent unattended scheduler Runs.', type: { kind: 'integer', min: 1 }, default: 2, merge: 'scalar', guardrail: 'cap' },
  { path: 'scheduler.catch_up_window_hours', owner: 'client/scheduler', description: 'How far back in hours a missed scheduled firing may be caught up.', type: { kind: 'integer', min: 0 }, default: 12, merge: 'scalar', guardrail: 'cap' },
  { path: 'scheduler.enabled_kinds', owner: 'client/scheduler', description: 'Trigger kinds users may create.', type: { kind: 'list', item: { kind: 'enum', values: ['schedule', 'file', 'mail', 'calendar', 'meeting', 'session'] } }, default: ['schedule', 'file', 'mail', 'calendar', 'meeting', 'session'], merge: 'union', guardrail: 'restrict' },
  { path: 'scheduler.max_triggers', owner: 'client/scheduler', description: 'Maximum number of Triggers per user.', type: { kind: 'integer', min: 1 }, default: 50, merge: 'scalar', guardrail: 'cap' },
  { path: 'scheduler.min_interval_minutes', owner: 'client/scheduler', description: 'Minimum interval in minutes between schedule Trigger firings.', type: { kind: 'integer', min: 1 }, default: 15, merge: 'scalar', guardrail: 'floor' },
  { path: 'scheduler.history_days', owner: 'client/scheduler', description: 'Number of days to retain scheduler history.', type: { kind: 'integer', min: 1 }, default: 90, merge: 'scalar', guardrail: 'cap' },
  { path: 'scheduler.remote_trusted_issuers', owner: 'client/scheduler', description: 'Remote task issuer IDs that do not require the first-issuer acceptance question.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },

  // specs: add-scheduled-and-remote-tasks/specs/control-plane/task-dispatch/spec.md
  { path: 'scheduler.remote_share_results', owner: 'control-plane/task-dispatch', description: 'Remote task issuer IDs allowed to receive result content in status queries.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },

  // specs: add-client-foundation/specs/client/local-storage/spec.md
  { path: 'storage.root', owner: 'client/local-storage', description: 'Override the user data-root folder within the user profile; takes effect on next start.', type: { kind: 'string' }, merge: 'scalar' },
  { path: 'storage.max_size_gb', owner: 'client/local-storage', description: 'Total data-root size limit in gigabytes; nonessential growth is stopped as the limit approaches.', type: { kind: 'number', min: 0 }, default: 10, merge: 'scalar', guardrail: 'cap' },

  // specs: add-client-foundation/specs/client/desktop-host/spec.md
  { path: 'ui.background_on_battery', owner: 'client/desktop-host', description: 'Allow otherwise deferred background work while battery saver is on.', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },

  // specs: add-offline-sign-in/specs/client/identity/spec.md
  { path: 'identity.offline_sign_in', owner: 'client/identity', description: 'Keep the user signed in beyond the browser session by requesting an offline refresh token at sign-in. Off revokes an existing offline token at the next policy update and requires sign-in again.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },

  // specs: add-mail-calendar/specs/client/mail-calendar/spec.md
  { path: 'mail.graph_base_url', owner: 'client/mail-calendar', description: 'Microsoft Graph endpoint used for mail and calendar; release builds require an approved Graph host.', type: { kind: 'string', pattern: '^(https://|http://(localhost|127\\.0\\.0\\.1|\\[::1\\])(:[0-9]+)?(/|$))' }, default: 'https://graph.microsoft.com/v1.0', merge: 'scalar', guardrail: 'force' },
  { path: 'mail.oauth.authority', owner: 'client/mail-calendar', description: 'OAuth authority for the organization Graph app registration.', type: { kind: 'string' }, default: '', merge: 'scalar', guardrail: 'force' },
  { path: 'mail.oauth.tenant_id', owner: 'client/mail-calendar', description: 'Tenant ID for the organization Graph app registration.', type: { kind: 'string' }, default: '', merge: 'scalar' },
  { path: 'mail.oauth.client_id', owner: 'client/mail-calendar', description: 'Client ID for the organization Graph app registration.', type: { kind: 'string' }, default: '', merge: 'scalar' },
  { path: 'mail.auth', owner: 'client/mail-calendar', description: 'Select OAuth or development-only mock authentication for mail and calendar.', type: { kind: 'enum', values: ['oauth', 'mock'] }, default: 'oauth', merge: 'scalar', guardrail: 'max-level' },
  { path: 'mail.excluded_labels', owner: 'client/mail-calendar', description: 'Sensitivity-label IDs whose items are withheld from indexing, model requests and tool results.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'mail.unlabelled', owner: 'client/mail-calendar', description: 'Choose whether mail and calendar items without a readable sensitivity label are included or withheld.', type: { kind: 'enum', values: ['exclude', 'include'] }, default: 'include', merge: 'scalar', guardrail: 'max-level' },
  { path: 'mail.exclusions.folders', owner: 'client/mail-calendar', description: 'Mail folder names excluded from sync, indexing and tool results.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'mail.exclusions.categories', owner: 'client/mail-calendar', description: 'Mail and calendar categories excluded from sync, indexing and tool results.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'mail.exclusions.private_items', owner: 'client/mail-calendar', description: 'Exclude private mail and calendar items from sync, indexing and tool results.', type: { kind: 'boolean' }, default: true, merge: 'scalar', guardrail: 'force' },
  { path: 'mail.sync_interval_minutes', owner: 'client/mail-calendar', description: 'Interval in minutes between background mail and calendar synchronization.', type: { kind: 'integer', min: 1 }, default: 5, merge: 'scalar', guardrail: 'floor' },
  { path: 'mail.sync_window.mail_days', owner: 'client/mail-calendar', description: 'Number of past days of mail retained by synchronization.', type: { kind: 'integer', min: 1 }, default: 180, merge: 'scalar', guardrail: 'cap' },
  { path: 'mail.sync_window.calendar_past_days', owner: 'client/mail-calendar', description: 'Number of past days of calendar events retained by synchronization.', type: { kind: 'integer', min: 0 }, default: 90, merge: 'scalar', guardrail: 'cap' },
  { path: 'mail.sync_window.calendar_future_days', owner: 'client/mail-calendar', description: 'Number of future days of calendar events retained by synchronization.', type: { kind: 'integer', min: 0 }, default: 365, merge: 'scalar', guardrail: 'cap' },
  // specs: add-user-messaging/specs/control-plane/user-messaging/spec.md (User Directory)
  { path: 'messaging.mode', owner: 'control-plane/user-messaging', description: 'Allow the user to use messaging and appear in the messaging directory.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  // specs: add-local-apps/specs/client/local-apps/spec.md (App Runtime, MCP Bridge)
  { path: 'apps.mode', owner: 'client/local-apps', description: 'Allow Local apps to start and make their tools available.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'apps.mcp', owner: 'client/local-apps', description: 'Allow Local apps to use the read-risk MCP bridge subject to confirmation and policy.', type: { kind: 'enum', values: ['off', 'read'] }, default: 'read', merge: 'scalar', guardrail: 'max-level' },
  // specs: add-dictation/specs/client/policy-enforcement/spec.md (Dictation Policy)
  { path: 'dictation.mode', owner: 'client/dictation', description: 'Allow the Dictate control for locally transcribing microphone input into a draft.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  // specs: improve-skowork-and-knowledge-control/specs/client/speech-models/spec.md (Speech Download Policy, Speech Package Catalog)
  { path: 'speech.download', owner: 'client/speech-models', description: 'Allow users to download the speech package. Off keeps an installed package and offline import.', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'speech.package.url', owner: 'client/speech-models', description: 'https address of the speech package archive (Skowork-speech-v<version>-win-x64.zip). Empty: no download is offered.', type: { kind: 'string' }, default: '', merge: 'scalar' },
  { path: 'speech.package.sha256', owner: 'client/speech-models', description: 'SHA-256 of the speech package archive, 64 hex characters. Downloads and imports must match it.', type: { kind: 'string' }, default: '', merge: 'scalar' },
  { path: 'speech.package.version', owner: 'client/speech-models', description: 'Version of the speech package named by the address, as in its file name. Shown to users in speech setup.', type: { kind: 'string' }, default: '', merge: 'scalar' },
  { path: 'speech.package.hosts', owner: 'client/speech-models', description: 'Further hosts a speech package download may be redirected to, for example the storage host of a release page.', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },
];
