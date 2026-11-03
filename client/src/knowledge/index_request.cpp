// The index worker's requests (add-knowledge-graph design D7): reading plain-text and source files and chunking every
// kind of content run in workbench-worker.exe --role index, at below-normal priority under its memory budget.
#include "knowledge/core.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace wb::knowledge {

json chunks_json(const std::vector<Chunk>& chunks) {
    json out = json::array();
    for (const auto& c : chunks) out.push_back({{"heading", c.heading}, {"text", c.text}, {"at", c.at}});
    return out;
}

std::vector<Chunk> chunks_from_json(const json& j) {
    std::vector<Chunk> out;
    if (!j.is_array()) return out;
    for (const auto& c : j) {
        Chunk x{jstr(c, "heading"), jstr(c, "text"), jint(c, "at")};
        if (!x.text.empty()) out.push_back(std::move(x));
    }
    return out;
}

std::string content_hash(std::string_view bytes) {
    std::uint64_t h = 1469598103934665603ull;
    for (char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

namespace {

std::vector<Turn> turns_of(const json& j) {
    std::vector<Turn> out;
    if (!j.is_array()) return out;
    for (const auto& t : j) out.push_back({jstr(t, "speaker"), jstr(t, "text"), jint(t, "at")});
    return out;
}

}  // namespace

Result<json> index_request(const std::string& method, const json& params) {
    if (method == "index.chunk") {
        const std::string kind = jstr(params, "kind");
        std::vector<Chunk> chunks;
        if (kind == "transcript") chunks = chunk_transcript(turns_of(params.value("turns", json::array())));
        else if (kind == "messages") chunks = chunk_messages(turns_of(params.value("turns", json::array())));
        else chunks = chunk_text(kind, jstr(params, "text"));
        return json{{"chunks", chunks_json(chunks)}};
    }
    if (method == "index.read") {
        const std::string path = jstr(params, "path"), kind = jstr(params, "kind");
        if (kind != "md" && kind != "txt" && kind != "code") return make_error("unsupported", "not a plain-text or source file");
        const long long max = jint(params, "max_bytes", 25ll << 20);
        std::error_code ec;
        const std::filesystem::path p(std::u8string(path.begin(), path.end()));
        const auto size = std::filesystem::file_size(p, ec);
        if (ec) return make_error("read_failed", ec.message());
        if (static_cast<long long>(size) > max) return make_error("too_large", "larger than the size limit");
        std::ifstream in(p, std::ios::binary);
        if (!in) return make_error("read_failed", "cannot open the file");
        std::string bytes(static_cast<size_t>(size), '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(size));
        bytes.resize(static_cast<size_t>(in.gcount()));
        if (looks_binary(bytes)) return make_error("binary", "the file is not text");
        const std::string text = decode_text(bytes);
        return json{{"chunks", chunks_json(chunk_text(kind, text))}, {"hash", content_hash(bytes)}, {"bytes", bytes.size()}};
    }
    return make_error("unknown_method", "the index worker has no method " + method);
}

}  // namespace wb::knowledge
