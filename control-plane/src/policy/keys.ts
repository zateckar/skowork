// Policy keys of every capability (control-plane/policy-service Policy Schema Ownership; design D9, D10).
// Generated from the capability specs; each entry names its owning spec. Keep it in step with the specs.
import type { KeyDef } from './schema.ts';

export const SCHEMA_VERSION = 1;

/** A host pattern of the network lists: a host name or address with * and ? wildcards, no scheme, path or spaces. */
const HOST_PATTERN = '^[A-Za-z0-9*?._:\\[\\]-]+$';

export const KEYS: KeyDef[] = [
  // specs: add-agent-runtime/specs/client/agent-runtime/spec.md
  { path: 'llm.max_concurrent_runs', owner: 'client/agent-runtime', type: { kind: 'integer', min: 1, max: 64 }, default: 3, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.max_parallel_tools', owner: 'client/agent-runtime', type: { kind: 'integer', min: 1 }, default: 4, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.max_tool_result_kb', owner: 'client/agent-runtime', type: { kind: 'integer', min: 1 }, default: 64, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.attended.model_calls', owner: 'client/agent-runtime', type: { kind: 'integer', min: 1 }, default: 60, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.attended.tool_calls', owner: 'client/agent-runtime', type: { kind: 'integer', min: 1 }, default: 200, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.attended.wall_minutes', owner: 'client/agent-runtime', type: { kind: 'integer', min: 1 }, default: 60, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.unattended.model_calls', owner: 'client/agent-runtime', type: { kind: 'integer', min: 1 }, default: 30, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.unattended.tool_calls', owner: 'client/agent-runtime', type: { kind: 'integer', min: 1 }, default: 80, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.run_limits.unattended.wall_minutes', owner: 'client/agent-runtime', type: { kind: 'integer', min: 1 }, default: 30, merge: 'scalar', guardrail: 'cap' },

  // specs: add-agent-runtime/specs/client/model-access/spec.md
  { path: 'llm.max_images_per_request', owner: 'client/model-access', type: { kind: 'integer', min: 1 }, default: 4, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.timeouts.connect_seconds', owner: 'client/model-access', type: { kind: 'integer', min: 1 }, default: 10, merge: 'scalar' },
  { path: 'llm.timeouts.idle_seconds', owner: 'client/model-access', type: { kind: 'integer', min: 1 }, default: 60, merge: 'scalar' },

  // specs: add-agent-runtime/specs/control-plane/llm-gateway/spec.md
  { path: 'llm.models', owner: 'control-plane/llm-gateway', type: { kind: 'map', value: { kind: 'string' }, keys: ['main', 'vision', 'small', 'embed', 'rerank', 'image'] }, default: { main: 'acme-llm-primary', vision: 'acme-llm-primary', small: 'acme-llm-secondary', embed: 'acme-llm-embedding', rerank: 'acme-llm-reranker', image: 'acme-llm-image' }, merge: 'whole-map', guardrail: 'force-entries' },
  { path: 'llm.budget.tokens', owner: 'control-plane/llm-gateway', type: { kind: 'integer', min: 0 }, default: 1000000, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.budget.period', owner: 'control-plane/llm-gateway', type: { kind: 'enum', values: ['day', 'week', 'month'] }, default: 'day', merge: 'scalar' },
  { path: 'llm.budget.embed_daily_tokens', owner: 'control-plane/llm-gateway', type: { kind: 'integer', min: 0 }, default: 12000000, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.budget.images_daily', owner: 'control-plane/llm-gateway', type: { kind: 'integer', min: 0 }, default: 50, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.rate_limits.requests_per_minute', owner: 'control-plane/llm-gateway', type: { kind: 'integer', min: 1 }, default: 60, merge: 'scalar', guardrail: 'cap' },
  { path: 'llm.rate_limits.concurrent_requests', owner: 'control-plane/llm-gateway', type: { kind: 'integer', min: 1 }, default: 4, merge: 'scalar', guardrail: 'cap' },

  // specs: add-policy-and-sandbox/specs/client/policy-enforcement/spec.md
  { path: 'tools.rules', owner: 'client/policy-enforcement', type: { kind: 'decision-map' }, default: {}, merge: 'decision-map', guardrail: 'stricter' },
  { path: 'tools.risk', owner: 'client/policy-enforcement', type: { kind: 'decision-map', keys: ['read', 'write-local', 'external', 'destructive'] }, default: { read: 'allow', 'write-local': 'approve', external: 'approve', destructive: 'deny' }, merge: 'decision-map', guardrail: 'stricter' },
  { path: 'unattended.allowed_risk', owner: 'client/policy-enforcement', type: { kind: 'list', item: { kind: 'enum', values: ['read', 'write-local', 'external', 'destructive'] } }, default: ['read', 'write-local'], merge: 'union', guardrail: 'restrict' },
  { path: 'unattended.rules', owner: 'client/policy-enforcement', type: { kind: 'decision-map' }, default: {}, merge: 'decision-map', guardrail: 'stricter' },
  { path: 'filesystem.read', owner: 'client/policy-enforcement', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },
  { path: 'filesystem.write', owner: 'client/policy-enforcement', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },
  { path: 'filesystem.deny', owner: 'client/policy-enforcement', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'filesystem.grantable', owner: 'client/policy-enforcement', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },
  { path: 'network.egress_allow', owner: 'client/policy-enforcement', type: { kind: 'list', item: { kind: 'string', pattern: HOST_PATTERN } }, default: [], merge: 'union' },
  { path: 'network.egress_deny', owner: 'client/policy-enforcement', type: { kind: 'list', item: { kind: 'string', pattern: HOST_PATTERN } }, default: [], merge: 'union', guardrail: 'add' },

  // specs: add-policy-and-sandbox/specs/client/tool-sandbox/spec.md
  { path: 'tools.shell.mode', owner: 'client/tool-sandbox', type: { kind: 'enum', values: ['off', 'sandboxed', 'allow'] }, default: 'off', merge: 'scalar', guardrail: 'max-level' },
  { path: 'tools.shell.limits.memory_mb', owner: 'client/tool-sandbox', type: { kind: 'integer', min: 1 }, default: 1024, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.shell.limits.cpu_percent', owner: 'client/tool-sandbox', type: { kind: 'integer', min: 1, max: 100 }, default: 50, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.shell.limits.processes', owner: 'client/tool-sandbox', type: { kind: 'integer', min: 1 }, default: 32, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.shell.limits.wall_minutes', owner: 'client/tool-sandbox', type: { kind: 'integer', min: 1 }, default: 15, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.shell.limits.output_kb', owner: 'client/tool-sandbox', type: { kind: 'integer', min: 1 }, default: 1024, merge: 'scalar', guardrail: 'cap' },

  // specs: add-core-tools-and-mcp/specs/client/core-tools/spec.md (and control-plane/mcp-catalog for mcp.* below)
  { path: 'tools.shell.executable', owner: 'client/core-tools', type: { kind: 'enum', values: ['powershell', 'pwsh'] }, default: 'powershell', merge: 'scalar' },
  { path: 'tools.shell.destructive_patterns', owner: 'client/core-tools', type: { kind: 'list', item: { kind: 'string' } }, merge: 'union', guardrail: 'add' },
  { path: 'tools.files.max_read_kb', owner: 'client/core-tools', type: { kind: 'integer', min: 1 }, default: 256, merge: 'scalar', guardrail: 'cap' },

  // specs: add-work-inbox/specs/client/work-inbox/spec.md
  { path: 'notifications.completed', owner: 'client/work-inbox', type: { kind: 'boolean' }, default: true, merge: 'scalar' },
  { path: 'notifications.failed', owner: 'client/work-inbox', type: { kind: 'boolean' }, default: true, merge: 'scalar' },
  { path: 'notifications.action', owner: 'client/work-inbox', type: { kind: 'boolean' }, default: true, merge: 'scalar' },

  // specs: add-core-tools-and-mcp/specs/client/mcp-client/spec.md
  { path: 'mcp.idle_timeout_minutes', owner: 'client/mcp-client', type: { kind: 'integer', min: 1 }, default: 10, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.mcp.limits.memory_mb', owner: 'client/mcp-client', type: { kind: 'integer', min: 1 }, default: 1024, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.mcp.limits.cpu_percent', owner: 'client/mcp-client', type: { kind: 'integer', min: 1, max: 100 }, default: 50, merge: 'scalar', guardrail: 'cap' },
  { path: 'tools.mcp.limits.processes', owner: 'client/mcp-client', type: { kind: 'integer', min: 1 }, default: 32, merge: 'scalar', guardrail: 'cap' },

  // specs: add-computer-use/specs/client/computer-use/spec.md
  { path: 'tools.computer.mode', owner: 'client/computer-use', type: { kind: 'enum', values: ['off', 'allow'] }, default: 'off', merge: 'scalar', guardrail: 'max-level' },
  { path: 'tools.computer.perception', owner: 'client/computer-use', type: { kind: 'enum', values: ['tree_only', 'tree_and_vision'] }, default: 'tree_and_vision', merge: 'scalar', guardrail: 'max-level' },
  { path: 'tools.computer.stop_hotkey', owner: 'client/computer-use', type: { kind: 'string' }, default: 'Ctrl+Alt+End', merge: 'scalar' },
  { path: 'tools.computer.app_allowlist', owner: 'client/computer-use', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },
  { path: 'tools.computer.app_denylist', owner: 'client/computer-use', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'tools.computer.retain_screenshots', owner: 'client/computer-use', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },

  // specs: add-browser-use/specs/client/browser-use/spec.md
  { path: 'tools.browser.retain_screenshots', owner: 'client/browser-use', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'tools.browser.allow_eval', owner: 'client/browser-use', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'tools.browser.headless_unattended', owner: 'client/browser-use', type: { kind: 'boolean' }, default: false, merge: 'scalar' },
  { path: 'tools.browser.idle_timeout_minutes', owner: 'client/browser-use', type: { kind: 'integer', min: 1 }, default: 15, merge: 'scalar' },

  // specs: add-skills/specs/client/skills/spec.md
  { path: 'skills.locked', owner: 'client/skills', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'skills.publish', owner: 'client/skills', type: { kind: 'list', item: { kind: 'enum', values: ['shared', 'group', 'org'] } }, default: ['shared'], merge: 'union', guardrail: 'restrict' },
  { path: 'skills.install_from', owner: 'client/skills', type: { kind: 'list', item: { kind: 'string', pattern: '^(org|group|shared|group:.+)$' } }, default: ['org', 'group', 'shared'], merge: 'union', guardrail: 'restrict' },
  { path: 'skills.script_runtimes', owner: 'client/skills', type: { kind: 'list', item: { kind: 'string' } }, default: ['powershell'], merge: 'union' },
  { path: 'skills.auto_update', owner: 'client/skills', type: { kind: 'boolean' }, default: true, merge: 'scalar' },

  // specs: add-knowledge-graph/specs/client/knowledge-graph/spec.md (on|off strings, D9)
  { path: 'knowledge.sources.threads', owner: 'client/knowledge-graph', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.sources.files', owner: 'client/knowledge-graph', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.sources.mail', owner: 'client/knowledge-graph', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.sources.calendar', owner: 'client/knowledge-graph', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  // specs: add-knowledge-sharing/specs/client/knowledge-graph/spec.md (Knowledge Notes, Sharing Knowledge Items)
  { path: 'knowledge.sources.notes', owner: 'client/knowledge-graph', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.sharing.groups', owner: 'client/knowledge-graph', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'restrict' },
  { path: 'knowledge.sources.meetings', owner: 'client/knowledge-graph', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.extraction', owner: 'client/knowledge-graph', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.directory_enrichment', owner: 'client/knowledge-graph', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'knowledge.max_file_mb', owner: 'client/knowledge-graph', type: { kind: 'integer', min: 1 }, default: 25, merge: 'scalar', guardrail: 'cap' },
  { path: 'knowledge.embedding_daily_tokens', owner: 'client/knowledge-graph', type: { kind: 'integer', min: 0 }, default: 10000000, merge: 'scalar', guardrail: 'cap' },
  { path: 'knowledge.extraction_daily_calls', owner: 'client/knowledge-graph', type: { kind: 'integer', min: 0 }, default: 2000, merge: 'scalar', guardrail: 'cap' },
  { path: 'knowledge.max_threads', owner: 'client/knowledge-graph', type: { kind: 'integer', min: 1 }, default: 2, merge: 'scalar', guardrail: 'cap' },
  { path: 'knowledge.exclusions', owner: 'client/knowledge-graph', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  // specs: add-save-as-note/specs/client/knowledge-graph/spec.md (Save As Note)
  { path: 'knowledge.save_as_note', owner: 'client/knowledge-graph', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },

  // specs: add-meeting-capture/specs/client/meeting-capture/spec.md
  { path: 'meeting_capture.enabled', owner: 'client/meeting-capture', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.apps', owner: 'client/meeting-capture', type: { kind: 'list', item: { kind: 'string' } }, merge: 'union' },
  { path: 'meeting_capture.allow_manual', owner: 'client/meeting-capture', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.allow_series_auto', owner: 'client/meeting-capture', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.live_transcript', owner: 'client/meeting-capture', type: { kind: 'boolean' }, default: false, merge: 'scalar' },
  { path: 'meeting_capture.keep_audio', owner: 'client/meeting-capture', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.max_pending_days', owner: 'client/meeting-capture', type: { kind: 'integer', min: 1 }, default: 7, merge: 'scalar', guardrail: 'cap' },
  { path: 'meeting_capture.require_notice', owner: 'client/meeting-capture', type: { kind: 'boolean' }, default: true, merge: 'scalar', guardrail: 'force' },
  { path: 'meeting_capture.notice_text', owner: 'client/meeting-capture', type: { kind: 'map', value: { kind: 'string' }, keys: ['cs', 'de', 'en'] }, merge: 'per-key-map' },

  // specs: add-telemetry-and-admin/specs/client/telemetry/spec.md
  { path: 'telemetry.level', owner: 'client/telemetry', type: { kind: 'enum', values: ['off', 'metadata', 'content'] }, default: 'metadata', merge: 'scalar', guardrail: 'max-level' },
  { path: 'telemetry.crash_reports', owner: 'client/telemetry', type: { kind: 'boolean' }, default: true, merge: 'scalar' },
  { path: 'telemetry.buffer_max_mb', owner: 'client/telemetry', type: { kind: 'integer', min: 1 }, default: 20, merge: 'scalar', guardrail: 'cap' },
  { path: 'telemetry.redaction_patterns', owner: 'client/telemetry', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },

  // specs: add-scheduled-and-remote-tasks/specs/client/scheduler/spec.md
  { path: 'scheduler.max_concurrent_runs', owner: 'client/scheduler', type: { kind: 'integer', min: 1 }, default: 2, merge: 'scalar', guardrail: 'cap' },
  { path: 'scheduler.catch_up_window_hours', owner: 'client/scheduler', type: { kind: 'integer', min: 0 }, default: 12, merge: 'scalar', guardrail: 'cap' },
  { path: 'scheduler.enabled_kinds', owner: 'client/scheduler', type: { kind: 'list', item: { kind: 'enum', values: ['schedule', 'file', 'mail', 'calendar', 'meeting', 'session'] } }, default: ['schedule', 'file', 'mail', 'calendar', 'meeting', 'session'], merge: 'union', guardrail: 'restrict' },
  { path: 'scheduler.max_triggers', owner: 'client/scheduler', type: { kind: 'integer', min: 1 }, default: 50, merge: 'scalar', guardrail: 'cap' },
  { path: 'scheduler.min_interval_minutes', owner: 'client/scheduler', type: { kind: 'integer', min: 1 }, default: 15, merge: 'scalar', guardrail: 'floor' },
  { path: 'scheduler.history_days', owner: 'client/scheduler', type: { kind: 'integer', min: 1 }, default: 90, merge: 'scalar', guardrail: 'cap' },
  { path: 'scheduler.remote_trusted_issuers', owner: 'client/scheduler', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },

  // specs: add-scheduled-and-remote-tasks/specs/control-plane/task-dispatch/spec.md
  { path: 'scheduler.remote_share_results', owner: 'control-plane/task-dispatch', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union' },

  // specs: add-client-foundation/specs/client/local-storage/spec.md
  { path: 'storage.root', owner: 'client/local-storage', type: { kind: 'string' }, merge: 'scalar' },
  { path: 'storage.max_size_gb', owner: 'client/local-storage', type: { kind: 'number', min: 0 }, default: 10, merge: 'scalar', guardrail: 'cap' },

  // specs: add-client-foundation/specs/client/desktop-host/spec.md
  { path: 'ui.background_on_battery', owner: 'client/desktop-host', type: { kind: 'boolean' }, default: false, merge: 'scalar', guardrail: 'force' },

  // specs: add-mail-calendar/specs/client/mail-calendar/spec.md
  { path: 'mail.graph_base_url', owner: 'client/mail-calendar', type: { kind: 'string', pattern: '^(https://|http://(localhost|127\\.0\\.0\\.1|\\[::1\\])(:[0-9]+)?(/|$))' }, default: 'https://graph.microsoft.com/v1.0', merge: 'scalar', guardrail: 'force' },
  { path: 'mail.oauth.authority', owner: 'client/mail-calendar', type: { kind: 'string' }, default: '', merge: 'scalar', guardrail: 'force' },
  { path: 'mail.oauth.tenant_id', owner: 'client/mail-calendar', type: { kind: 'string' }, default: '', merge: 'scalar' },
  { path: 'mail.oauth.client_id', owner: 'client/mail-calendar', type: { kind: 'string' }, default: '', merge: 'scalar' },
  { path: 'mail.auth', owner: 'client/mail-calendar', type: { kind: 'enum', values: ['oauth', 'mock'] }, default: 'oauth', merge: 'scalar', guardrail: 'max-level' },
  { path: 'mail.excluded_labels', owner: 'client/mail-calendar', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'mail.unlabelled', owner: 'client/mail-calendar', type: { kind: 'enum', values: ['exclude', 'include'] }, default: 'include', merge: 'scalar', guardrail: 'max-level' },
  { path: 'mail.exclusions.folders', owner: 'client/mail-calendar', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'mail.exclusions.categories', owner: 'client/mail-calendar', type: { kind: 'list', item: { kind: 'string' } }, default: [], merge: 'union', guardrail: 'add' },
  { path: 'mail.exclusions.private_items', owner: 'client/mail-calendar', type: { kind: 'boolean' }, default: true, merge: 'scalar', guardrail: 'force' },
  { path: 'mail.sync_interval_minutes', owner: 'client/mail-calendar', type: { kind: 'integer', min: 1 }, default: 5, merge: 'scalar', guardrail: 'floor' },
  { path: 'mail.sync_window.mail_days', owner: 'client/mail-calendar', type: { kind: 'integer', min: 1 }, default: 180, merge: 'scalar', guardrail: 'cap' },
  { path: 'mail.sync_window.calendar_past_days', owner: 'client/mail-calendar', type: { kind: 'integer', min: 0 }, default: 90, merge: 'scalar', guardrail: 'cap' },
  { path: 'mail.sync_window.calendar_future_days', owner: 'client/mail-calendar', type: { kind: 'integer', min: 0 }, default: 365, merge: 'scalar', guardrail: 'cap' },
  // specs: add-user-messaging/specs/control-plane/user-messaging/spec.md (User Directory)
  { path: 'messaging.mode', owner: 'control-plane/user-messaging', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  // specs: add-local-apps/specs/client/local-apps/spec.md (App Runtime, MCP Bridge)
  { path: 'apps.mode', owner: 'client/local-apps', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
  { path: 'apps.mcp', owner: 'client/local-apps', type: { kind: 'enum', values: ['off', 'read'] }, default: 'read', merge: 'scalar', guardrail: 'max-level' },
  // specs: add-dictation/specs/client/policy-enforcement/spec.md (Dictation Policy)
  { path: 'dictation.mode', owner: 'client/dictation', type: { kind: 'enum', values: ['off', 'on'] }, default: 'on', merge: 'scalar', guardrail: 'max-level' },
];
