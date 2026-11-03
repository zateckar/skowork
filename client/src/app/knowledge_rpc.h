// The Knowledge sources settings section's host methods (client/knowledge-graph Knowledge Status And Control) and the
// Knowledge view's (add-knowledge-sharing: documents, notes, browse, item, search, curation, sharing), UI only, and the
// mail-calendar items as the knowledge connectors index them.
#pragma once

#include "host/rpc.h"
#include "knowledge/knowledge_service.h"
#include "mail/mail_store.h"
#include "runtime/run_store.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wb {

// `service`: the running knowledge service, or null. `forget`: the forget cascade of client/local-storage. `open_file`
// opens a document in its default application; knowledge.open calls it only for an indexed item of the files Source.
void register_knowledge_rpc(RpcRegistry& rpc, std::function<knowledge::KnowledgeService*()> service,
                            std::function<Status(const std::string& type, const std::string& id)> forget,
                            std::function<Status(const std::string& path)> open_file = {});

// A message (kind mail) or calendar event (kind calendar) of the mail-calendar local copy for the knowledge connectors:
// {title, body, at, ref, location, withheld, people}. Private events keep only their times and are reported withheld.
std::optional<json> knowledge_mail_item(mail::MailStore& store, const std::string& kind, const std::string& id);
std::vector<std::string> knowledge_mail_ids(mail::MailStore& store, const std::string& kind);
// A message with its header for Save as note (add-save-as-note): {subject, from, to, cc, at, body, withheld}.
std::optional<json> knowledge_mail_message(mail::MailStore& store, const std::string& id);

// A Thread for the threads connector: {title, messages: [{role, text, at}]} with the user's and the assistant's messages
// (tool calls and results are not indexed); nullopt when the Thread does not exist.
std::optional<json> knowledge_thread_item(runtime::RunStore& runs, const std::string& thread_id);

}  // namespace wb
