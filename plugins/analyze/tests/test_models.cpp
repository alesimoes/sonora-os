// SPDX-License-Identifier: GPL-3.0-or-later
//
// Manifest loader, streaming digest, policy and size bounds, runner stub
// (ADR-0066 §3). Refusals are checked together with their messages and with
// "the factory was never asked", i.e. no fallback.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "model_loader.h"
#include "sonora/analyze/analyze_fakes.h"
#include "sonora/sha256.h"

namespace {

using namespace sonora::analyze;
namespace fs = std::filesystem;

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void test_sha256_nist() {
    check(sonora::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    check(sonora::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    check(sonora::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "448-bit message");
    sonora::Sha256 h;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(chunk.data(), chunk.size());
    check(h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "million a");
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("sonora_analyze_models_" + std::to_string(std::rand()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string write(const std::string& name, const std::string& bytes) const {
        std::ofstream(path / name, std::ios::binary) << bytes;
        return (path / name).string();
    }
};

ManifestEntry entry_for(const std::string& bytes, WeightsStatus status) {
    ManifestEntry e;
    e.name = "m";
    e.file = "m.onnx";
    e.sha256 = sonora::sha256_hex(bytes);
    e.size_bytes = static_cast<std::int64_t>(bytes.size());
    e.weights_status = status;
    e.outputs = {"y"};
    return e;
}

void test_fs_digest() {
    TempDir dir;
    const std::string bytes(200000, 'q');  // several read blocks
    const std::string path = dir.write("f", bytes);
    FsFileDigest d;
    std::int64_t size = 0;
    std::string hex, error;
    check(d.digest(path, size, hex, error), "digest ok");
    check(size == 200000 && hex == sonora::sha256_hex(bytes), "streaming digest equals one-shot");
    check(!d.digest((dir.path / "none").string(), size, hex, error) && has(error, "cannot open"), "missing file");
    FsFileDigest small(1000);
    check(!small.digest(path, size, hex, error) && has(error, "exceeds"), "oversize file refused");
}

void test_load_model_refusals() {
    TempDir dir;
    const std::string bytes = "pretend weights";
    dir.write("m.onnx", bytes);
    ModelPolicy open_policy;
    open_policy.allow_conditional_models = true;
    FsFileDigest digest;
    FakeRunnerFactory factory;
    std::string error;

    Manifest ok;
    ok.models.push_back(entry_for(bytes, WeightsStatus::cleared));
    check(load_model(ok, "m", dir.path.string(), ModelPolicy{}, digest, factory, error) != nullptr, "cleared loads");
    check(factory.opened.size() == 1, "factory asked once");

    // Each refusal below must leave the factory untouched.
    factory.opened.clear();
    Manifest cond;
    cond.models.push_back(entry_for(bytes, WeightsStatus::conditional));
    check(!load_model(cond, "m", dir.path.string(), ModelPolicy{}, digest, factory, error), "conditional refused");
    check(has(error, "conditional") && has(error, "allow_conditional_models"), "conditional message");
    check(load_model(cond, "m", dir.path.string(), open_policy, digest, factory, error) != nullptr,
          "conditional loads with opt-in");
    factory.opened.clear();

    Manifest bad = ok;
    bad.models[0].sha256 = std::string(64, '0');
    check(!load_model(bad, "m", dir.path.string(), ModelPolicy{}, digest, factory, error), "hash mismatch refused");
    check(has(error, "sha256 mismatch"), "hash mismatch message");

    bad = ok;
    bad.models[0].size_bytes += 1;
    check(!load_model(bad, "m", dir.path.string(), ModelPolicy{}, digest, factory, error), "size mismatch refused");
    check(has(error, "size"), "size mismatch message");

    bad = ok;
    bad.models[0].file = "absent.onnx";
    check(!load_model(bad, "m", dir.path.string(), ModelPolicy{}, digest, factory, error), "missing file refused");
    check(has(error, "absent.onnx") && has(error, "cannot open"), "missing file message");

    bad = ok;
    bad.models[0].size_bytes = kMaxModelBytes + 1;
    check(!load_model(bad, "m", dir.path.string(), ModelPolicy{}, digest, factory, error), "oversize entry refused");
    check(has(error, "exceeds"), "oversize message");

    check(!load_model(ok, "other", dir.path.string(), ModelPolicy{}, digest, factory, error), "unknown name refused");
    check(has(error, "not in the manifest"), "unknown name message");
    check(factory.opened.empty(), "no refusal reached the runner: no fallback");
}

void test_bounds() {
    Manifest m;
    std::string error;
    for (int i = 0; i < 4; ++i) {
        ManifestEntry e;
        e.name = "m" + std::to_string(i);
        e.size_bytes = kMaxModelBytes;
        m.models.push_back(e);
    }
    check(check_model_bounds(m, error), "four at the per-model limit total exactly the cap");
    m.models[3].size_bytes = kMaxModelBytes;
    ManifestEntry extra;
    extra.name = "x";
    extra.size_bytes = 1;
    m.models.push_back(extra);
    check(!check_model_bounds(m, error) && has(error, "total"), "total over 100 MB refused");
    m.models.pop_back();
    m.models[0].size_bytes = kMaxModelBytes + 1;
    check(!check_model_bounds(m, error) && has(error, "m0"), "one over 25 MB refused");
}

void test_shipped_manifest() {
    Manifest m;
    std::string error;
    check(load_manifest_file(SONORA_MANIFEST_PATH, m, error), "shipped manifest parses within bounds");
    const ManifestEntry* e = find_model(m, "musicalkeycnn-keynet");
    check(e != nullptr, "keynet entry");
    if (!e) return;
    check(e->weights_status == WeightsStatus::conditional, "keynet is conditional");
    check(e->sha256 == "fc92072f1b9b19552ce3b74a9c8ce0cecb97633a12ae524ac0d93c05800e354e", "keynet sha from research");
    check(e->url.find("/61d8fc17") != std::string::npos, "url pinned to a commit");
    check(!load_manifest_file("/nonexistent/manifest.json", m, error) && has(error, "cannot open"), "missing manifest");
}

void test_runner_factory() {
    std::unique_ptr<RunnerFactory> f = make_runner_factory();
    check(f != nullptr, "factory exists");
#ifdef SONORA_TEST_EXPECT_NO_ONNX
    check(!onnx_runtime_built(), "stub build");
    ManifestEntry e = entry_for("x", WeightsStatus::cleared);
    std::string error;
    check(f->open(e, "x", error) == nullptr, "stub refuses");
    check(error == "m: model unavailable: built without ONNX Runtime", "stub message");
#endif
}

}  // namespace

int main() {
    test_sha256_nist();
    test_fs_digest();
    test_load_model_refusals();
    test_bounds();
    test_shipped_manifest();
    test_runner_factory();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("analyze models: ok");
    return 0;
}
