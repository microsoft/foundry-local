// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// This translation unit is only compiled when the WinML EP catalog NuGet
// package was resolved at CMake time (WinMLEpCatalog_FOUND). See
// sdk_v2/cpp/CMakeLists.txt for the source-list gating. The corresponding
// header (and this file) unconditionally reference WinML 2.x catalog APIs.
#include "ep_detection/winml_ep_bootstrapper.h"

#include "logger.h"

#include <fmt/format.h>

#include <atomic>
#include <string>
#include <vector>

// WinML EP Catalog C API — delay-loaded via Microsoft.Windows.AI.MachineLearning.dll
#include <WinMLAsync.h>
#include <WinMLEpCatalog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fl {

namespace {

/// Look up the running Windows build number via ``ntdll!RtlGetVersion``,
/// resolved through ``GetProcAddress`` to avoid pulling ``<winternl.h>`` and
/// its macro pollution. ``RtlGetVersion`` accepts an ``OSVERSIONINFOW``
/// (typedef-aliased to ``RTL_OSVERSIONINFOW`` in ``<winnt.h>``).
/// Returns 0 on failure; the value is purely diagnostic and never gates behavior.
DWORD QueryWindowsBuild() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) {
    return 0;
  }
  auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
      ::GetProcAddress(ntdll, "RtlGetVersion"));
  if (!rtl_get_version) {
    return 0;
  }
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (rtl_get_version(&info) != 0) {
    return 0;
  }
  return info.dwBuildNumber;
}

/// Try to load ``Microsoft.Windows.AI.MachineLearning.dll`` from the directory
/// that hosts ``foundry_local.dll`` (i.e., the module containing this code).
/// Returns ``NULL`` if the module path cannot be resolved or the sibling file
/// is absent — callers should fall back to a bare-name ``LoadLibraryW`` so a
/// system-installed copy on the default search path still wins.
///
/// Rationale: bindings that load ``foundry_local.dll`` from a non-default
/// directory (e.g., the JS prebuilds folder) don't push that directory onto
/// the process search path, so a bare ``LoadLibraryW("Microsoft.Windows.AI.MachineLearning.dll")``
/// silently misses the sibling DLL and EP discovery returns zero providers.
/// Resolving the path explicitly removes that dependency on caller-side
/// PATH / ``os.add_dll_directory`` setup.
HMODULE LoadWinMLDllFromOwnDirectory() {
  HMODULE own_module = nullptr;
  if (!::GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&LoadWinMLDllFromOwnDirectory),
          &own_module) ||
      own_module == nullptr) {
    return nullptr;
  }

  // Resolve own directory to find the sibling WinML DLL.
  // Dynamic buffer supports long paths (>MAX_PATH) when enabled.
  std::wstring buf(MAX_PATH, L'\0');
  DWORD len;
  while ((len = ::GetModuleFileNameW(own_module, buf.data(),
                                     static_cast<DWORD>(buf.size()))) == buf.size()) {
    buf.resize(buf.size() * 2);
  }
  if (len == 0) {
    return nullptr;
  }

  auto dll_path = std::filesystem::path(buf.data(), buf.data() + len)
                      .parent_path() / L"Microsoft.Windows.AI.MachineLearning.dll";

  return ::LoadLibraryExW(dll_path.c_str(), nullptr,
                          LOAD_WITH_ALTERED_SEARCH_PATH);
}

/// Context handed to ``WinMLAsyncBlock`` so the progress thunk can forward
/// fractional progress to the caller-supplied percentage callback and signal
/// cancellation back through ``WinMLAsyncCancel``.
struct EnsureReadyAsyncCtx {
  const std::string* name = nullptr;
  const fl::IEpBootstrapper::ProgressCallback* progress_cb = nullptr;
  std::atomic<bool> cancel_requested{false};
};

/// Forwards WinML's download progress to our percentage callback. WinML
/// reports a percent value (0-100) directly, matching how the WinRT
/// projection's IAsyncOperationWithProgress<_, double> reports progress.
void CALLBACK EnsureReadyProgressThunk(WinMLAsyncBlock* async, double progress) {
  auto* ctx = static_cast<EnsureReadyAsyncCtx*>(async->context);
  if (!ctx || !ctx->progress_cb || !*ctx->progress_cb) {
    return;
  }

  double pct = progress;
  if (pct < 0.0) pct = 0.0;
  if (pct > 100.0) pct = 100.0;

  if (!(*ctx->progress_cb)(*ctx->name, static_cast<float>(pct))) {
    ctx->cancel_requested.store(true, std::memory_order_release);
    WinMLAsyncCancel(async);
  }
}

}  // namespace

WinMLEpBootstrapper::WinMLEpBootstrapper(std::string name, EpRegistrationCallback register_ep,
                                         std::shared_ptr<void> catalog_ref, WinMLEpHandle ep_handle)
    : name_(std::move(name)),
      register_ep_(std::move(register_ep)),
      catalog_ref_(std::move(catalog_ref)),
      ep_handle_(ep_handle) {}

const std::string& WinMLEpBootstrapper::Name() const {
  return name_;
}

bool WinMLEpBootstrapper::IsRegistered() const {
  return registered_;
}

bool WinMLEpBootstrapper::DownloadAndRegister(bool force,
                                              const ProgressCallback& progress_cb,
                                              ILogger& logger) {
  if (registered_ && !force) {
    if (progress_cb) {
      progress_cb(name_, 100.0f);
    }
    return true;
  }

  // Ask the OS to download/prepare the EP if needed. We use the async variant
  // so we can forward fractional download progress to the caller and respect
  // cancellation; WinMLAsyncGetStatus(..., TRUE) gives us a synchronous wait.
  EnsureReadyAsyncCtx ctx;
  ctx.name = &name_;
  ctx.progress_cb = &progress_cb;

  WinMLAsyncBlock block{};
  block.context = &ctx;
  block.callback = nullptr;
  block.progress = (progress_cb ? &EnsureReadyProgressThunk : nullptr);

  HRESULT hr = WinMLEpEnsureReadyAsync(ep_handle_, &block);
  if (SUCCEEDED(hr)) {
    hr = WinMLAsyncGetStatus(&block, TRUE);
  }
  WinMLAsyncClose(&block);

  if (FAILED(hr)) {
    if (ctx.cancel_requested.load(std::memory_order_acquire)) {
      logger.Log(LogLevel::Information,
                 fmt::format("WinML EP {}: cancelled by caller", name_));
    } else {
      logger.Log(LogLevel::Warning,
                 fmt::format("WinML EP {}: EnsureReady failed (hr=0x{:08X})", name_,
                             static_cast<unsigned>(hr)));
    }
    return false;
  }

  // Reuse the path captured during enumeration when the EP was already Ready;
  // otherwise fetch it now that EnsureReady has populated it.
  if (library_path_.empty()) {
    size_t path_size = 0;
    hr = WinMLEpGetLibraryPathSize(ep_handle_, &path_size);

    if (FAILED(hr) || path_size == 0) {
      logger.Log(LogLevel::Warning, fmt::format("WinML EP {}: failed to get library path size (hr=0x{:08X})", name_,
                                                static_cast<unsigned>(hr)));
      return false;
    }

    std::string path(path_size, '\0');
    hr = WinMLEpGetLibraryPath(ep_handle_, path.size(), path.data(), nullptr);

    if (FAILED(hr)) {
      logger.Log(LogLevel::Warning, fmt::format("WinML EP {}: failed to get library path (hr=0x{:08X})", name_,
                                                static_cast<unsigned>(hr)));
      return false;
    }

    // The API may include a trailing null in the reported size.
    if (!path.empty() && path.back() == '\0') {
      path.pop_back();
    }

    library_path_ = std::move(path);
  }

  // Register with ORT via the callback.
  if (!register_ep_(name_, std::filesystem::path(library_path_))) {
    logger.Log(LogLevel::Warning, fmt::format("WinML EP {}: ORT registration failed", name_));
    return false;
  }

  registered_ = true;

  if (progress_cb) {
    progress_cb(name_, 100.0f);
  }

  // Library path + version are logged by the central register_ep callback;
  // no extra bootstrapper-side line needed.
  return true;
}

std::vector<std::unique_ptr<WinMLEpBootstrapper>> WinMLEpBootstrapper::DiscoverProviders(
    EpRegistrationCallback register_ep,
    ILogger& logger) {
  // Pre-check that the WinML DLL is loadable. The DLL is delay-loaded, so
  // calling WinML functions without it present would cause a structured
  // exception. Loading it explicitly is cleaner than SEH.
  //
  // Try the sibling-of-foundry_local.dll location first so bindings that
  // distribute the WinML DLL alongside foundry_local.dll (JS prebuilds,
  // Python wheels, C# packed runtimes) don't need to also push that
  // directory onto the process PATH. Fall back to the bare-name lookup
  // so a system-installed copy on the default search path still wins.
  HMODULE winml_dll = LoadWinMLDllFromOwnDirectory();
  if (!winml_dll) {
    winml_dll = LoadLibraryW(L"Microsoft.Windows.AI.MachineLearning.dll");
  }

  if (!winml_dll) {
    // Microsoft.Windows.AI.MachineLearning.dll only ships on Windows 10 19H1
    // (build 18362) and newer. Log GetLastError() and the OS build for diagnostics.
    DWORD load_err = ::GetLastError();
    DWORD os_build = QueryWindowsBuild();
    logger.Log(LogLevel::Information,
               fmt::format("WinML EP catalog: DLL not available — EP discovery disabled "
                           "(LoadLibrary err={}, Windows build {})",
                           load_err, os_build));
    return {};
  }
  // Keep the DLL loaded — the delay-load stubs will resolve against it.

  WinMLEpCatalogHandle catalog = nullptr;
  HRESULT hr = WinMLEpCatalogCreate(&catalog);

  if (FAILED(hr) || catalog == nullptr) {
    logger.Log(LogLevel::Information,
               fmt::format("WinML EP catalog: creation failed (hr=0x{:08X})", static_cast<unsigned>(hr)));
    return {};
  }

  // Wrap the catalog in a shared_ptr so all bootstrappers keep it alive.
  // The last bootstrapper destroyed will release the catalog.
  auto catalog_guard = std::shared_ptr<void>(catalog, [](void* c) {
    WinMLEpCatalogRelease(static_cast<WinMLEpCatalogHandle>(c));
  });

  // Context for the enumeration callback — collects discovered bootstrappers.
  struct EnumContext {
    std::vector<std::unique_ptr<WinMLEpBootstrapper>> bootstrappers;
    std::shared_ptr<void> catalog_ref;
    EpRegistrationCallback register_ep;
    ILogger* logger;
  };

  EnumContext ctx;
  ctx.catalog_ref = catalog_guard;
  ctx.register_ep = register_ep;
  ctx.logger = &logger;

  WinMLEpCatalogEnumProviders(
      catalog,
      [](WinMLEpHandle ep, const WinMLEpInfo* info, void* context) -> BOOL {
        auto* ctx = static_cast<EnumContext*>(context);

        std::string provider_name = info->name ? info->name : "";

        ctx->logger->Log(
            LogLevel::Information,
            fmt::format("WinML EP discovered: {} (state: {})", provider_name,
                        static_cast<int>(info->readyState)));

        // If the EP is already ready, capture its library path now.
        std::string library_path;
        if (info->readyState == WinMLEpReadyState_Ready && info->libraryPath) {
          library_path = info->libraryPath;
        }

        auto bootstrapper = std::unique_ptr<WinMLEpBootstrapper>(
            new WinMLEpBootstrapper(std::move(provider_name), ctx->register_ep,
                                    ctx->catalog_ref, ep));

        if (!library_path.empty()) {
          bootstrapper->library_path_ = std::move(library_path);
        }

        ctx->bootstrappers.push_back(std::move(bootstrapper));

        return TRUE;  // continue enumeration
      },
      &ctx);

  logger.Log(LogLevel::Information,
             fmt::format("WinML EP catalog: discovered {} provider(s)", ctx.bootstrappers.size()));

  return std::move(ctx.bootstrappers);
}

}  // namespace fl
