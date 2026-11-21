// Fuzz target for the Teams cache decoders (client/teams; add-local-outlook-and-teams design D4): Snappy blocks,
// LevelDB table blocks, tables, logs, write batches and manifests, IndexedDB keys, Blink/V8 values and message HTML
// must give an answer for any bytes, never a crash, and a decoded value must always be dumpable JSON.
#include "fuzz_main.h"
#include "teams/indexeddb.h"
#include "teams/leveldb.h"
#include "teams/snappy.h"
#include "teams/teams_extract.h"
#include "teams/v8value.h"

using namespace wb;
using namespace wb::teams;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);

    if (auto out = snappy_decompress(bytes, std::size_t{1} << 20)) {
        if (out->size() > (std::size_t{1} << 20)) __builtin_trap();
    }
    (void)parse_block(bytes, [](std::string_view, std::string_view) {});
    (void)read_table(bytes, [](std::string_view, std::string_view) {});
    read_log(bytes, [](std::string_view rec) {
        (void)decode_write_batch(rec, [](std::uint64_t, bool, std::string_view, std::string_view) {});
    });
    (void)parse_manifest(bytes);

    std::string_view key = bytes;
    (void)decode_key_prefix(bytes);
    (void)decode_idb_key(key);

    if (auto v = deserialize_idb_value(bytes)) {
        (void)v->dump();  // throws on invalid UTF-8: every decoded string must be sanitised
        (void)conversation_type(*v, "19:x@thread.v2");
        (void)time_ms(*v);
    }
    (void)html_to_text(bytes);
    return 0;
}
