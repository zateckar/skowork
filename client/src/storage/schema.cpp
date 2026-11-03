// app.db schema. Migrations are append-only: never edit a released migration, add a new version instead.
// Version ranges per roadmap change keep merges simple: 1-99 client-foundation, 100-199 agent-runtime,
// 200-299 policy-and-sandbox, 300-399 work-inbox, 400-499 core-tools-and-mcp, 500-599 browser-use,
// 600-699 telemetry, 700-799 computer-use, 800-899 skills, 900-999 scheduler, 1000-1099 mail-calendar,
// 1100-1199 knowledge (app.db side), 1200-1299 meeting-capture, 1300-1399 skills additions made after them, 1400-1499 user-messaging.
#include "storage/app_store.h"

namespace wb {

const std::vector<Migration>& app_migrations() {
    static const std::vector<Migration> migrations = {
        {1, R"sql(
CREATE TABLE settings(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  updated_at TEXT NOT NULL);
CREATE TABLE secrets(
  name TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  updated_at TEXT NOT NULL);
CREATE TABLE security_events(
  id INTEGER PRIMARY KEY,
  at TEXT NOT NULL,
  kind TEXT NOT NULL,
  detail TEXT NOT NULL);
CREATE TABLE channel_outbox(
  id INTEGER PRIMARY KEY,
  type TEXT NOT NULL,
  ref INTEGER NOT NULL,
  payload TEXT NOT NULL,
  created_at TEXT NOT NULL);
)sql"},
        // 100-199: add-agent-runtime (client/agent-runtime Thread Persistence, Run Lifecycle States, Interrupts).
        {100, R"sql(
CREATE TABLE threads(
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL);
CREATE TABLE runs(
  id TEXT PRIMARY KEY,
  thread_id TEXT NOT NULL REFERENCES threads(id),
  parent_run_id TEXT,
  state TEXT NOT NULL CHECK(state IN ('queued','running','interrupted','succeeded','failed','cancelled')),
  reason TEXT NOT NULL DEFAULT '',
  origin_kind TEXT NOT NULL CHECK(origin_kind IN ('user_message','trigger_occurrence','remote_task')),
  origin_ref TEXT NOT NULL DEFAULT '',
  unattended INTEGER NOT NULL DEFAULT 0,
  queue_seq INTEGER NOT NULL,
  role TEXT NOT NULL DEFAULT 'main',
  active_ms INTEGER NOT NULL DEFAULT 0,
  model_calls INTEGER NOT NULL DEFAULT 0,
  tool_calls INTEGER NOT NULL DEFAULT 0,
  prompt_tokens INTEGER NOT NULL DEFAULT 0,
  completion_tokens INTEGER NOT NULL DEFAULT 0,
  reasoning_tokens INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL,
  started_at TEXT NOT NULL DEFAULT '',
  ended_at TEXT NOT NULL DEFAULT '');
CREATE INDEX runs_thread ON runs(thread_id, queue_seq);
CREATE INDEX runs_state ON runs(state, queue_seq);
CREATE TABLE run_events(
  run_id TEXT NOT NULL REFERENCES runs(id),
  seq INTEGER NOT NULL,
  at TEXT NOT NULL,
  kind TEXT NOT NULL,
  payload TEXT NOT NULL,
  PRIMARY KEY(run_id, seq)) WITHOUT ROWID;
CREATE TABLE action_items(
  id TEXT PRIMARY KEY,
  thread_id TEXT NOT NULL REFERENCES threads(id),
  run_id TEXT NOT NULL REFERENCES runs(id),
  kind TEXT NOT NULL CHECK(kind IN ('approval','question')),
  call_id TEXT NOT NULL,
  payload TEXT NOT NULL,
  created_at TEXT NOT NULL,
  resolved_at TEXT NOT NULL DEFAULT '',
  resolution TEXT NOT NULL DEFAULT 'null');
CREATE INDEX action_items_run ON action_items(run_id, created_at);
CREATE INDEX action_items_thread ON action_items(thread_id, resolved_at);
CREATE TABLE queued_messages(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  thread_id TEXT NOT NULL REFERENCES threads(id),
  text TEXT NOT NULL,
  mode TEXT NOT NULL CHECK(mode IN ('queue','steer_now')),
  created_at TEXT NOT NULL,
  delivered_at TEXT NOT NULL DEFAULT '');
CREATE INDEX queued_messages_thread ON queued_messages(thread_id, delivered_at);
)sql"},
        // Compaction records and Thread attachments (client/agent-runtime Context Compaction, Large Tool Results).
        {101, R"sql(
CREATE TABLE thread_compactions(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  thread_id TEXT NOT NULL REFERENCES threads(id),
  at TEXT NOT NULL,
  kind TEXT NOT NULL CHECK(kind IN ('automatic','manual')),
  run_id TEXT NOT NULL DEFAULT '',
  summary TEXT NOT NULL,
  boundary_run TEXT NOT NULL,
  boundary_seq INTEGER NOT NULL,
  carried TEXT NOT NULL DEFAULT '[]',
  messages_compacted INTEGER NOT NULL,
  tokens_before INTEGER NOT NULL,
  tokens_after INTEGER NOT NULL);
CREATE INDEX thread_compactions_thread ON thread_compactions(thread_id, id);
CREATE TABLE thread_attachments(
  id TEXT PRIMARY KEY,
  thread_id TEXT NOT NULL REFERENCES threads(id),
  run_id TEXT NOT NULL,
  call_id TEXT NOT NULL,
  name TEXT NOT NULL,
  size INTEGER NOT NULL,
  provenance TEXT NOT NULL,
  content BLOB NOT NULL,
  created_at TEXT NOT NULL);
CREATE INDEX thread_attachments_thread ON thread_attachments(thread_id);
)sql"},
        // 200-299: add-policy-and-sandbox (client/policy-enforcement Decision Audit, User Folder Grants).
        {200, R"sql(
CREATE TABLE policy_decisions(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  at TEXT NOT NULL,
  kind TEXT NOT NULL CHECK(kind IN ('decision','resolution')),
  run_id TEXT NOT NULL DEFAULT '',
  call_id TEXT NOT NULL DEFAULT '',
  tool TEXT NOT NULL,
  risk TEXT NOT NULL DEFAULT '',
  outcome TEXT NOT NULL,
  rule TEXT NOT NULL DEFAULT '',
  source TEXT NOT NULL DEFAULT '',
  policy_version INTEGER NOT NULL DEFAULT 0,
  reason TEXT NOT NULL DEFAULT '',
  reported INTEGER NOT NULL DEFAULT 0);
CREATE INDEX policy_decisions_call ON policy_decisions(call_id);
CREATE INDEX policy_decisions_at ON policy_decisions(at);
CREATE TABLE folder_grants(
  folder TEXT PRIMARY KEY COLLATE NOCASE,
  write INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL);
)sql"},
        // 300-399: add-work-inbox (design D8-D12). Standalone Action items have no Run; Threads get their inbox state;
        // folders, drafts and the FTS5 search index. Triggers keep the preview, the activity time, the archive state and
        // the index in step with the runtime's writes, including those made while the window is closed.
        {300, R"sql(
CREATE TABLE action_items_v300(
  id TEXT PRIMARY KEY,
  thread_id TEXT NOT NULL REFERENCES threads(id),
  run_id TEXT REFERENCES runs(id),
  kind TEXT NOT NULL CHECK(kind IN ('approval','question')),
  call_id TEXT NOT NULL,
  payload TEXT NOT NULL,
  created_at TEXT NOT NULL,
  resolved_at TEXT NOT NULL DEFAULT '',
  resolution TEXT NOT NULL DEFAULT 'null',
  raised_by TEXT NOT NULL DEFAULT '');
INSERT INTO action_items_v300(id,thread_id,run_id,kind,call_id,payload,created_at,resolved_at,resolution)
  SELECT id,thread_id,run_id,kind,call_id,payload,created_at,resolved_at,resolution FROM action_items;
DROP TABLE action_items;
ALTER TABLE action_items_v300 RENAME TO action_items;
CREATE INDEX action_items_run ON action_items(run_id, created_at);
CREATE INDEX action_items_thread ON action_items(thread_id, resolved_at);

CREATE TABLE folders(
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  created_at TEXT NOT NULL);
ALTER TABLE threads ADD COLUMN origin_kind TEXT NOT NULL DEFAULT 'user'
  CHECK(origin_kind IN ('user','trigger','remote','meeting','fork','system'));
ALTER TABLE threads ADD COLUMN origin_ref TEXT NOT NULL DEFAULT 'null';
ALTER TABLE threads ADD COLUMN unread INTEGER NOT NULL DEFAULT 0;
ALTER TABLE threads ADD COLUMN archived INTEGER NOT NULL DEFAULT 0;
ALTER TABLE threads ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0;
ALTER TABLE threads ADD COLUMN folder_id TEXT REFERENCES folders(id);
ALTER TABLE threads ADD COLUMN last_activity TEXT NOT NULL DEFAULT '';
ALTER TABLE threads ADD COLUMN preview TEXT NOT NULL DEFAULT '';
UPDATE threads SET last_activity=updated_at;
UPDATE threads SET preview=COALESCE((
  SELECT substr(json_extract(e.payload,'$.content'),1,200) FROM run_events e JOIN runs r ON r.id=e.run_id
  WHERE r.thread_id=threads.id AND r.parent_run_id IS NULL AND e.kind='message'
    AND json_extract(e.payload,'$.role') IN ('user','assistant') AND COALESCE(json_extract(e.payload,'$.content'),'')<>''
  ORDER BY r.queue_seq DESC, e.seq DESC LIMIT 1), '');
CREATE INDEX threads_inbox ON threads(archived, pinned, last_activity, id);
CREATE INDEX threads_folder ON threads(folder_id);

CREATE TABLE drafts(
  thread_key TEXT PRIMARY KEY,
  text TEXT NOT NULL,
  updated_at TEXT NOT NULL);
CREATE TABLE draft_attachments(
  id TEXT PRIMARY KEY,
  thread_key TEXT NOT NULL,
  name TEXT NOT NULL,
  size INTEGER NOT NULL,
  content BLOB NOT NULL,
  created_at TEXT NOT NULL);
CREATE INDEX draft_attachments_key ON draft_attachments(thread_key);

CREATE VIRTUAL TABLE thread_search USING fts5(title, body, thread_id UNINDEXED, run_id UNINDEXED, seq UNINDEXED,
  tokenize='unicode61 remove_diacritics 2');
INSERT INTO thread_search(title,body,thread_id,run_id,seq) SELECT title,'',id,'',0 FROM threads;
INSERT INTO thread_search(title,body,thread_id,run_id,seq)
  SELECT '', json_extract(e.payload,'$.content'), r.thread_id, e.run_id, e.seq FROM run_events e JOIN runs r ON r.id=e.run_id
  WHERE r.parent_run_id IS NULL AND e.kind='message' AND json_extract(e.payload,'$.role') IN ('user','assistant')
    AND COALESCE(json_extract(e.payload,'$.content'),'')<>'';

CREATE TRIGGER threads_search_insert AFTER INSERT ON threads BEGIN
  INSERT INTO thread_search(title,body,thread_id,run_id,seq) VALUES(new.title,'',new.id,'',0);
  UPDATE threads SET last_activity=new.created_at WHERE id=new.id AND last_activity='';
END;
CREATE TRIGGER threads_search_title AFTER UPDATE OF title ON threads BEGIN
  DELETE FROM thread_search WHERE thread_id=old.id AND seq=0 AND run_id='';
  INSERT INTO thread_search(title,body,thread_id,run_id,seq) VALUES(new.title,'',new.id,'',0);
END;
CREATE TRIGGER threads_search_delete AFTER DELETE ON threads BEGIN
  DELETE FROM thread_search WHERE thread_id=old.id;
END;
CREATE TRIGGER run_events_inbox AFTER INSERT ON run_events
  WHEN new.kind='message' AND json_extract(new.payload,'$.role') IN ('user','assistant')
   AND COALESCE(json_extract(new.payload,'$.content'),'')<>''
BEGIN
  INSERT INTO thread_search(title,body,thread_id,run_id,seq)
    SELECT '', json_extract(new.payload,'$.content'), r.thread_id, new.run_id, new.seq FROM runs r
    WHERE r.id=new.run_id AND r.parent_run_id IS NULL;
  UPDATE threads SET preview=substr(json_extract(new.payload,'$.content'),1,200), last_activity=max(last_activity,new.at)
    WHERE id=(SELECT thread_id FROM runs WHERE id=new.run_id AND parent_run_id IS NULL);
END;
CREATE TRIGGER runs_inbox_insert AFTER INSERT ON runs WHEN new.parent_run_id IS NULL BEGIN
  UPDATE threads SET archived=0, last_activity=max(last_activity,new.created_at) WHERE id=new.thread_id;
END;
CREATE TRIGGER runs_inbox_state AFTER UPDATE OF state ON runs WHEN new.parent_run_id IS NULL BEGIN
  UPDATE threads SET last_activity=max(last_activity, CASE WHEN new.ended_at<>'' THEN new.ended_at ELSE new.started_at END)
    WHERE id=new.thread_id;
END;
CREATE TRIGGER action_items_inbox AFTER INSERT ON action_items BEGIN
  UPDATE threads SET archived=0, last_activity=max(last_activity,new.created_at) WHERE id=new.thread_id;
END;
)sql"},
        // client/scheduler (add-scheduled-and-remote-tasks design D1, D9): Triggers, Occurrences with leases and their
        // attempts, the files a file Trigger has seen, and Remote tasks with the issuers the user always accepts.
        // Times are unix milliseconds.
        {900, R"sql(
CREATE TABLE triggers(
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  kind TEXT NOT NULL CHECK(kind IN ('schedule','file','mail','calendar','meeting','session')),
  conditions TEXT NOT NULL,
  prompt TEXT NOT NULL,
  skill TEXT NOT NULL DEFAULT '',
  thread_mode TEXT NOT NULL CHECK(thread_mode IN ('new','append')),
  thread_id TEXT NOT NULL DEFAULT '',
  enabled INTEGER NOT NULL DEFAULT 1,
  notify INTEGER NOT NULL DEFAULT 1,
  owner TEXT NOT NULL,
  created_by TEXT NOT NULL CHECK(created_by IN ('user','agent')),
  disabled_reason TEXT NOT NULL DEFAULT '',
  next_fire_at INTEGER,
  missed_since INTEGER,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL);
CREATE TABLE occurrences(
  id TEXT PRIMARY KEY,
  trigger_id TEXT NOT NULL DEFAULT '',
  remote_task_id TEXT NOT NULL DEFAULT '',
  fire_key TEXT NOT NULL,
  reason TEXT NOT NULL CHECK(reason IN ('scheduled','catch-up','manual','event','remote')),
  fire_at INTEGER NOT NULL,
  state TEXT NOT NULL CHECK(state IN ('pending','running','succeeded','failed','dead','cancelled','skipped')),
  attempts INTEGER NOT NULL DEFAULT 0,
  next_attempt_at INTEGER NOT NULL DEFAULT 0,
  detail TEXT NOT NULL DEFAULT '{}',
  thread_id TEXT NOT NULL DEFAULT '',
  run_id TEXT NOT NULL DEFAULT '',
  lease_owner TEXT NOT NULL DEFAULT '',
  lease_until INTEGER NOT NULL DEFAULT 0,
  outcome TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL,
  started_at INTEGER NOT NULL DEFAULT 0,
  ended_at INTEGER NOT NULL DEFAULT 0,
  UNIQUE(trigger_id, remote_task_id, fire_key));
CREATE INDEX occurrences_state ON occurrences(state, next_attempt_at);
CREATE INDEX occurrences_trigger ON occurrences(trigger_id, fire_at);
CREATE TABLE occurrence_attempts(
  occurrence_id TEXT NOT NULL,
  n INTEGER NOT NULL,
  run_id TEXT NOT NULL DEFAULT '',
  started_at INTEGER NOT NULL,
  ended_at INTEGER NOT NULL DEFAULT 0,
  outcome TEXT NOT NULL DEFAULT '',
  error TEXT NOT NULL DEFAULT '',
  PRIMARY KEY(occurrence_id, n));
CREATE TABLE trigger_files(
  trigger_id TEXT NOT NULL,
  path TEXT NOT NULL,
  size INTEGER NOT NULL,
  mtime INTEGER NOT NULL,
  PRIMARY KEY(trigger_id, path));
CREATE TABLE remote_tasks(
  task_id TEXT PRIMARY KEY,
  push_ref INTEGER NOT NULL,
  issuer_id TEXT NOT NULL,
  issuer_name TEXT NOT NULL,
  payload TEXT NOT NULL,
  state TEXT NOT NULL,
  action_id TEXT NOT NULL DEFAULT '',
  thread_id TEXT NOT NULL DEFAULT '',
  occurrence_id TEXT NOT NULL DEFAULT '',
  note TEXT NOT NULL DEFAULT '',
  expires_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL);
CREATE TABLE remote_trusted_issuers(
  issuer_id TEXT PRIMARY KEY,
  issuer_name TEXT NOT NULL,
  added_at INTEGER NOT NULL);
)sql"},
        // client/mail-calendar (add-mail-calendar design D3): the synced mail and calendar copy, its search index, the
        // delta links and the user's exclusions. Times are unix milliseconds; withheld items keep no content.
        {1100, R"sql(
CREATE TABLE mail_folders(
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  well_known TEXT NOT NULL DEFAULT '');
CREATE TABLE mail_messages(
  id TEXT PRIMARY KEY,
  folder_id TEXT NOT NULL,
  conversation_id TEXT NOT NULL DEFAULT '',
  change_key TEXT NOT NULL DEFAULT '',
  received_at INTEGER NOT NULL,
  subject TEXT NOT NULL DEFAULT '',
  from_name TEXT NOT NULL DEFAULT '',
  from_address TEXT NOT NULL DEFAULT '',
  to_json TEXT NOT NULL DEFAULT '[]',
  cc_json TEXT NOT NULL DEFAULT '[]',
  body TEXT NOT NULL DEFAULT '',
  preview TEXT NOT NULL DEFAULT '',
  has_attachments INTEGER NOT NULL DEFAULT 0,
  is_read INTEGER NOT NULL DEFAULT 0,
  is_draft INTEGER NOT NULL DEFAULT 0,
  categories TEXT NOT NULL DEFAULT '[]',
  label TEXT NOT NULL DEFAULT '',
  withheld INTEGER NOT NULL DEFAULT 0,
  web_link TEXT NOT NULL DEFAULT '');
CREATE INDEX mail_messages_folder ON mail_messages(folder_id, received_at);
CREATE INDEX mail_messages_received ON mail_messages(received_at);
CREATE VIRTUAL TABLE mail_search USING fts5(subject, body, people, message_id UNINDEXED, tokenize='unicode61 remove_diacritics 2');
CREATE TABLE cal_events(
  id TEXT PRIMARY KEY,
  ical_uid TEXT NOT NULL DEFAULT '',
  series_master_id TEXT NOT NULL DEFAULT '',
  change_key TEXT NOT NULL DEFAULT '',
  start_at INTEGER NOT NULL,
  end_at INTEGER NOT NULL,
  all_day INTEGER NOT NULL DEFAULT 0,
  subject TEXT NOT NULL DEFAULT '',
  body TEXT NOT NULL DEFAULT '',
  location TEXT NOT NULL DEFAULT '',
  organizer_json TEXT NOT NULL DEFAULT '{}',
  attendees_json TEXT NOT NULL DEFAULT '[]',
  is_organizer INTEGER NOT NULL DEFAULT 0,
  cancelled INTEGER NOT NULL DEFAULT 0,
  response TEXT NOT NULL DEFAULT '',
  show_as TEXT NOT NULL DEFAULT '',
  sensitivity TEXT NOT NULL DEFAULT '',
  masked INTEGER NOT NULL DEFAULT 0,
  categories TEXT NOT NULL DEFAULT '[]',
  label TEXT NOT NULL DEFAULT '',
  withheld INTEGER NOT NULL DEFAULT 0,
  web_link TEXT NOT NULL DEFAULT '');
CREATE INDEX cal_events_start ON cal_events(start_at);
CREATE TABLE mail_sync_state(
  key TEXT PRIMARY KEY,
  delta_link TEXT NOT NULL DEFAULT '',
  extra TEXT NOT NULL DEFAULT '',
  synced_at INTEGER NOT NULL DEFAULT 0);
CREATE TABLE mail_user_exclusions(
  kind TEXT NOT NULL CHECK(kind IN ('folder','category')),
  value TEXT NOT NULL,
  PRIMARY KEY(kind, value));
)sql"},
        // The join link of online meetings, for the calendar match of client/meeting-capture (add-meeting-capture D3).
        {1101, R"sql(
ALTER TABLE cal_events ADD COLUMN online_url TEXT NOT NULL DEFAULT '';
)sql"},
        // client/meeting-capture (add-meeting-capture design D5): meetings with their state, audio and transcript, and
        // the series opted in for automatic capture.
        {1200, R"sql(
CREATE TABLE meetings(
  id TEXT PRIMARY KEY,
  thread_id TEXT NOT NULL DEFAULT '',
  state TEXT NOT NULL CHECK(state IN ('offered','notice','capturing','pending','transcribing','summarizing','done',
                                      'declined','discarded','failed','unsupported','expired')),
  source TEXT NOT NULL DEFAULT '',
  app TEXT NOT NULL DEFAULT '',
  title TEXT NOT NULL DEFAULT '',
  event_id TEXT NOT NULL DEFAULT '',
  series_id TEXT NOT NULL DEFAULT '',
  event_link TEXT NOT NULL DEFAULT '',
  attendees TEXT NOT NULL DEFAULT '[]',
  detected_at INTEGER NOT NULL,
  started_at INTEGER NOT NULL DEFAULT 0,
  ended_at INTEGER NOT NULL DEFAULT 0,
  pid INTEGER NOT NULL DEFAULT 0,
  audio_dir TEXT NOT NULL DEFAULT '',
  audio_deleted INTEGER NOT NULL DEFAULT 0,
  user_started INTEGER NOT NULL DEFAULT 0,
  language TEXT NOT NULL DEFAULT '',
  error TEXT NOT NULL DEFAULT '',
  action_id TEXT NOT NULL DEFAULT '',
  run_id TEXT NOT NULL DEFAULT '',
  segments TEXT NOT NULL DEFAULT '[]',
  speakers TEXT NOT NULL DEFAULT '{}');
CREATE INDEX meetings_detected ON meetings(detected_at);
CREATE TABLE meeting_series(
  series_id TEXT PRIMARY KEY,
  title TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL);
)sql"},
        // client/skills Suggested Triggers (add-skills task 2.4): each Skill version's suggested Triggers are offered once.
        // Numbered after meeting-capture: databases already past 800 would skip a migration in the skills range.
        {1300, R"sql(
CREATE TABLE skill_trigger_offers(
  skill TEXT NOT NULL,
  version TEXT NOT NULL,
  action_id TEXT NOT NULL DEFAULT '',
  state TEXT NOT NULL CHECK(state IN ('offered','accepted','declined')),
  triggers TEXT NOT NULL DEFAULT '[]',
  created_at INTEGER NOT NULL,
  PRIMARY KEY(skill, version));
)sql"},
        // client/user-messaging (add-user-messaging design D4): the Chats as the control plane last listed them, their
        // recent messages, the outbox (pending messages in the order written) and the user's busy choice.
        {1400, R"sql(
CREATE TABLE msg_chats(
  id TEXT PRIMARY KEY,
  data TEXT NOT NULL,
  last_at TEXT NOT NULL DEFAULT '',
  read_dirty INTEGER NOT NULL DEFAULT 0);
CREATE TABLE msg_messages(
  chat TEXT NOT NULL,
  id TEXT NOT NULL,
  seq INTEGER NOT NULL,
  sender TEXT NOT NULL,
  sender_name TEXT NOT NULL DEFAULT '',
  text TEXT NOT NULL,
  at TEXT NOT NULL,
  pending INTEGER NOT NULL DEFAULT 0,
  queued_at INTEGER NOT NULL DEFAULT 0,
  error TEXT NOT NULL DEFAULT '',
  PRIMARY KEY(chat, id));
CREATE INDEX msg_messages_seq ON msg_messages(chat, seq);
CREATE TABLE msg_state(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL);
)sql"},
        // client/work-inbox Chat Entries (add-chats-in-inbox design D1): the Inbox state of each Chat (folder, pin,
        // archive) and what its list entry shows, kept current by the messaging service.
        {1410, R"sql(
CREATE TABLE chat_inbox(
  chat_id TEXT PRIMARY KEY,
  kind TEXT NOT NULL DEFAULT 'team',
  title TEXT NOT NULL DEFAULT '',
  preview TEXT NOT NULL DEFAULT '',
  presence TEXT NOT NULL DEFAULT '',
  last_activity TEXT NOT NULL DEFAULT '',
  unread INTEGER NOT NULL DEFAULT 0,
  folder_id TEXT,
  pinned INTEGER NOT NULL DEFAULT 0,
  archived INTEGER NOT NULL DEFAULT 0);
CREATE INDEX chat_inbox_order ON chat_inbox(pinned, last_activity, chat_id);
)sql"},
    };
    return migrations;
}

}  // namespace wb
