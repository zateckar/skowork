#include "eval/jsonl.h"

namespace wb::eval {

std::string jsonl_line(const json& record) {
    try {
        // Line breaks inside strings are escaped by dump(); invalid UTF-8 becomes U+FFFD.
        return record.dump(-1, ' ', false, json::error_handler_t::replace);
    } catch (const std::exception&) {
        return json{{"error", "record could not be serialized"}}.dump();
    }
}

Result<JsonlWriter> JsonlWriter::open(const std::filesystem::path& file) {
    JsonlWriter w;
    w.file_ = file;
    w.out_ = std::make_unique<std::ofstream>(file, std::ios::binary | std::ios::trunc);
    if (!*w.out_) return make_error("write_failed", "Cannot write " + file.filename().string());
    return w;
}

Status JsonlWriter::write(const json& record) {
    if (!out_ || !*out_) return make_error("write_failed", "Cannot write " + file_.filename().string());
    const std::string line = jsonl_line(record);
    out_->write(line.data(), static_cast<std::streamsize>(line.size()));
    out_->put('\n');
    if (!*out_) return make_error("write_failed", "Cannot write " + file_.filename().string() + " (is the disk full?)");
    ++lines_;
    return {};
}

Status JsonlWriter::close() {
    if (!out_) return {};
    out_->flush();
    const bool ok = static_cast<bool>(*out_);
    out_->close();
    out_.reset();
    if (!ok) return make_error("write_failed", "Cannot write " + file_.filename().string());
    return {};
}

}  // namespace wb::eval
