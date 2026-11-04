// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <string_view>

namespace {

int Fail(const char* message) {
  std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", message, GetLastError());
  return 32;
}

int Child(const wchar_t* library_path, const char* component, const char* mode, HANDLE verified) {
  const auto library = LoadLibraryW(library_path);
  if (library == nullptr) return Fail("cannot load the production-linked fixture");
  using RunTest = void (*)(const char*, const char*, HANDLE);
  const auto run = reinterpret_cast<RunTest>(GetProcAddress(library, "WindowsTelemetryShutdownTest"));
  if (run == nullptr) return Fail("cannot find fixture entry point");
  run(component, mode, verified);
  const DWORD expected = std::string_view{mode} == "normal-destruction" ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
  if (WaitForSingleObject(verified, 0) != expected) return Fail("cleanup occurred at the wrong lifecycle stage");
  if (std::string_view{mode} == "process-exit") ExitProcess(0);
  if (!FreeLibrary(library)) return Fail("explicit FreeLibrary failed");
  if (WaitForSingleObject(verified, 0) != WAIT_OBJECT_0) return Fail("DLL teardown was not verified");
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  if (argc != 4 && argc != 6) return Fail("expected fixture, component, and lifecycle mode");
  const std::wstring_view component{argv[2]};
  const std::wstring_view mode{argv[3]};
  if (component != L"manager" && component != L"telemetry") return Fail("unknown fixture component");
  if (mode != L"process-exit" && mode != L"normal-destruction" && mode != L"explicit-unload") {
    return Fail("unknown fixture lifecycle mode");
  }
  if (argc == 6) {
    if (std::wstring_view{argv[4]} != L"--child") return Fail("unknown child argument");
    const auto verified = reinterpret_cast<HANDLE>(std::wcstoull(argv[5], nullptr, 10));
    return Child(argv[1], component == L"manager" ? "manager" : "telemetry",
                 mode == L"process-exit"         ? "process-exit"
                 : mode == L"normal-destruction" ? "normal-destruction"
                                                 : "explicit-unload",
                 verified);
  }

  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  const HANDLE verified = CreateEventW(&attributes, TRUE, FALSE, nullptr);
  if (verified == nullptr) return Fail("cannot create inherited verification event");
  wchar_t executable[32768];
  const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
  if (length == 0 || length == ARRAYSIZE(executable)) return Fail("cannot locate test executable");
  std::wstring command = L"\"" + std::wstring(executable) + L"\" \"" + argv[1] + L"\" " + argv[2] + L" " +
                         argv[3] + L" --child " + std::to_wstring(reinterpret_cast<uintptr_t>(verified));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
    CloseHandle(verified);
    return Fail("cannot start fixture child");
  }
  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
  DWORD code = 32;
  if (wait != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 32);
    WaitForSingleObject(process.hProcess, 5000);
    std::fputs("FAIL: child hung during DLL lifecycle\n", stderr);
  } else if (!GetExitCodeProcess(process.hProcess, &code)) {
    code = 32;
  }
  const bool checked = WaitForSingleObject(verified, 0) == WAIT_OBJECT_0;
  CloseHandle(process.hProcess);
  CloseHandle(verified);
  if (wait != WAIT_OBJECT_0 || code != 0 || !checked) {
    std::fprintf(stderr, "FAIL: child exit=0x%08lx, teardown verified=%d\n", code, checked);
    return Fail("child did not complete verified DLL teardown");
  }
  std::fputs("PASS: production DLL lifecycle verified\n", stdout);
  return 0;
}
