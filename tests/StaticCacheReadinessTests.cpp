#include "local/shader-warmup-cache.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"
#if defined(__linux__)
#include "graphics/host_gpu/renderer/pipeline/pipelineBinaries.h"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <xxhash.h>

// The parser's genuine key builders come from shaderStaticKey.cpp. Unexpected fatal assertions fail this CPU harness.
namespace Common {
int DbgExitIfHandler(const char* expression, const char*, int) {
    std::fprintf(stderr, "StaticCacheReadinessTests: fatal: %s\n", expression);
    std::abort();
}
void DbgExit(int) { std::abort(); }
}
namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "StaticCacheReadinessTests: failed: %s\n", message);
        failures++;
    }
}
void Write(const std::filesystem::path& path, const std::string& signature, const std::vector<uint32_t>& words, bool damage = false) {
    const auto checksum = XXH3_64bits(words.data(), words.size() * 4) ^ (damage ? 1u : 0u);
    std::ofstream file(path, std::ios::binary);
    file.write(signature.data(), signature.size());
    file.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
    file.write(reinterpret_cast<const char*>(words.data()), words.size() * 4);
    Check(bool(file), "could not write synthetic fixture");
}
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("kyty-static-readiness-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    if (!std::filesystem::create_directory(directory, error)) return 1;
    const auto seeds = directory / "test.seeds";
    const auto inputs = directory / "test.shaders";
    const auto driver = directory / "test.bin";
    { std::ofstream file(seeds); file << "synthetic seeds"; }
    const auto time = std::filesystem::last_write_time(seeds);
    const std::string signature = "SYNTHETIC:device\n";
    const auto input_signature = "KytyShaderWarmup3:" + LocalShaderWarmup::Cache::StaticSignature(signature, "producer-A");
    Write(inputs, input_signature, {0, 0});
    std::filesystem::last_write_time(inputs, time);
    Check(LocalShaderWarmup::Cache::Current(inputs, signature, seeds, "producer-A"), "valid unchanged empty inputs must be current");
    Check(!LocalShaderWarmup::Cache::Current(inputs, signature, seeds, "producer-B"), "producer change with identical seeds/time must refresh inputs");
    Write(inputs, input_signature, {0, 0}, true);
    std::filesystem::last_write_time(inputs, time);
    LocalShaderWarmup::Cache loader;
    Check(!loader.Open(inputs, LocalShaderWarmup::Cache::StaticSignature(signature, "producer-A")), "actual loader baseline must reject damaged inputs");
    Check(!LocalShaderWarmup::Cache::Current(inputs, signature, seeds, "producer-A"), "recent header-valid checksum-damaged inputs must not be current");
    Write(inputs, input_signature, {0, 0, 1234});
    std::filesystem::last_write_time(inputs, time);
    Check(!loader.Open(inputs, LocalShaderWarmup::Cache::StaticSignature(signature, "producer-A")), "actual loader baseline must reject hash-valid trailing words");
    Check(!LocalShaderWarmup::Cache::Current(inputs, signature, seeds, "producer-A"), "hash-valid malformed input body must not be current");
    Write(inputs, input_signature, {0, 1, 1, 0});
    std::filesystem::last_write_time(inputs, time);
    Check(!loader.Open(inputs, LocalShaderWarmup::Cache::StaticSignature(signature, "producer-A")), "actual loader must reject invalid pipeline references");
    Check(!LocalShaderWarmup::Cache::Current(inputs, signature, seeds, "producer-A"), "invalid pipeline references must not be current");
    Write(inputs, input_signature, {0});
    std::filesystem::last_write_time(inputs, time);
    Check(!LocalShaderWarmup::Cache::Current(inputs, signature, seeds, "producer-A"), "truncated input structure must not be current");
    Write(inputs, input_signature, {0, 0});
    std::filesystem::last_write_time(inputs, time - std::chrono::seconds(1));
    Check(!LocalShaderWarmup::Cache::Current(inputs, signature, seeds, "producer-A"), "inputs older than seeds must refresh");
    Check(!LocalShaderWarmup::Cache::Current(inputs, signature, seeds, {}), "missing producer identity must not be current");
    const auto populated = directory / "populated.shaders";
    LocalShaderWarmup::Record record;
    record.stage = Libs::Graphics::ShaderType::Compute;
    record.code = {0}; // Synthetic parser data; never translated or sent to a device.
    record.compute.wave_size = 32;
    record.compute.host_subgroup_size = 32;
    Libs::Graphics::BuildStageStaticKey(record.compute, record.static_key);
    LocalShaderWarmup::Cache writer;
    Check(writer.Open(populated, LocalShaderWarmup::Cache::StaticSignature(signature, "producer-A")), "new input writer must open");
    Check(writer.Add(record) == 0 && writer.Save(), "actual writer must save a valid nonempty record");
    Check(LocalShaderWarmup::Cache::Current(populated, signature, seeds, "producer-A"), "fully parsed nonempty inputs must be current");
    LocalShaderWarmup::Cache populated_loader;
    Check(populated_loader.Open(populated, LocalShaderWarmup::Cache::StaticSignature(signature, "producer-A")) &&
          populated_loader.records.size() == 1, "actual loader must round-trip a nonempty record");
    record.static_key[0] ^= 1u;
    LocalShaderWarmup::Writer encoded;
    Check(LocalShaderWarmup::Visit(encoded, record), "could not serialize malformed-key fixture");
    std::vector<uint32_t> malformed {1, static_cast<uint32_t>(encoded.words.size())};
    malformed.insert(malformed.end(), encoded.words.begin(), encoded.words.end());
    malformed.push_back(0);
    Write(populated, input_signature, malformed);
    Check(!populated_loader.Open(populated, LocalShaderWarmup::Cache::StaticSignature(signature, "producer-A")), "actual loader must reject an inconsistent owning static key");
    Check(!LocalShaderWarmup::Cache::Current(populated, signature, seeds, "producer-A"), "hash-valid inconsistent static keys must not be current");
    namespace Driver = Libs::Graphics::PipelineCacheFile;
    Driver::Identity identity {std::string(40, 'a'), 1, 2, 3};
    const auto driver_signature = Driver::Signature(identity);
    Write(driver, driver_signature, {32, 1, 1, 2, 0, 0, 0, 0, 42}, true);
    std::filesystem::last_write_time(driver, time);
    Check(Driver::Read(driver, identity).payload.empty(), "actual driver loader must reject damaged payload");
    Check(!Driver::Current(driver, identity, seeds), "recent header-valid damaged driver body must not be current");
    Write(driver, driver_signature, {32, 1, 1, 2, 0, 0, 0, 0, 42});
    std::filesystem::last_write_time(driver, time);
    Check(Driver::Current(driver, identity, seeds), "valid driver payload must be current");
    identity.revision = std::string(40, 'b');
    Check(Driver::Current(driver, identity, seeds), "unchanged driver inputs survive source provenance changes");
    identity.instrumentation = 1;
    Check(!Driver::Current(driver, identity, seeds), "instrumented driver data must remain separate");
    identity.instrumentation = 0;
    Write(driver, driver_signature, {32, 1, 999, 2, 0, 0, 0, 0, 42});
    std::filesystem::last_write_time(driver, time);
    Check(!Driver::Current(driver, identity, seeds), "hash-valid wrong Vulkan device header must not be current");
#if defined(__linux__)
    unsetenv("KYTY_PRECOMPILE_CLAIMS");
    Libs::Graphics::PipelineBinaryWriter binary_writer(vk::Device{});
    Check(!binary_writer.Save("/dev/full", "SYNTHETIC\n"), "actual binary writer must report close-time write failure");
#endif
    std::filesystem::remove_all(directory, error);
    std::printf("StaticCacheReadinessTests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
