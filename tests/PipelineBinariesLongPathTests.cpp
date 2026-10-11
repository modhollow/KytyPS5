// Pipeline-binary stores and precompile shards on paths longer than MAX_PATH: the writer saves and the
// store opens through the native long-path form (PipelineCacheFile::NativePath), as the cache files' temporary
// and rename already do. A packaged install under a user folder puts the precompile's shard files past 260
// characters; the save then failed silently and the shard was compiled again on every run.
#include "graphics/host_gpu/renderer/pipeline/pipelineBinaries.h"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

} // namespace

int main() {
	namespace File = Libs::Graphics::PipelineCacheFile;
	using Libs::Graphics::PipelineBinaries;
	using Libs::Graphics::PipelineBinaryWriter;
#ifdef _WIN32
	(void)_putenv_s("KYTY_PRECOMPILE_CLAIMS", "");
	const auto process = _getpid();
#else
	unsetenv("KYTY_PRECOMPILE_CLAIMS");
	const auto process = getpid();
#endif
	const std::string signature = "SYNTHETIC SIGNATURE\n";
	const auto base = std::filesystem::temp_directory_path() / ("kyty-binaries-long-" + std::to_string(process));
	// As the precompile names them: a 99-character driver directory and a shard of the seed file.
	const auto short_dir = base / "short";
	const auto long_dir  = base / std::string(100, 'd') /
	                      "000010de-00002c02-93d58000-e0f407cbac49b7b8acfdd8c3260a3ec9-00000000-0000000000000000";
	const auto short_path = short_dir / "PPSA01341_01.007.000.binaries";
	const auto long_path =
	    long_dir / "PPSA01341_01.007.000.binaries.shard3of512-seeds-PPSA01341_01.007.000";
	const auto long_store = long_dir / "PPSA01341_01.007.000.binaries";
	if (!File::CreateDirectories(short_dir) || !File::CreateDirectories(long_dir)) {
		std::printf("setup: cannot create the test directories under %s\n", base.string().c_str());
		return 2;
	}
	Check(long_path.native().size() > 300, "the shard path is longer than MAX_PATH (setup)");

	PipelineBinaryWriter writer(vk::Device {});
	Check(writer.Save(short_path, signature), "a store on a short path saves (control)");
	Check(PipelineBinaries::Open(vk::Device {}, short_path, signature) != nullptr,
	      "a store on a short path opens (control)");

	Check(writer.Save(long_path, signature), "a shard on a path longer than MAX_PATH saves");
	const auto shard = PipelineBinaries::Open(vk::Device {}, long_path, signature);
	Check(shard != nullptr && shard->Pipelines() == 0, "a shard on a path longer than MAX_PATH opens (the merge)");

	// The precompile's sequence: reserve a temporary next to the store, save into it, rename it over.
	const auto temporary = File::Temporary(long_store);
	Check(!temporary.empty(), "a temporary next to a long store is reserved");
	Check(!temporary.empty() && writer.Save(temporary, signature), "the temporary of a long store is written");
	Check(!temporary.empty() && File::Replace(temporary, long_store), "the temporary replaces the long store");
	Check(PipelineBinaries::Open(vk::Device {}, long_store, signature) != nullptr,
	      "the long store opens (the emulator's lookup)");
	Check(PipelineBinaries::Open(vk::Device {}, long_store, "ANOTHER SIGNATURE!\n") == nullptr,
	      "a long store of another GPU or driver is still rejected");

	std::error_code error;
	std::filesystem::remove_all(File::NativePath(base), error);
	if (g_failures != 0) {
		std::printf("PipelineBinariesLongPathTests: %d failures\n", g_failures);
		return 1;
	}
	std::printf("PipelineBinariesLongPathTests: all passed\n");
	return 0;
}
