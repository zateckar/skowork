#include "meeting/speech_archive.h"

#include "base/strings.h"

#include <miniz.h>

#include <map>
#include <set>

namespace wb::meeting {
namespace {

size_t read_cb(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
    const auto* read = static_cast<const SpeechArchiveRead*>(opaque);
    return (*read)(ofs, buf, n);
}

struct Sink {
    const std::function<bool(const char*, size_t)>* put = nullptr;
    const std::function<bool()>* cancelled = nullptr;
    std::uint64_t expected = 0, written = 0;
    bool failed = false;
};

size_t sink_write(void* opaque, mz_uint64, const void* buf, size_t n) {
    auto* s = static_cast<Sink*>(opaque);
    if (n == 0) return 0;
    if (*s->cancelled && (*s->cancelled)()) return s->failed = true, 0;
    // An entry that inflates past its declared size is refused rather than grown.
    if (n > s->expected - s->written || !(*s->put)(static_cast<const char*>(buf), n)) return s->failed = true, 0;
    s->written += n;
    return n;
}

}  // namespace

struct SpeechArchive::Impl {
    SpeechArchiveRead read;
    mz_zip_archive zip{};
    bool open = false;
    ~Impl() {
        if (open) mz_zip_reader_end(&zip);
    }
};

SpeechArchive::SpeechArchive() : impl_(std::make_unique<Impl>()) {}
SpeechArchive::~SpeechArchive() = default;

Status SpeechArchive::open(SpeechArchiveRead read, std::uint64_t size) {
    if (impl_->open) return make_error("speech_package", "the archive is already open");
    impl_->read = std::move(read);
    impl_->zip.m_pRead = read_cb;
    impl_->zip.m_pIO_opaque = &impl_->read;
    if (!mz_zip_reader_init(&impl_->zip, size, 0)) return make_error("speech_package", "the speech package is not a zip archive");
    impl_->open = true;
    return {};
}

Status SpeechArchive::extract(const SpeechArchiveEntry& e, const std::function<bool(const char*, size_t)>& put,
                              const std::function<bool()>& cancelled) {
    if (!impl_->open) return make_error("speech_package", "the archive is not open");
    Sink s;
    s.put = &put;
    s.cancelled = &cancelled;
    s.expected = e.size;
    const bool ok = e.size == 0 || mz_zip_reader_extract_to_callback(&impl_->zip, e.index, sink_write, &s, 0) != 0;
    if (cancelled && cancelled()) return make_error("cancelled", "the speech package was cancelled");
    if (!ok || s.failed || s.written != e.size) return make_error("speech_package", "an archive entry cannot be read: " + e.name);
    return {};
}

Result<SpeechArchivePlan> SpeechArchive::inspect(const SpeechLimits& lim, const std::function<bool()>& cancelled) {
    if (!impl_->open) return make_error("speech_package", "the archive is not open");
    mz_zip_archive& zip = impl_->zip;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    if (n == 0 || n > lim.max_entries) return make_error("speech_package", "the archive has an unsupported number of entries");
    SpeechArchivePlan plan;
    std::set<std::string> seen;
    const SpeechArchiveEntry* manifest_entry = nullptr;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) return make_error("speech_package", "an archive entry cannot be read");
        SpeechArchiveEntry e{st.m_filename, i, st.m_uncomp_size, -1};
        if (st.m_is_directory || st.m_is_encrypted || !st.m_is_supported)
            return make_error("speech_package", "the archive has a directory, encrypted or unsupported entry");
        // Unix file type in the high 16 bits of the external attributes: only regular files (or none) are allowed.
        const mz_uint type = (st.m_external_attr >> 16) & 0170000u;
        if (type != 0 && type != 0100000u) return make_error("speech_package", "the archive has a link or special file: " + e.name);
        // FILE_ATTRIBUTE_REPARSE_POINT from a Windows archiver.
        if (st.m_external_attr & 0x400u) return make_error("speech_package", "the archive has a link: " + e.name);
        if (auto s = check_speech_entry_name(e.name); !s.ok()) return s.error();
        if (e.size > lim.max_entry) return make_error("speech_package", "an archive entry is too large: " + e.name);
        plan.total += e.size;
        if (plan.total > lim.max_total) return make_error("speech_package", "the archive inflates beyond the speech budget");
        if (!seen.insert(to_lower(e.name)).second) return make_error("speech_package", "the archive has a duplicate entry: " + e.name);
        plan.entries.push_back(std::move(e));
    }
    for (const auto& e : plan.entries)
        if (e.name == "manifest.json") manifest_entry = &e;
    if (!manifest_entry || manifest_entry->size == 0 || manifest_entry->size > kSpeechManifestMaxBytes)
        return make_error("speech_package", "the speech package has no usable manifest.json");
    std::string text;
    if (auto st = extract(*manifest_entry, [&](const char* p, size_t k) { text.append(p, k); return true; }, cancelled); !st.ok())
        return st.error();
    auto manifest = parse_speech_manifest(text);
    if (!manifest) return manifest.error();
    plan.manifest = std::move(*manifest);

    // Every archive file is either the manifest, a license, or a listed file under its exact path and size; every
    // listed file is present.
    std::map<std::string, int> listed;
    for (size_t i = 0; i < plan.manifest.files.size(); ++i) listed[to_lower(plan.manifest.files[i].path)] = static_cast<int>(i);
    size_t matched = 0;
    for (auto& e : plan.entries) {
        if (e.name == "manifest.json" || e.name.rfind("licenses/", 0) == 0) continue;
        const auto it = listed.find(to_lower(e.name));
        if (it == listed.end() || plan.manifest.files[static_cast<size_t>(it->second)].path != e.name)
            return make_error("speech_package", "the archive has a file the manifest does not list: " + e.name);
        if (plan.manifest.files[static_cast<size_t>(it->second)].bytes != e.size)
            return make_error("speech_package", "a file does not match the manifest: " + e.name);
        e.listed = it->second;
        ++matched;
    }
    if (matched != plan.manifest.files.size()) return make_error("speech_package", "the manifest lists a file the archive does not contain");
    return plan;
}

}  // namespace wb::meeting
