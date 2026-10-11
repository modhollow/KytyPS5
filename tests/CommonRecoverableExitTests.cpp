#include "common/assert.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(__cpp_exceptions) && !defined(__EXCEPTIONS) && !defined(_CPPUNWIND)
#error Common consumers require C++ exceptions for recoverable exits and stack cleanup.
#endif

#if defined(_WIN32)
#include <windows.h>
#include <vector>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "CommonRecoverableExitTests: failed: %s\n", message);
		std::abort();
	}
}

int g_drain_count = 0;

void CountDrain() {
	++g_drain_count;
}

struct Cleanup {
	int& count;
	~Cleanup() { ++count; }
};

void TestRecovery(int kind) {
	int         cleanup_count = 0;
	bool        caught        = false;
	bool        continued     = false;
	std::string expected;
	try {
		Common::RecoverableExitScope scope;
		Cleanup                      cleanup {cleanup_count};
		if (kind == 0) {
			expected = "synthetic refusal in CommonRecoverableExitTests:37";
			Check(Common::DbgExitHandler("CommonRecoverableExitTests", 37, "synthetic refusal") ==
			          1,
			      "plain recovery handler result");
		} else if (kind == 1) {
			expected = "styled refusal in CommonRecoverableExitTests:38";
			Check(Common::DbgExitHandler("CommonRecoverableExitTests", 38, fmt::fg(fmt::color::red),
			                             "styled refusal") == 1,
			      "styled recovery handler result");
		} else if (kind == 2) {
			expected =
			    "Error: condition (synthetic condition) is true in CommonRecoverableExitTests:39";
			Check(Common::DbgExitIfHandler("synthetic condition", "CommonRecoverableExitTests",
			                               39) == 1,
			      "condition recovery handler result");
		} else {
			expected = "Not implemented (synthetic operation) in CommonRecoverableExitTests:40";
			Check(Common::DbgNotImplementedHandler("synthetic operation",
			                                       "CommonRecoverableExitTests", 40) == 1,
			      "not-implemented recovery handler result");
		}
		Common::DbgExit(321);
		continued = true;
	} catch (const Common::RecoverableExit& exit) {
		caught = true;
		Check(exit.message == expected, "recoverable message differs from real handler report");
	}
	Check(caught && !continued, "recoverable exit did not leave the calling frame");
	Check(cleanup_count == 1, "intervening consumer frame did not unwind exactly once");
	Check(g_drain_count == 0, "recoverable exit invoked the fatal drain");
}

int NormalExitCase(const char* mode) {
	if (std::strcmp(mode, "return") == 0) {
		Common::RecoverableExitScope scope;
	} else {
		TestRecovery(0);
	}
	try {
		Common::DbgExit(321);
	} catch (const Common::RecoverableExit&) {
		return 123;
	}
	return 124;
}

void TestNormalExit(const char* executable, const char* mode) {
#if defined(_WIN32)
	wchar_t     path[32768] {};
	const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
	Check(length != 0 && length < 32768, "GetModuleFileName failed");
	std::wstring command = std::wstring(L"\"") + path + L"\" --normal-exit ";
	command += std::strcmp(mode, "return") == 0 ? L"return" : L"unwind";
	std::vector<wchar_t> mutable_command(command.begin(), command.end());
	mutable_command.push_back(L'\0');
	STARTUPINFOW        startup {sizeof(startup)};
	PROCESS_INFORMATION process {};
	Check(CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
	                     nullptr, nullptr, &startup, &process) != 0,
	      "CreateProcess failed");
	Check(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "normal exit timed out");
	DWORD status = 0;
	Check(GetExitCodeProcess(process.hProcess, &status) != 0 && status == 321,
	      "scope destruction did not restore the normal fatal exit");
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
#else
	const pid_t pid = fork();
	Check(pid >= 0, "fork failed");
	if (pid == 0) {
		execlp(executable, executable, "--normal-exit", mode, nullptr);
		std::_Exit(125);
	}
	int status = 0;
	Check(waitpid(pid, &status, 0) == pid, "waitpid failed");
	Check(WIFEXITED(status) && WEXITSTATUS(status) == (321 & 0xff),
	      "scope destruction did not restore the normal fatal exit");
#endif
}

} // namespace

int main(int argc, char** argv) {
	if (argc == 3 && std::strcmp(argv[1], "--normal-exit") == 0) {
		return NormalExitCase(argv[2]);
	}
	Check(argc == 1, "unexpected arguments");
	Common::SetExitDrain(CountDrain);
	for (int kind = 0; kind < 4; ++kind) {
		TestRecovery(kind);
	}
	Common::SetExitDrain(nullptr);
	TestNormalExit(argv[0], "return");
	TestNormalExit(argv[0], "unwind");
	std::puts("CommonRecoverableExitTests: passed");
	return 0;
}
