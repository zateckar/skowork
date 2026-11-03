// Retrieval benchmark for client/knowledge-graph (add-knowledge-graph spike 1.1, tasks 5.1 and 2.3): builds an encrypted
// kg.db of N synthetic chunks in cs, de and en, grouped by topic, with 1024- or 4096-dimension vectors near their topic's
// centroid, then runs the service's hybrid search (FTS, vector, graph expansion, fusion, reranking) for a query set and
// reports local latency percentiles, storage per chunk, the core process's memory and topic precision. The query
// embedding and the reranker are answered in process, so the numbers exclude the two gateway round trips; add them
// from the gateway measurement. Not a ctest test: run it by hand.
//
//   kg_bench --chunks 400000 [--dim 1024] [--queries 200] [--cores 4] [--keep DIR] [--reuse DIR]
#include "knowledge/kg_store.h"
#include "knowledge/knowledge_service.h"
#include "platform/crypto.h"
#include "platform/win.h"
#include "storage/data_root.h"
#include "storage/database.h"
#include "storage/keystore.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

using namespace wb;
using namespace wb::knowledge;

namespace {

constexpr int kTopics = 2000, kTopicWords = 24, kCommon = 3000, kChunksPerItem = 10, kWordsPerChunk = 110;

std::string word(std::mt19937_64& r, int lang) {
    static const char* syll[3][12] = {{"pra", "ce", "zpr", "ava", "roz", "poc", "ty", "ne", "lin", "ka", "dí", "ly"},
                                      {"be", "richt", "an", "lage", "wer", "kung", "ver", "trag", "zeit", "plan", "ung", "en"},
                                      {"re", "port", "line", "main", "ten", "ance", "sup", "ply", "con", "tract", "ion", "er"}};
    std::string w;
    const int n = 2 + static_cast<int>(r() % 3);
    for (int i = 0; i < n; ++i) w += syll[lang][r() % 12];
    return w;
}

struct World {
    std::vector<std::vector<std::string>> topic_words;  // per topic
    std::vector<std::string> common;
    std::vector<std::vector<float>> centroid;
    int dim = 1024;
};

std::vector<float> normalized(std::vector<float> v) {
    double n = 0;
    for (float x : v) n += static_cast<double>(x) * x;
    const float k = n > 0 ? static_cast<float>(1.0 / std::sqrt(n)) : 1.0f;
    for (float& x : v) x *= k;
    return v;
}

std::vector<float> around(const World& w, int topic, double noise, std::mt19937_64& r) {
    std::normal_distribution<float> g(0.0f, 1.0f);
    std::vector<float> v(static_cast<size_t>(w.dim));
    const float s = static_cast<float>(noise / std::sqrt(static_cast<double>(w.dim)));
    for (size_t i = 0; i < v.size(); ++i) v[i] = w.centroid[static_cast<size_t>(topic)][i] + s * g(r);
    return normalized(std::move(v));
}

World make_world(int dim) {
    World w;
    w.dim = dim;
    std::mt19937_64 r(42);
    std::normal_distribution<float> g(0.0f, 1.0f);
    for (int i = 0; i < kCommon; ++i) w.common.push_back(word(r, i % 3));
    for (int t = 0; t < kTopics; ++t) {
        std::vector<std::string> ws;
        for (int i = 0; i < kTopicWords; ++i) ws.push_back(word(r, t % 3) + std::to_string(t));
        w.topic_words.push_back(std::move(ws));
        std::vector<float> c(static_cast<size_t>(dim));
        for (float& x : c) x = g(r);
        w.centroid.push_back(normalized(std::move(c)));
    }
    return w;
}

long long private_mb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return static_cast<long long>(pmc.PrivateUsage / (1024 * 1024));
}

long long working_set_mb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return static_cast<long long>(pmc.WorkingSetSize / (1024 * 1024));
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
    return v[i];
}

}  // namespace

int main(int argc, char** argv) {
    long long chunks = 400000;
    int dim = 1024, queries = 200, cores = 0;
    std::string keep, reuse;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i], v = argv[i + 1];
        if (a == "--chunks") chunks = std::stoll(v);
        else if (a == "--dim") dim = std::stoi(v);
        else if (a == "--queries") queries = std::stoi(v);
        else if (a == "--cores") cores = std::stoi(v);
        else if (a == "--keep") keep = v;
        else if (a == "--reuse") reuse = v;
    }
    if (cores > 0) ::SetProcessAffinityMask(::GetCurrentProcess(), (1ull << cores) - 1);
    const World w = make_world(dim);

    const std::filesystem::path dir = !reuse.empty() ? std::filesystem::path(widen(reuse))
                                      : !keep.empty() ? std::filesystem::path(widen(keep))
                                                      : std::filesystem::path(local_appdata()) / L"workbench-test" / widen("kgbench-" + random_urlsafe(6));
    auto root = DataRoot::open(dir);
    if (!root) return std::fprintf(stderr, "data root: %s\n", root.error().message.c_str()), 1;
    KeyStore keys(root->keys());
    (void)keys.load();
    auto key = keys.get_or_create("kg.db");
    if (!key) return 1;
    MigrationOutcome mo;
    auto db = open_and_migrate(root->file("kg.db"), *key, kg_migrations(), mo);
    if (!db) return std::fprintf(stderr, "kg.db: %s\n", db.error().message.c_str()), 1;
    (void)(*db)->exec("PRAGMA synchronous=OFF;");  // the build only; queries are what is measured
    auto store = std::make_unique<KgStore>(std::move(*db));
    const std::string model = "bench-embedding";
    std::mt19937_64 r(7);

    if (reuse.empty()) {
        const auto t0 = std::chrono::steady_clock::now();
        long long made = 0;
        int item = 0;
        while (made < chunks) {
            ItemInput in;
            in.source = "files";
            in.ext_id = "doc" + std::to_string(item);
            in.title = "Document " + std::to_string(item);
            in.ref = "C:\\Docs\\doc" + std::to_string(item) + ".md";
            in.hash = std::to_string(item);
            in.created_at = in.updated_at = 1'790'000'000'000LL - item * 60'000LL;
            in.priority = 0;
            in.extract = false;
            const int topic = static_cast<int>(r() % kTopics);
            for (int c = 0; c < kChunksPerItem && made < chunks; ++c, ++made) {
                std::string text;
                for (int k = 0; k < kWordsPerChunk; ++k) {
                    text += (r() % 10 < 3) ? w.topic_words[static_cast<size_t>(topic)][r() % kTopicWords] : w.common[r() % kCommon];
                    text += ' ';
                }
                Chunk ch;
                ch.heading = "Section " + std::to_string(c);
                ch.text = text;
                in.chunks.push_back(ch);
            }
            auto id = store->put_item(in, in.updated_at);
            if (!id) return std::fprintf(stderr, "put_item: %s\n", id.error().message.c_str()), 1;
            std::vector<std::pair<long long, std::vector<float>>> vecs;
            for (const auto& c : store->chunks_of_item(*id)) vecs.emplace_back(c.id, around(w, topic, 0.9, r));
            store->store_vectors(vecs, model, dim);
            store->set_meta("bench_topic_" + std::to_string(*id), std::to_string(topic));
            ++item;
            if (item % 10000 == 0) std::fprintf(stderr, "  %lld chunks\n", made);
        }
        store->set_active_embedding(model, dim);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("built %lld chunks in %.0f s\n", made, secs);
    }
    (void)store->db().exec("PRAGMA synchronous=NORMAL;");

    std::error_code ec;
    const auto bytes = std::filesystem::file_size(root->file("kg.db"), ec);
    const long long n_chunks = store->counts("files").chunks;
    std::printf("kg.db %.1f MB, %.0f bytes per chunk (dim %d)\n", static_cast<double>(bytes) / 1e6,
                static_cast<double>(bytes) / static_cast<double>(std::max(1LL, n_chunks)), dim);

    // The service over the store, answering the query embedding and the reranker in process.
    std::vector<float> next_query;
    KnowledgeEnv e;
    e.clock = &system_clock();
    e.policy = [](const std::string&) -> std::optional<json> { return std::nullopt; };
    e.background = false;
    e.embed = [&](const std::vector<std::string>& texts, const model::RequestContext&) -> model::ModelResult<model::EmbedResult> {
        model::EmbedResult res;
        for (size_t i = 0; i < texts.size(); ++i) res.vectors.push_back(next_query);
        res.model = model;
        res.dimension = dim;
        return res;
    };
    e.rerank = [](const std::string&, const std::vector<std::string>& docs, const model::RequestContext&) -> model::ModelResult<std::vector<double>> {
        std::vector<double> s;
        for (size_t i = 0; i < docs.size(); ++i) s.push_back(1.0 / static_cast<double>(i + 1));
        return s;
    };
    KnowledgeService svc(std::move(e), std::move(store));
    auto& st = svc.store();
    const long long mem_before = private_mb();

    std::vector<double> total_ms, vec_ms, fts_ms;
    int hits = 0, returned = 0;
    for (int q = 0; q < queries + 5; ++q) {
        const int topic = static_cast<int>(r() % kTopics);
        std::string text;
        for (int k = 0; k < 3; ++k) text += w.topic_words[static_cast<size_t>(topic)][r() % kTopicWords] + " ";
        next_query = around(w, topic, 0.5, r);
        const auto a = std::chrono::steady_clock::now();
        (void)st.fts(text, 50);
        const auto b = std::chrono::steady_clock::now();
        (void)st.vector_search(next_query, model, dim, 50);
        const auto c = std::chrono::steady_clock::now();
        const auto res = svc.search(text, {});
        const auto d = std::chrono::steady_clock::now();
        if (q < 5) continue;  // warm-up
        fts_ms.push_back(std::chrono::duration<double, std::milli>(b - a).count());
        vec_ms.push_back(std::chrono::duration<double, std::milli>(c - b).count());
        total_ms.push_back(std::chrono::duration<double, std::milli>(d - c).count());
        for (const auto& p : res.passages) {
            ++returned;
            if (st.meta("bench_topic_" + std::to_string(p.item_id)) == std::to_string(topic)) ++hits;
        }
    }
    std::printf("queries %d, cores %s\n", queries, cores > 0 ? std::to_string(cores).c_str() : "all");
    std::printf("hybrid search (local): p50 %.0f ms, p95 %.0f ms, max %.0f ms\n", pct(total_ms, 0.5), pct(total_ms, 0.95), pct(total_ms, 1.0));
    std::printf("  fts alone: p50 %.0f ms, p95 %.0f ms; vector alone: p50 %.0f ms, p95 %.0f ms\n", pct(fts_ms, 0.5), pct(fts_ms, 0.95),
                pct(vec_ms, 0.5), pct(vec_ms, 0.95));
    std::printf("topic precision of returned passages: %.2f (%d of %d)\n", returned ? static_cast<double>(hits) / returned : 0.0, hits, returned);
    std::printf("core memory: private %lld MB before queries, %lld MB after; working set %lld MB\n", mem_before, private_mb(), working_set_mb());
    if (keep.empty() && reuse.empty()) {
        svc.stop();
        std::filesystem::remove_all(dir, ec);
    }
    return 0;
}
