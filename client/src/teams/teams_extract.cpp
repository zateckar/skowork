// Teams record extraction and message plain text (client/teams; add-local-outlook-and-teams design D4, D5).
#include "teams/teams_extract.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <unordered_map>
#include <utility>

#include "teams/bytes.h"
#include "teams/v8value.h"

namespace wb::teams {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }
bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

// The member at a dotted path ("threadProperties.topic"), or nullptr.
const json* at(const json& j, std::string_view path) {
    const json* cur = &j;
    while (!path.empty()) {
        const auto dot = path.find('.');
        const std::string key(path.substr(0, dot));
        if (!cur->is_object()) return nullptr;
        const auto it = cur->find(key);
        if (it == cur->end()) return nullptr;
        cur = &*it;
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
    }
    return cur;
}

// A string member, or an integer member as decimal text.
std::string text_at(const json& j, std::string_view path) {
    const json* v = at(j, path);
    if (!v) return {};
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number()) {
        if (const auto n = json_int(*v)) return std::to_string(*n);
    }
    return {};
}

std::string first_text(const json& j, std::initializer_list<std::string_view> paths) {
    for (const auto p : paths)
        if (auto s = text_at(j, p); !s.empty()) return s;
    return {};
}

long long first_time(const json& j, std::initializer_list<std::string_view> paths) {
    for (const auto p : paths)
        if (const json* v = at(j, p))
            if (const long long t = time_ms(*v); t > 0) return t;
    return 0;
}

bool is_guid(std::string_view s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

// "8:orgid:<guid>" when the name has a ...:<tenant-guid>:<user-guid> pair, else empty.
std::string user_mri_from_db_name(std::string_view name) {
    std::vector<std::string_view> guids;
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto colon = name.find(':', start);
        const auto part = name.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start);
        if (is_guid(part)) guids.push_back(part);
        if (colon == std::string_view::npos) break;
        start = colon + 1;
    }
    if (guids.size() < 2) return {};
    return "8:orgid:" + lower(guids.back());
}

// A JSON value that may arrive as a JSON-encoded string.
json maybe_parsed(const json* v) {
    if (!v) return nullptr;
    if (v->is_string()) {
        if (auto p = parse_json(v->get<std::string>())) return *p;
        return nullptr;
    }
    return *v;
}

std::string mri_from(std::string s) {
    const auto pos = s.find("/contacts/");  // "https://.../v1/users/ME/contacts/8:orgid:..."
    if (pos != std::string::npos) s = s.substr(pos + 10);
    return s;
}

std::uint64_t fnv1a(std::string_view s, std::uint64_t h = 1469598103934665603ull) {
    for (const char ch : s) {
        h ^= static_cast<std::uint8_t>(ch);
        h *= 1099511628211ull;
    }
    return h;
}

bool parse_message(const json& m, const json& chain, const std::string& map_key, TeamsMessage& out) {
    if (!m.is_object()) return false;
    out.id = first_text(m, {"id", "messageId", "clientMessageId"});
    if (out.id.empty()) out.id = map_key;
    out.conversation_id = first_text(m, {"conversationId", "conversationid"});
    if (out.conversation_id.empty()) out.conversation_id = first_text(chain, {"conversationId", "conversationid"});
    out.at_ms = first_time(m, {"originalArrivalTime", "originalarrivaltime", "composeTime", "composetime",
                               "clientArrivalTime", "arrivalTime", "createdTime"});
    if (out.id.empty() || out.conversation_id.empty() || out.at_ms <= 0) return false;

    out.parent_id = first_text(m, {"parentMessageId", "parentmessageid"});
    if (out.parent_id.empty()) {
        const std::string chain_id = first_text(chain, {"replyChainId", "parentMessageId", "id"});
        if (chain_id != out.id && chain_id != out.conversation_id) out.parent_id = chain_id;
    }
    out.sender_mri = mri_from(first_text(m, {"creator", "from", "creatorProfile.mri", "fromUserId"}));
    out.sender_name = first_text(m, {"imDisplayName", "imdisplayname", "creatorName", "creatorProfile.displayName",
                                     "fromDisplayNameInToken", "senderDisplayName"});

    const json props = maybe_parsed(at(m, "properties"));
    const json empty = json::object();
    const json& p = props.is_object() ? props : empty;
    out.edited_ms = first_time(p, {"edittime", "editTime"});
    if (!out.edited_ms) out.edited_ms = first_time(m, {"editTime", "lastEditTime", "edittime"});
    out.deleted = jbool(m, "isDeleted") || first_time(p, {"deletetime", "deleteTime"}) > 0 ||
                  first_time(m, {"deleteTime", "deletionTime", "deletetime"}) > 0;

    const std::string message_type = first_text(m, {"messageType", "messagetype", "type"});
    const std::string mt = lower(message_type);
    out.system = starts_with(mt, "threadactivity/") || starts_with(mt, "event/") || starts_with(mt, "control/");
    const std::string content_type = lower(first_text(m, {"contentType", "contenttype"}));
    out.html = first_text(m, {"content", "body"});
    if (mt == "text" || content_type == "text") {  // plain text message: kept as is, only trimmed
        const auto b = out.html.find_first_not_of(" \t\r\n");
        const auto e = out.html.find_last_not_of(" \t\r\n");
        out.text = b == std::string::npos ? std::string{} : sanitize_utf8(out.html.substr(b, e - b + 1));
    } else {
        out.text = html_to_text(out.html);
    }

    json mentions = maybe_parsed(at(p, "mentions"));
    if (!mentions.is_array()) mentions = maybe_parsed(at(m, "mentions"));
    if (mentions.is_array()) {
        for (const auto& item : mentions) {
            std::string mri = item.is_string() ? item.get<std::string>() : first_text(item, {"mri", "id", "userId"});
            if (!mri.empty() && std::find(out.mention_mris.begin(), out.mention_mris.end(), mri) == out.mention_mris.end())
                out.mention_mris.push_back(std::move(mri));
        }
    }

    out.version = first_text(m, {"version", "sequenceId"});
    if (out.version.empty()) {
        std::uint64_t h = fnv1a(out.html);
        h = fnv1a(std::to_string(out.edited_ms) + (out.deleted ? "|d" : "|"), h);
        static const char* hex = "0123456789abcdef";
        out.version = "h:";
        for (int i = 15; i >= 0; --i) out.version += hex[(h >> (4 * i)) & 0xf];
    }
    return true;
}

bool parse_conversation(const json& v, TeamsConversation& out) {
    if (!v.is_object()) return false;
    out.id = first_text(v, {"id", "conversationId"});
    if (out.id.empty()) return false;
    out.type = conversation_type(v, out.id);
    out.topic = first_text(v, {"threadProperties.topic", "topic", "threadProperties.spaceThreadTopic", "title"});
    out.last_activity_ms = first_time(v, {"lastMessageTimeUtc", "threadProperties.lastMessageTimeUtc",
                                          "lastMessage.originalArrivalTime", "lastMessage.composeTime", "version"});
    return true;
}

bool parse_profile(const json& v, TeamsProfile& out) {
    if (!v.is_object()) return false;
    out.mri = first_text(v, {"mri", "userMri"});
    if (out.mri.empty()) {
        const std::string id = first_text(v, {"id"});
        if (id.find(':') != std::string::npos) out.mri = id;
    }
    if (out.mri.empty()) return false;
    out.name = first_text(v, {"displayName", "name", "displayname"});
    out.email = first_text(v, {"email", "mail", "userPrincipalName", "upn"});
    if (out.email.empty()) {
        std::string sip = first_text(v, {"sipAddress"});
        if (starts_with(lower(sip), "sip:")) sip = sip.substr(4);
        out.email = sip;
    }
    return true;
}

void append_entity(std::string& out, std::string_view name) {
    struct Named {
        const char* name;
        std::uint32_t cp;
    };
    static const Named table[] = {{"amp", '&'},       {"lt", '<'},         {"gt", '>'},         {"quot", '"'},
                                  {"apos", '\''},     {"nbsp", ' '},       {"ndash", 0x2013},   {"mdash", 0x2014},
                                  {"hellip", 0x2026}, {"lsquo", 0x2018},   {"rsquo", 0x2019},   {"ldquo", 0x201c},
                                  {"rdquo", 0x201d},  {"bull", 0x2022},    {"middot", 0xb7},    {"copy", 0xa9},
                                  {"reg", 0xae},      {"trade", 0x2122},   {"euro", 0x20ac},    {"zwj", 0x200d},
                                  {"zwnj", 0x200c}};
    for (const auto& e : table)
        if (name == e.name) {
            append_utf8(out, e.cp);
            return;
        }
}

bool known_entity(std::string_view name) {
    static const char* names[] = {"amp",    "lt",    "gt",    "quot",  "apos",  "nbsp",  "ndash", "mdash",
                                  "hellip", "lsquo", "rsquo", "ldquo", "rdquo", "bull",  "middot", "copy",
                                  "reg",    "trade", "euro",  "zwj",   "zwnj"};
    for (const char* n : names)
        if (name == n) return true;
    return false;
}

bool is_block_tag(std::string_view name) {
    static const char* names[] = {"br", "p",  "div", "li", "tr", "h1",         "h2",  "h3",  "h4", "h5",
                                  "h6", "ul", "ol",  "table", "blockquote", "pre", "hr", "section"};
    for (const char* n : names)
        if (name == n) return true;
    return false;
}

std::string collapse_whitespace(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    bool pending_space = false, pending_newline = false;
    for (const char ch : s) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') {
            pending_space = true;
        } else if (ch == '\n') {
            pending_newline = true;
        } else {
            if (!res.empty()) {
                if (pending_newline) res += '\n';
                else if (pending_space) res += ' ';
            }
            pending_space = pending_newline = false;
            res += ch;
        }
    }
    return res;
}

}  // namespace

long long time_ms(const json& v) {
    if (v.is_number()) return json_int(v).value_or(0);
    if (!v.is_string()) return 0;
    const std::string& s = v.get_ref<const std::string&>();
    if (s.empty()) return 0;
    if (std::all_of(s.begin(), s.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
        if (s.size() > 18) return 0;
        long long n = 0;
        for (const char ch : s) n = n * 10 + (ch - '0');
        return n;
    }
    // ISO 8601: YYYY-MM-DDTHH:MM:SS[.fff][Z|+HH:MM|-HH:MM]
    auto num = [&](std::size_t pos, std::size_t len, int& out) {
        if (pos + len > s.size()) return false;
        out = 0;
        for (std::size_t i = pos; i < pos + len; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    };
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!num(0, 4, y) || s.size() < 19 || s[4] != '-' || !num(5, 2, mo) || s[7] != '-' || !num(8, 2, d) ||
        (s[10] != 'T' && s[10] != ' ') || !num(11, 2, h) || s[13] != ':' || !num(14, 2, mi) || s[16] != ':' ||
        !num(17, 2, se))
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return 0;
    std::size_t pos = 19;
    long long frac_ms = 0;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        int digits = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            if (digits < 3) frac_ms = frac_ms * 10 + (s[pos] - '0');
            ++digits;
            ++pos;
        }
        for (int k = digits; k < 3; ++k) frac_ms *= 10;
    }
    long long offset_min = 0;
    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        int oh = 0, om = 0;
        const bool neg = s[pos] == '-';
        if (!num(pos + 1, 2, oh)) return 0;
        std::size_t mpos = pos + 3;
        if (mpos < s.size() && s[mpos] == ':') ++mpos;
        if (!num(mpos, 2, om)) om = 0;
        offset_min = (neg ? -1 : 1) * (oh * 60LL + om);
    }
    // Days from civil (Howard Hinnant).
    const long long yy = y - (mo <= 2 ? 1 : 0);
    const long long era = (yy >= 0 ? yy : yy - 399) / 400;
    const long long yoe = yy - era * 400;
    const long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + doe - 719468;
    return ((days * 24 + h) * 60 + mi - offset_min) * 60000LL + se * 1000LL + frac_ms;
}

std::string conversation_type(const json& conversation, std::string_view id) {
    const std::string tt = lower(first_text(conversation, {"threadProperties.threadType", "threadType"}));
    const std::string pt = lower(first_text(conversation, {"threadProperties.productThreadType", "productThreadType"}));
    const auto one_on_one = [&] {
        if (ends_with(id, "@unq.gbl.spaces")) return true;
        const std::string unique = lower(first_text(conversation, {"threadProperties.uniquerosterthread"}));
        const json* tp = at(conversation, "threadProperties");
        if (unique == "true" || (tp && jbool(*tp, "uniquerosterthread"))) return true;
        if (const json* members = at(conversation, "members"); members && members->is_array() && members->size() == 2)
            return true;
        const std::string count = first_text(conversation, {"threadProperties.memberCount", "memberCount"});
        return count == "2";
    };
    if (contains(pt, "oneonone")) return "chat";
    if (contains(pt, "meeting") || tt == "meeting") return "meeting";
    if (contains(pt, "channel") || contains(pt, "space") || contains(pt, "topic") || tt == "space" || tt == "topic" ||
        tt == "channel")
        return "channel";
    if (tt == "chat" || contains(pt, "chat") || contains(pt, "group")) return one_on_one() ? "chat" : "group";
    if (starts_with(id, "19:meeting_")) return "meeting";
    if (ends_with(id, "@unq.gbl.spaces")) return "chat";
    if (contains(id, "@thread.tacv2") || contains(id, "@thread.skype")) return "channel";
    if (contains(id, "@thread.v2")) return "group";
    return "other";
}

std::string html_to_text(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    std::size_t i = 0;
    while (i < html.size()) {
        const char ch = html[i];
        if (ch == '<') {
            const auto close = html.find('>', i + 1);
            if (close == std::string_view::npos) {  // not a tag: keep the rest as text
                out.append(html.substr(i));
                break;
            }
            std::string_view tag = html.substr(i + 1, close - i - 1);
            const bool closing = !tag.empty() && tag.front() == '/';
            if (closing) tag.remove_prefix(1);
            std::size_t n = 0;
            while (n < tag.size() && std::isalnum(static_cast<unsigned char>(tag[n]))) ++n;
            const std::string name = lower(tag.substr(0, n));
            i = close + 1;
            if (!closing && (name == "script" || name == "style")) {
                const std::string end_tag = "</" + name;
                std::size_t end = std::string_view::npos;
                for (std::size_t k = html.find("</", i); k != std::string_view::npos; k = html.find("</", k + 2)) {
                    if (lower(html.substr(k, end_tag.size())) == end_tag) {
                        end = k;
                        break;
                    }
                }
                if (end == std::string_view::npos) break;
                const auto gt = html.find('>', end);
                i = gt == std::string_view::npos ? html.size() : gt + 1;
                continue;
            }
            if (name == "at" && !closing) out += '@';
            else if (is_block_tag(name)) out += '\n';
            continue;
        }
        if (ch == '&') {
            const auto semi = html.find(';', i + 1);
            if (semi != std::string_view::npos && semi - i <= 10 && semi > i + 1) {
                const std::string_view name = html.substr(i + 1, semi - i - 1);
                if (name.front() == '#') {
                    std::uint32_t cp = 0;
                    bool ok = name.size() > 1;
                    const bool hex = ok && (name[1] == 'x' || name[1] == 'X');
                    const std::size_t start = hex ? 2 : 1;
                    if (start >= name.size()) ok = false;
                    for (std::size_t k = start; ok && k < name.size(); ++k) {
                        const auto c = static_cast<unsigned char>(name[k]);
                        int digit = -1;
                        if (c >= '0' && c <= '9') digit = c - '0';
                        else if (hex && std::isxdigit(c)) digit = std::tolower(c) - 'a' + 10;
                        if (digit < 0) ok = false;
                        else cp = std::min<std::uint32_t>(cp * (hex ? 16 : 10) + static_cast<std::uint32_t>(digit), 0x110000);
                    }
                    if (ok) {
                        if (cp == 0xa0) out += ' ';
                        else append_utf8(out, cp == 0 ? 0xfffd : cp);
                        i = semi + 1;
                        continue;
                    }
                } else if (known_entity(name)) {
                    append_entity(out, name);
                    i = semi + 1;
                    continue;
                }
            }
        }
        out += ch;
        ++i;
    }
    return collapse_whitespace(sanitize_utf8(out));
}

Extraction extract(const IdbCatalog& catalog) {
    Extraction ex;
    for (const auto& d : catalog.databases()) {
        if (!contains(d.name, "conversation-manager") && !contains(d.name, "replychain-manager")) continue;
        if (auto mri = user_mri_from_db_name(d.name); !mri.empty()) {
            ex.user_mri = std::move(mri);
            break;
        }
    }
    if (ex.user_mri.empty())
        for (const auto& d : catalog.databases())
            if (auto mri = user_mri_from_db_name(d.name); !mri.empty()) {
                ex.user_mri = std::move(mri);
                break;
            }

    auto decode = [&ex](const IdbRecord& rec) -> std::optional<json> {
        auto v = deserialize_idb_value(rec.value);
        if (v) return std::move(v.value());
        if (v.error().code == "blob") ++ex.blobs;
        else ++ex.decode_errors;
        return std::nullopt;
    };

    std::unordered_map<std::string, std::size_t> conv_index;
    catalog.for_each_record("conversation-manager", "conversations", [&](const IdbRecord& rec) {
        const auto v = decode(rec);
        if (!v) return;
        TeamsConversation c;
        if (!parse_conversation(*v, c)) {
            ++ex.skipped;
            return;
        }
        const auto [it, inserted] = conv_index.emplace(c.id, ex.conversations.size());
        if (inserted) ex.conversations.push_back(std::move(c));
        else if (c.last_activity_ms > ex.conversations[it->second].last_activity_ms) ex.conversations[it->second] = std::move(c);
    });

    std::unordered_map<std::string, std::size_t> msg_index;
    auto add_message = [&](TeamsMessage m) {
        const std::string key = m.conversation_id + '\n' + m.id;
        const auto [it, inserted] = msg_index.emplace(key, ex.messages.size());
        if (inserted) ex.messages.push_back(std::move(m));
        else if (m.edited_ms >= ex.messages[it->second].edited_ms) ex.messages[it->second] = std::move(m);
    };
    catalog.for_each_record("replychain-manager", "replychains", [&](const IdbRecord& rec) {
        const auto v = decode(rec);
        if (!v) return;
        const json* map = at(*v, "messageMap");
        if (!map || !(map->is_object() || map->is_array())) map = at(*v, "messages");
        if (!map || !(map->is_object() || map->is_array())) {
            TeamsMessage m;  // a record that is a single message
            if (parse_message(*v, json::object(), std::string{}, m)) add_message(std::move(m));
            else ++ex.skipped;
            return;
        }
        if (map->is_object()) {
            for (auto it = map->begin(); it != map->end(); ++it) {
                TeamsMessage m;
                if (parse_message(it.value(), *v, it.key(), m)) add_message(std::move(m));
                else ++ex.skipped;
            }
        } else {
            for (const auto& item : *map) {
                TeamsMessage m;
                if (parse_message(item, *v, std::string{}, m)) add_message(std::move(m));
                else ++ex.skipped;
            }
        }
    });

    std::unordered_map<std::string, std::size_t> profile_index;
    catalog.for_each_record("profiles", "profiles", [&](const IdbRecord& rec) {
        const auto v = decode(rec);
        if (!v) return;
        TeamsProfile p;
        if (!parse_profile(*v, p)) {
            ++ex.skipped;
            return;
        }
        const auto [it, inserted] = profile_index.emplace(p.mri, ex.profiles.size());
        if (inserted) ex.profiles.push_back(std::move(p));
    }, /*store_substring=*/true);
    return ex;
}

}  // namespace wb::teams
