// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/hosted_backend.h"
#include "catalog.h"
#include "exception.h"
#include "hosting/bootstrap_descriptor.h"
#include "hosting/host_profile.h"
#include "hosting/host_coordinator.h"
#include "hosting/host_process.h"
#include "hosting/runtime_assets.h"
#include "hosting/sdk_connection.h"
#include "internal_api/c_api_test_helpers.h"
#include "util/scope_guard.h"
#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "items/message_item.h"
#include "util/sha256.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <thread>

#include <onnxruntime_c_api.h>
#include <ort_genai_c.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class HostChild {
 public:
  HostChild(const std::filesystem::path& executable, const std::filesystem::path& descriptor, bool& owns_root)
      : owns_root_(owns_root) {
#ifdef _WIN32
    // Test-controlled paths are quoted without a shell; neither ends in a backslash or contains a quote.
    auto command = L"\"" + executable.wstring() + L"\" --bootstrap \"" + descriptor.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process)) {
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "launch native host test");
    }

    process_ = process.hProcess;
    pid_ = process.dwProcessId;
    CloseHandle(process.hThread);
#else
    auto program = executable.string();
    auto argument = descriptor.string();
    char option[] = "--bootstrap";
    char* arguments[]{program.data(), option, argument.data(), nullptr};
    const auto error = posix_spawn(&pid_, executable.c_str(), nullptr, nullptr, arguments, environ);
    if (error != 0) {
      throw std::system_error(error, std::generic_category(), "launch native host test");
    }
#endif
  }

  ~HostChild() {
    try {
      if (!WaitForExit(Clock::now() + 35s)) {
        owns_root_ = false;
        ADD_FAILURE() << "Native host did not exit; preserving scratch state for PID " << pid_;
      }
    } catch (const std::exception& error) {
      owns_root_ = false;
      ADD_FAILURE() << error.what();
    }

#ifdef _WIN32
    CloseHandle(process_);
#endif
  }

  HostChild(const HostChild&) = delete;
  HostChild& operator=(const HostChild&) = delete;

  bool WaitForExit(Clock::time_point deadline) {
    while (!exited_) {
#ifdef _WIN32
      const auto result = WaitForSingleObject(process_, 0);
      if (result == WAIT_OBJECT_0) {
        DWORD status{};
        if (!GetExitCodeProcess(process_, &status)) {
          throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "read host exit code");
        }

        status_ = static_cast<int>(status);
        exited_ = true;
      } else if (result != WAIT_TIMEOUT) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "observe host exit");
      }
#else
      int status{};
      const auto result = waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        status_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        exited_ = true;
      } else if (result < 0 && errno != EINTR) {
        throw std::system_error(errno, std::generic_category(), "observe host exit");
      }
#endif
      if (exited_ || Clock::now() >= deadline) {
        break;
      }

      std::this_thread::sleep_for(10ms);
    }

    return exited_;
  }

  int ExitCode() const { return status_; }
  uint64_t Pid() const { return static_cast<uint64_t>(pid_); }

 private:
  bool& owns_root_;
  bool exited_ = false;
  int status_ = -1;
#ifdef _WIN32
  HANDLE process_ = nullptr;
  DWORD pid_ = 0;
#else
  pid_t pid_ = 0;
#endif
};

class BootstrapDescriptorTest : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<uint64_t> sequence{0};
    const auto scratch = std::filesystem::path(FOUNDRY_LOCAL_TEST_SCRATCH_DIR);
    ASSERT_TRUE(scratch.is_absolute());
    std::filesystem::create_directories(scratch);
    ASSERT_FALSE(std::filesystem::is_symlink(scratch));
    root_ = scratch / ("bootstrap-" + std::to_string(test::CurrentPid()) + "-" + std::to_string(sequence.fetch_add(1)));
    ASSERT_TRUE(std::filesystem::create_directory(root_));
    owns_root_ = true;
    descriptor_.native = Artifact("native.bin", "native runtime bytes");
    descriptor_.ort = Artifact("ort.bin", "selected ORT bytes");
    descriptor_.genai = Artifact("genai.bin", "selected GenAI bytes");
    host_ = Artifact("host.bin", "packaged executable bytes");
    descriptor_.manifest = root_ / "foundry-local-runtime.json";
    descriptor_.native_build = "runtime-build";
    descriptor_.runtime_identifier = "win-x64";
    Configuration configuration;
    configuration.app_name = "bootstrap-test";
    configuration.app_data_dir = (root_ / "data").string();
    configuration.model_cache_dir = (root_ / "models").string();
    configuration.logs_dir = (root_ / "logs").string();
    configuration.disable_nonessential_telemetry = true;
    configuration.catalog_urls = {{"https://example.test/models", std::nullopt}, {"https://example.test/other", ""}};
    configuration.additional_options = {{"ExecutionOption", "preserved"}, {"UserAgent", "client-only-label"}};
    const auto profile = BuildHostProfile(configuration, {descriptor_.native_build, descriptor_.runtime_identifier, 1});
    descriptor_.configuration = profile.configuration;
    descriptor_.compatibility = profile.compatibility;
    descriptor_.child = {root_ / "socket", profile.key, "unique-incarnation", {123, 456}};
    manifest_ = {{"version", 1},
                 {"protocol", 1},
                 {"native_build", descriptor_.native_build},
                 {"rid", descriptor_.runtime_identifier},
                 {"native", {{"file_name", "native.bin"}, {"sha256", descriptor_.native.sha256}}},
                 {"host", {{"file_name", "host.bin"}, {"sha256", host_.sha256}}}};
    Write(descriptor_.manifest, manifest_.dump());
  }

  void TearDown() override {
    if (owns_root_) {
      std::filesystem::remove_all(root_);
    }
  }

  static void Write(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output << value;
    output.close();
    ASSERT_TRUE(output);
  }

  BootstrapArtifact Artifact(const std::string& name, const std::string& value) {
    const auto path = root_ / name;
    Write(path, value);
    return {path, Sha256File(path)};
  }

  static std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void Verify(const BootstrapDescriptor& descriptor) const {
    VerifyBootstrapArtifacts(descriptor, host_.path, "runtime-build", "win-x64");
  }

  template <typename Function>
  static auto WithRuntime(Function&& function) {
    const auto directory = std::filesystem::path(FOUNDRY_NATIVE_RUNTIME_DIR);
    const auto manifest = ReadBootstrapJson(directory / "foundry-local-runtime.json");
    const auto path = directory / manifest.at("native").at("file_name").get<std::string>();
#ifdef _WIN32
    const auto library =
        LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "cannot load test runtime");
    }

    ScopeGuard release([&]() noexcept { FreeLibrary(library); });
    const auto symbol = reinterpret_cast<decltype(&FoundryLocalGetApi)>(GetProcAddress(library, "FoundryLocalGetApi"));
#else
    const auto library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "cannot load test runtime: ", dlerror());
    }

    ScopeGuard release([&]() noexcept { dlclose(library); });
    const auto symbol = reinterpret_cast<decltype(&FoundryLocalGetApi)>(dlsym(library, "FoundryLocalGetApi"));
#endif
    if (!symbol) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "test runtime has no public API export");
    }

    return function(symbol);
  }

  RuntimeAssets ResidentAssets() const {
    return WithRuntime([](const auto get_api) {
      return ResolveRuntimeAssets(ResidentModuleAt(reinterpret_cast<const void*>(get_api)));
    });
  }

  void RemoveFabricatedRecord(const HostStateStore& store, const HostLaunchRecord& record) noexcept {
    try {
      const auto lock = store.Lock(Clock::now() + 1s);
      const auto current = store.Read();
      if (!current || current->child.incarnation != record.child.incarnation || current->process != record.process ||
          std::filesystem::exists(store.BootstrapPath(record)) || store.ReadPublishedProcess(record)) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
                 "fabricated launch changed unexpectedly; preserving reconciliation state");
      }

      EXPECT_TRUE(std::filesystem::remove(store.Directory() / "launch.json"));
    } catch (const std::exception& error) {
      owns_root_ = false;
      ADD_FAILURE() << error.what();
    }
  }

  std::filesystem::path root_;
  bool owns_root_ = false;
  BootstrapDescriptor descriptor_;
  BootstrapArtifact host_;
  Json manifest_;
};

TEST_F(BootstrapDescriptorTest, MaterializesTheExactProfileAndVerifiesAllSelectedBytes) {
  const auto decoded = DecodeBootstrapDescriptor(EncodeBootstrapDescriptor(descriptor_));
  EXPECT_EQ(decoded.child.profile_key, descriptor_.child.profile_key);
  EXPECT_EQ(decoded.configuration.app_name, descriptor_.configuration.app_name);
  EXPECT_EQ(decoded.configuration.model_cache_dir, descriptor_.configuration.model_cache_dir);
  EXPECT_EQ(decoded.configuration.catalog_urls, descriptor_.configuration.catalog_urls);
  EXPECT_EQ(decoded.configuration.web_service_endpoints, descriptor_.configuration.web_service_endpoints);
  EXPECT_EQ(decoded.configuration.additional_options.at("ExecutionOption"), "preserved");
  EXPECT_FALSE(decoded.configuration.additional_options.contains("UserAgent"));
  EXPECT_TRUE(decoded.configuration.disable_nonessential_telemetry);
  EXPECT_NO_THROW(Verify(decoded));
}

TEST_F(BootstrapDescriptorTest, ChangedDependenciesAndExecutableAreRejectedBeforeAnyLibraryLoad) {
  for (const auto& artifact : {descriptor_.native, descriptor_.ort, descriptor_.genai, host_}) {
    const auto contents = ReadBytes(artifact.path);
    Write(artifact.path, contents + "changed");
    EXPECT_THROW(Verify(descriptor_), Exception) << artifact.path;
    Write(artifact.path, contents);
    EXPECT_NO_THROW(Verify(descriptor_));
  }
}

TEST_F(BootstrapDescriptorTest, DifferentPackagedDependenciesShareAProfileButStillVerifyTheirOwnFiles) {
  auto packaged = descriptor_;
  packaged.ort = Artifact("pip-ort.bin", "differently packaged ORT bytes");
  packaged.genai = Artifact("pip-genai.bin", "differently packaged GenAI bytes");
  const auto decoded = DecodeBootstrapDescriptor(EncodeBootstrapDescriptor(packaged));
  EXPECT_EQ(decoded.child.profile_key, descriptor_.child.profile_key);
  EXPECT_NO_THROW(Verify(decoded));

  Write(packaged.ort.path, "changed after selection");
  EXPECT_THROW(Verify(decoded), Exception);
}

TEST_F(BootstrapDescriptorTest, ForgedProfileRelativePathsAndNestedChildRolesAreRejected) {
  auto encoded = EncodeBootstrapDescriptor(descriptor_);
  encoded["compatibility"]["profile"]["execution_options"]["ExecutionOption"] = "different";
  EXPECT_THROW(DecodeBootstrapDescriptor(encoded), Exception);
  encoded = EncodeBootstrapDescriptor(descriptor_);
  encoded["ort"]["path"] = "relative-ort.bin";
  EXPECT_THROW(DecodeBootstrapDescriptor(encoded), Exception);
  encoded = EncodeBootstrapDescriptor(descriptor_);
  encoded["compatibility"]["profile"]["execution_options"][kChildHostOption] = "{}";
  encoded["child"]["profile"] = Sha256String(encoded["compatibility"].dump());
  EXPECT_THROW(DecodeBootstrapDescriptor(encoded), Exception);
}

TEST_F(BootstrapDescriptorTest, WrongReleaseAndManifestTraversalCannotSelectAnotherRuntime) {
  EXPECT_THROW(VerifyBootstrapArtifacts(descriptor_, host_.path, "other-build", "win-x64"), Exception);
  EXPECT_THROW(VerifyBootstrapArtifacts(descriptor_, host_.path, "runtime-build", "linux-x64"), Exception);
  manifest_["native"]["file_name"] = "../native.bin";
  Write(descriptor_.manifest, manifest_.dump());
  EXPECT_THROW(Verify(descriptor_), Exception);
}

TEST_F(BootstrapDescriptorTest, FailedFileReadsNeverBecomeValidEmptyContentHashes) {
  EXPECT_TRUE(Sha256File(root_).empty());
  EXPECT_THROW(ReadBootstrapJson(root_), Exception);
  Write(root_ / "oversized.json", std::string(1024 * 1024 + 1, ' '));
  EXPECT_THROW(ReadBootstrapJson(root_ / "oversized.json"), Exception);
}

TEST_F(BootstrapDescriptorTest, DiscoversResidentAssetsAndRejectsChangedOrUnidentifiableImages) {
  const auto runtime_directory = std::filesystem::path(FOUNDRY_NATIVE_RUNTIME_DIR);
  const auto manifest = ReadBootstrapJson(runtime_directory / "foundry-local-runtime.json");
  const auto native_path = runtime_directory / manifest.at("native").at("file_name").get<std::string>();
#ifdef _WIN32
  const auto library =
      LoadLibraryExW(native_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  ASSERT_NE(library, nullptr);
  ScopeGuard release([&]() noexcept { FreeLibrary(library); });
  const auto symbol = reinterpret_cast<const void*>(GetProcAddress(library, "FoundryLocalGetApi"));
#else
  const auto library = dlopen(native_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(library, nullptr) << dlerror();
  ScopeGuard release([&]() noexcept { dlclose(library); });
  const auto symbol = dlsym(library, "FoundryLocalGetApi");
#endif
  ASSERT_NE(symbol, nullptr);
  const auto native = ResidentModuleAt(symbol);
  const auto ort = ResidentModuleAt(OrtGetApiBase());
  // Keep the imported address observable so non-PIE builds exercise canonical executable PLT entries.
  const auto imported_genai = ResidentModuleAt(reinterpret_cast<const void*>(&OgaCreateModel));
#ifdef __linux__
  const auto genai = ResidentModuleAt(dlsym(library, "OgaCreateModel"));
#else
  const auto& genai = imported_genai;
#endif
  for (const auto& module : {native, ort, imported_genai, genai}) {
    EXPECT_EQ(FileModuleIdentity(module.path), module.image_identity) << module.path;
  }

  const auto assets = ResolveRuntimeAssets(native);
  EXPECT_EQ(std::filesystem::canonical(assets.executable),
            std::filesystem::canonical(std::filesystem::path(FOUNDRY_HOST_EXECUTABLE)));
  EXPECT_EQ(assets.bootstrap.native.sha256, manifest.at("native").at("sha256").get<std::string>());
  EXPECT_EQ(assets.bootstrap.ort.sha256, Sha256File(ort.path));
  EXPECT_EQ(assets.bootstrap.genai.sha256, Sha256File(genai.path));
  auto replaced = native;
  replaced.image_identity += "different-build";
  EXPECT_THROW(ResolveRuntimeAssets(replaced, ort, genai), Exception);
  replaced.image_identity.clear();
  try {
    static_cast<void>(ResolveRuntimeAssets(replaced, ort, genai));
    FAIL() << "Missing resident identity must be rejected";
  } catch (const Exception& error) {
    EXPECT_NE(std::string(error.what()).find("identifiable runtime module"), std::string::npos);
  }

  EXPECT_THROW(FileModuleIdentity(descriptor_.native.path), Exception);
  EXPECT_THROW(ResidentModuleAt(nullptr), Exception);
#ifdef _WIN32
  if (genai.image_identity.starts_with("pe-code:")) {
    auto changed = ReadBytes(genai.path);
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, changed.data(), sizeof(dos));
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, changed.data() + dos.e_lfanew, sizeof(nt));
    const auto section_table =
        dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    bool modified = false;
    for (uint16_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
      IMAGE_SECTION_HEADER section{};
      std::memcpy(&section, changed.data() + section_table + i * sizeof(section), sizeof(section));
      if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) {
        changed.at(section.PointerToRawData) ^= 1;
        modified = true;
        break;
      }
    }

    ASSERT_TRUE(modified);
    const auto altered = root_ / "altered-genai.bin";
    Write(altered, changed);
    EXPECT_NE(FileModuleIdentity(altered), genai.image_identity);
    auto replaced_genai = genai;
    replaced_genai.path = altered;
    EXPECT_THROW(ResolveRuntimeAssets(native, ort, replaced_genai), Exception);
  }
#endif
}

TEST_F(BootstrapDescriptorTest, ExecutableBootstrapsIndependentlyAndActuallyExitsAfterTheFinalOwner) {
  const auto runtime_directory = std::filesystem::path(FOUNDRY_NATIVE_RUNTIME_DIR);
  const auto executable = std::filesystem::path(FOUNDRY_HOST_EXECUTABLE);
  BootstrapDescriptor descriptor;
  descriptor.manifest = runtime_directory / "foundry-local-runtime.json";
  const auto manifest = ReadBootstrapJson(descriptor.manifest);
  descriptor.native_build = manifest.at("native_build").get<std::string>();
  descriptor.runtime_identifier = manifest.at("rid").get<std::string>();
  descriptor.native = {runtime_directory / manifest.at("native").at("file_name").get<std::string>(),
                       manifest.at("native").at("sha256").get<std::string>()};
#ifdef _WIN32
  descriptor.ort.path = runtime_directory / "onnxruntime.dll";
  descriptor.genai.path = runtime_directory / "onnxruntime-genai.dll";
#elif defined(__APPLE__)
  descriptor.ort.path = runtime_directory / "libonnxruntime.dylib";
  descriptor.genai.path = runtime_directory / "libonnxruntime-genai.dylib";
#else
  descriptor.ort.path = runtime_directory / "libonnxruntime.so";
  descriptor.genai.path = runtime_directory / "libonnxruntime-genai.so";
#endif
#ifndef _WIN32
  std::filesystem::permissions(root_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
#endif
  descriptor.ort.sha256 = Sha256File(descriptor.ort.path);
  descriptor.genai.sha256 = Sha256File(descriptor.genai.path);
  const auto profile = BuildHostProfile(descriptor_.configuration, {descriptor.native_build,
                                                                    descriptor.runtime_identifier,
                                                                    SdkConnection::kProtocolVersion});
  descriptor.compatibility = profile.compatibility;
  descriptor.configuration = profile.configuration;
  descriptor.child = {root_ / "s", profile.key, root_.filename().string(), CurrentProcessIdentity()};
#ifdef _WIN32
  descriptor.child.endpoint = LR"(\\.\pipe\foundry-host-bootstrap-test-)" + root_.filename().native();
#else
  const auto socket_directory = test::MakePrivateIpcDirectory();
  ScopeGuard remove_socket_directory([&]() noexcept {
    std::error_code error;
    std::filesystem::remove_all(socket_directory, error);
  });
  descriptor.child.endpoint = socket_directory / "s";
#endif
  const auto file = root_ / "bootstrap.json";
  Write(file, EncodeBootstrapDescriptor(descriptor).dump());
  std::shared_ptr<api::ManagerBackend> metadata = api::CreateEmbeddedBackend(profile.configuration);
  ModelInfo info;
  info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "coding");
  auto* local_model =
      metadata->GetCatalog(CatalogType::kLocal)
          .RegisterModel((std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention").string(),
                         "bootstrap-model:1", info);
  HostChild host(executable, file, owns_root_);
  std::optional<Channel> channel;
  const auto deadline = Clock::now() + 20s;
  while (!channel && Clock::now() < deadline) {
    try {
      channel.emplace(Connect(descriptor.child.endpoint));
    } catch (const Exception&) {
      ASSERT_FALSE(host.WaitForExit(Clock::now())) << "Native host exited before listening: " << host.ExitCode();
      std::this_thread::sleep_for(10ms);
    }
  }

  ASSERT_TRUE(channel);
  const auto process = CurrentProcessIdentity();
  const Json handshake{{"protocol", SdkConnection::kProtocolVersion},
                       {"profile", profile.key},
                       {"owner", true},
                       {"process", {{"pid", process.pid}, {"generation", process.generation}}}};
  auto first = std::make_shared<SdkConnection>(std::move(*channel), SdkConnection::Side::Client);
  const auto first_reply = first->Call(Operation::Handshake, handshake);
  ASSERT_EQ(first_reply.status, 0) << first_reply.error;
  EXPECT_EQ(first_reply.value.at("process").at("pid"), host.Pid());
  const auto published =
      ReadBootstrapJson(std::filesystem::path(file.native() + std::filesystem::path(".process").native()));
  EXPECT_EQ(published.at("process"), first_reply.value.at("process"));
  EXPECT_EQ(published.at("incarnation"), descriptor.child.incarnation);
  auto second = std::make_shared<SdkConnection>(Connect(descriptor.child.endpoint), SdkConnection::Side::Client);
  const auto second_reply = second->Call(Operation::Handshake, handshake);
  ASSERT_EQ(second_reply.status, 0) << second_reply.error;
  EXPECT_EQ(first->Call(Operation::Observe, {}).value.at("owners"), 2);
  auto first_context =
      std::make_shared<api::HostedContext>(first, first_reply.value.at("incarnation").get<std::string>(),
                                           first_reply.value.at("connection").get<uint64_t>());
  auto second_context =
      std::make_shared<api::HostedContext>(second, second_reply.value.at("incarnation").get<std::string>(),
                                           second_reply.value.at("connection").get<uint64_t>());
  auto first_manager = api::CreateHostedBackend(metadata, [first_context](bool) { return first_context; });
  auto second_manager = api::CreateHostedBackend(metadata, [second_context](bool) { return second_context; });
  auto* first_model = first_manager->GetCatalog(CatalogType::kLocal).GetModelVariant("bootstrap-model:1");
  auto* second_model = second_manager->GetCatalog(CatalogType::kLocal).GetModelVariant("bootstrap-model:1");
  first_model->Load();
  EXPECT_TRUE(second_model->IsLoaded());
  EXPECT_FALSE(local_model->IsLoaded());
  auto first_session = first_model->CreateSession();
  auto second_session = second_model->CreateSession();
  Request request;
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Remember the word sapphire."));
  request.options.Add("max_output_tokens", "17");
  request.options.Add("temperature", "0");
  Response first_response;
  first_session->ProcessRequest(request, first_response);
  EXPECT_EQ(first_response.usage.completion_tokens, 17);
  EXPECT_EQ(first_session->TurnCount(), 1u);
  EXPECT_EQ(second_session->TurnCount(), 0u);
  EXPECT_THROW(second_model->Unload(), Exception);
  first_session.reset();
  first_manager->Shutdown();
  EXPECT_EQ(second->Call(Operation::Observe, {}).status, 0);
  EXPECT_FALSE(host.WaitForExit(Clock::now()));
  Response second_response;
  second_session->ProcessRequest(request, second_response);
  EXPECT_EQ(second_response.usage.completion_tokens, 17);
  EXPECT_EQ(second_session->TurnCount(), 1u);
  EXPECT_FALSE(local_model->IsLoaded());
  second_session.reset();
  second_model->Unload();
  second_manager->Shutdown();
  ASSERT_TRUE(host.WaitForExit(Clock::now() + 15s));
  EXPECT_EQ(host.ExitCode(), 0);
}

TEST_F(BootstrapDescriptorTest, CoordinatorReportsChildStartupFailureAndRetiresItsExitedLaunch) {
  auto assets = ResidentAssets();
  assets.bootstrap.ort = Artifact("invalid-ort", "not a loadable library");
  HostCoordinator coordinator(descriptor_.configuration, std::move(assets));
  try {
    coordinator.Connect(true);
    FAIL() << "Host started with an unloadable ORT library";
  } catch (const Exception& error) {
    EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_INTERNAL);
    const std::string message = error.what();
    EXPECT_NE(message.find("native host exited before owner attachment"), std::string::npos);
#ifdef _WIN32
    EXPECT_NE(message.find("load native host artifact"), std::string::npos);
#else
    EXPECT_NE(message.find("invalid-ort"), std::string::npos);
#endif
  }

  EXPECT_FALSE(coordinator.Store().Read());
}

TEST_F(BootstrapDescriptorTest, CoordinatorStartsOnceSharesOwnersReconnectsAndReplacesOnlyAfterActualExit) {
  const auto assets = ResidentAssets();
  HostCoordinator first(descriptor_.configuration, assets);
  HostCoordinator second(descriptor_.configuration, assets);
  const bool existed = first.Store().Exists();
  EXPECT_FALSE(first.Connect(false));
  EXPECT_EQ(first.Store().Exists(), existed);
  std::shared_ptr<api::HostedContext> owner_a, owner_b, reconnected, successor;
  ScopeGuard cleanup([&]() noexcept {
    for (const auto& owner : {owner_a, owner_b, reconnected, successor}) {
      if (owner) {
        owner->Close();
      }
    }
    try {
      const auto record = first.Store().Read();
      if (record && record->process && ProbeProcessIdentity(*record->process) == ProcessIdentityProbe::kMatching) {
        ProcessWatch watch(*record->process);
        if (!watch.WaitUntil(Clock::now() + 35s)) {
          owns_root_ = false;
          ADD_FAILURE() << "Coordinator child did not exit; preserving scratch state";
        }
      } else if (record && !record->process) {
        owns_root_ = false;
        ADD_FAILURE() << "Coordinator child identity is uncertain; preserving scratch state";
      }
    } catch (const std::exception& error) {
      owns_root_ = false;
      ADD_FAILURE() << error.what();
    }
  });
#ifndef _WIN32
  int application_pipe[2]{-1, -1};
  ASSERT_EQ(pipe(application_pipe), 0);
  ScopeGuard close_application_pipe([&]() noexcept {
    close(application_pipe[0]);
    if (application_pipe[1] >= 0) {
      close(application_pipe[1]);
    }
  });
  ASSERT_EQ(fcntl(application_pipe[0], F_SETFL, O_NONBLOCK), 0);
#endif
  auto a = std::async(std::launch::async, [&] { return first.Connect(true); });
  auto b = std::async(std::launch::async, [&] { return second.Connect(true); });
  owner_a = a.get();
  owner_b = b.get();
  ASSERT_TRUE(owner_a);
  ASSERT_TRUE(owner_b);
#ifndef _WIN32
  close(application_pipe[1]);
  application_pipe[1] = -1;
  char byte{};
  EXPECT_EQ(read(application_pipe[0], &byte, 1), 0) << "Host retained the application's pipe writer";
#endif
  const auto record = first.Store().Read();
  ASSERT_TRUE(record && record->process);
  ProcessWatch predecessor(*record->process);
  EXPECT_EQ(owner_a->Call(Operation::Observe, Json::object()).at("owners"), 2);
  owner_a->Close();
  EXPECT_EQ(owner_b->Call(Operation::Observe, Json::object()).at("state"), 1);
  owner_b->Close();
  reconnected = first.Connect(true);
  EXPECT_EQ(first.Store().Read()->process, record->process);
  EXPECT_FALSE(predecessor.IsExited());
  reconnected->Close();
  ASSERT_TRUE(predecessor.WaitUntil(Clock::now() + 15s));
  EXPECT_FALSE(first.Connect(false));
  successor = second.Connect(true);
  const auto replacement = second.Store().Read();
  ASSERT_TRUE(replacement && replacement->process);
  EXPECT_NE(replacement->child.incarnation, record->child.incarnation);
  EXPECT_NE(replacement->process, record->process);
  ProcessWatch replacement_exit(*replacement->process);
  successor->Close();
  ASSERT_TRUE(replacement_exit.WaitUntil(Clock::now() + 15s));
  EXPECT_FALSE(second.Connect(false));
}

TEST_F(BootstrapDescriptorTest, PublicConfigurationLazilyRoutesExistingApisThroughThePackagedHost) {
  const auto cached_path = std::filesystem::path(*descriptor_.configuration.model_cache_dir) / "tiny-cache-1";
  std::filesystem::create_directories(cached_path.parent_path());
  std::filesystem::copy(std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention", cached_path,
                        std::filesystem::copy_options::recursive);
  Write(cached_path / "inference_model.json", Json{{"Name", "tiny-cache:1"}}.dump());
  ModelInfo cached_info;
  cached_info.model_id = "tiny-cache:1";
  cached_info.name = "tiny-cache";
  cached_info.alias = "tiny-cache";
  cached_info.version = 1;
  cached_info.device_type = DeviceType::kCPU;
  cached_info.execution_provider = "CPUExecutionProvider";
  cached_info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "coding");
  Write(cached_path.parent_path() / "foundry.modelinfo.json",
        Json{{"version", 1}, {"savedAtUnix", 0}, {"models", Json::array({ModelInfoToJson(cached_info)})}}.dump());
  descriptor_.configuration.catalog_urls = {{"http://127.0.0.1:0", std::nullopt}};

  WithRuntime([&](const auto get_api) -> void {
    const auto* native = get_api(3);
    ASSERT_NE(native, nullptr);
    const auto* config_api = native->GetConfigurationApi();
    const auto* model_api = native->GetModelApi();
    const auto* inference = native->GetInferenceApi();
    const auto* items = native->GetItemApi();
    HostCoordinator coordinator(descriptor_.configuration, ResidentAssets());
    EXPECT_FALSE(coordinator.Connect(false));
    const bool existed = coordinator.Store().Exists();
    ScopeGuard child_exit([&]() noexcept {
      try {
        const auto record = coordinator.Store().Read();
        if (record) {
          if (!record->process) {
            owns_root_ = false;
            ADD_FAILURE() << "Public API child identity is uncertain; preserving scratch state";
          } else {
            ProcessWatch watch(*record->process);
            if (!watch.WaitUntil(Clock::now() + 35s)) {
              owns_root_ = false;
              ADD_FAILURE() << "Public API child did not exit; preserving scratch state";
            } else {
              EXPECT_FALSE(coordinator.Connect(false));
            }
          }
        }
      } catch (const std::exception& error) {
        owns_root_ = false;
        ADD_FAILURE() << error.what();
      }
    });
    flConfiguration* config = nullptr;
    flManager* manager = nullptr;
    flModelInfo* info = nullptr;
    flKeyValuePairs* options = nullptr;
    flKeyValuePairs* generation = nullptr;
    flSession* first = nullptr;
    flSession* second = nullptr;
    flRequest* request = nullptr;
    flResponse* response = nullptr;
    flItem* text = nullptr;
    flItem* message = nullptr;
    ScopeGuard release([&]() noexcept {
      inference->Session_Release(first);
      inference->Session_Release(second);
      inference->Request_Release(request);
      inference->Response_Release(response);
      items->Item_Release(message);
      items->Item_Release(text);
      model_api->ReleaseModelInfo(info);
      native->KeyValuePairs_Release(options);
      native->KeyValuePairs_Release(generation);
      native->Manager_Release(manager);
      config_api->Configuration_Release(config);
    });
    ASSERT_FL_OK(native, config_api->Create(descriptor_.configuration.app_name.c_str(), &config));
    ASSERT_FL_OK(native, config_api->SetUseSharedHost(config, true));
    const auto relative_data = std::filesystem::relative(*descriptor_.configuration.app_data_dir).string();
    const auto relative_cache = std::filesystem::relative(*descriptor_.configuration.model_cache_dir).string();
    const auto relative_logs = std::filesystem::relative(*descriptor_.configuration.logs_dir).string();
    ASSERT_FL_OK(native, config_api->SetAppDataDir(config, relative_data.c_str()));
    ASSERT_FL_OK(native, config_api->SetModelCacheDir(config, relative_cache.c_str()));
    ASSERT_FL_OK(native, config_api->SetLogsDir(config, relative_logs.c_str()));
    ASSERT_FL_OK(native, config_api->SetCatalogRegion(config, descriptor_.configuration.catalog_region->c_str()));
    for (const auto& [url, filter] : descriptor_.configuration.catalog_urls) {
      ASSERT_FL_OK(native, config_api->AddCatalogUrl(config, url.c_str(), filter ? filter->c_str() : nullptr));
    }

    for (const auto& endpoint : descriptor_.configuration.web_service_endpoints) {
      ASSERT_FL_OK(native, config_api->AddWebServiceEndpoint(config, endpoint.c_str()));
    }

    native->CreateKeyValuePairs(&options);
    for (const auto& [key, value] : descriptor_.configuration.additional_options) {
      native->AddKeyValuePair(options, key.c_str(), value.c_str());
    }

    native->AddKeyValuePair(options, "DisableNonessentialTelemetry", "true");
    ASSERT_FL_OK(native, config_api->SetAdditionalOptions(config, options));
    ASSERT_FL_OK(native, native->Manager_Create(config, &manager));
    EXPECT_EQ(coordinator.Store().Exists(), existed);
    EXPECT_FALSE(coordinator.Store().Read());

    ASSERT_FL_OK(native, config_api->SetUseSharedHost(config, false));
    flManager* conflicting = nullptr;
    test::StatusGuard conflict{native->Manager_Create(config, &conflicting), native};
    ASSERT_NE(conflict.s, nullptr);
    EXPECT_EQ(native->Status_GetErrorCode(conflict.s), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_EQ(conflicting, nullptr);

    flCatalog* catalog = nullptr;
    ASSERT_FL_OK(native, native->Manager_GetCatalogByType(manager, FOUNDRY_LOCAL_CATALOG_LOCAL, &catalog));
    ASSERT_FL_OK(native, model_api->CreateModelInfo(&info));
    ASSERT_FL_OK(native, model_api->Info_SetStringProperty(info, FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "coding"));
    flModel* model = nullptr;
    const auto path = (std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention").string();
    ASSERT_FL_OK(native,
                 native->GetCatalogApi()->RegisterModel(catalog, path.c_str(), "public-host-model:1", info, &model));
    int loaded = -1;
    ASSERT_FL_OK(native, model_api->IsLoaded(model, &loaded));
    EXPECT_EQ(loaded, 0);
    EXPECT_FALSE(coordinator.Store().Read());
    flCatalog* public_catalog = nullptr;
    flModel* cached_model = nullptr;
    ASSERT_FL_OK(native, native->Manager_GetCatalogByType(manager, FOUNDRY_LOCAL_CATALOG_PUBLIC, &public_catalog));
    ASSERT_FL_OK(native, native->GetCatalogApi()->GetModelVariant(public_catalog, "tiny-cache:1", &cached_model));
    ASSERT_NE(cached_model, nullptr);
    int cached = 0;
    ASSERT_FL_OK(native, model_api->IsCached(cached_model, &cached));
    ASSERT_EQ(cached, 1);
    EXPECT_FALSE(coordinator.Store().Read());
    ASSERT_FL_OK(native, model_api->Load(cached_model));
    ASSERT_FL_OK(native, inference->Session_Create(cached_model, &first));
    inference->Session_Release(first);
    first = nullptr;
    ASSERT_FL_OK(native, model_api->Unload(cached_model));
    ASSERT_FL_OK(native, model_api->Load(model));
    const auto record = coordinator.Store().Read();
    ASSERT_TRUE(record && record->process);
    ProcessWatch process(*record->process);
    ASSERT_FL_OK(native, model_api->IsLoaded(model, &loaded));
    EXPECT_EQ(loaded, 1);
#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
    ASSERT_FL_OK(native, native->Manager_WebServiceStart(manager));
    const char* const* urls = nullptr;
    size_t url_count = 0;
    ASSERT_FL_OK(native, native->Manager_WebServiceUrls(manager, &urls, &url_count));
    ASSERT_EQ(url_count, 1u);
    {
      httplib::Client http(urls[0]);
      http.set_read_timeout(10, 0);
      const auto inventory = http.Get("/catalogs/local/models/loaded?details=true");
      ASSERT_TRUE(inventory);
      EXPECT_EQ(inventory->status, 200);
      const auto models = Json::parse(inventory->body);
      ASSERT_TRUE(models.is_array());
      EXPECT_EQ(models.size(), 1u);
    }
#endif
    ASSERT_FL_OK(native, inference->Session_Create(model, &first));
    ASSERT_FL_OK(native, inference->Session_Create(model, &second));
    ASSERT_FL_OK(native, inference->Request_Create(&request));
    ASSERT_FL_OK(native, items->Create(FOUNDRY_LOCAL_ITEM_TEXT, &text));
    const flTextData text_data{FOUNDRY_LOCAL_DATA_VERSION, "Hello", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT};
    ASSERT_FL_OK(native, items->SetText(text, &text_data));
    ASSERT_FL_OK(native, items->Create(FOUNDRY_LOCAL_ITEM_MESSAGE, &message));
    const flItem* parts[]{text};
    const flMessageData message_data{FOUNDRY_LOCAL_DATA_VERSION, FOUNDRY_LOCAL_ROLE_USER, parts, 1, nullptr};
    ASSERT_FL_OK(native, items->SetMessage(message, &message_data));
    ASSERT_FL_OK(native, inference->Request_AddItem(request, message, false));
    native->CreateKeyValuePairs(&generation);
    native->AddKeyValuePair(generation, FOUNDRY_LOCAL_PARAM_MAX_OUTPUT_TOKENS, "8");
    ASSERT_FL_OK(native, inference->Request_SetOptions(request, generation));
    ASSERT_FL_OK(native, inference->Session_ProcessRequest(first, request, &response));
    EXPECT_GT(inference->Response_GetItemCount(response), 0u);
    EXPECT_EQ(inference->Session_GetTurnCount(first), 1u);
    EXPECT_EQ(inference->Session_GetTurnCount(second), 0u);
    test::StatusGuard busy{model_api->Unload(model), native};
    ASSERT_NE(busy.s, nullptr);
    EXPECT_EQ(native->Status_GetErrorCode(busy.s), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    inference->Session_Release(first);
    first = nullptr;
    inference->Session_Release(second);
    second = nullptr;
    ASSERT_FL_OK(native, model_api->Unload(model));
    auto observer = coordinator.Connect(false);
    ASSERT_TRUE(observer);
    EXPECT_EQ(observer->Call(Operation::Observe, Json::object()).at("owners"), 1);
    observer->Close();
    ASSERT_FL_OK(native, native->Manager_Shutdown(manager));
    EXPECT_TRUE(native->Manager_IsShutdownRequested(manager));
    ASSERT_TRUE(process.WaitUntil(Clock::now() + 15s));
    EXPECT_FALSE(coordinator.Connect(false));
  });
}

TEST_F(BootstrapDescriptorTest, CoordinatorPreservesUnknownInterruptedLaunchInsteadOfSpawningAgain) {
  HostCoordinator coordinator(descriptor_.configuration, ResidentAssets(), {}, 100ms);
  HostLaunchRecord record;
  {
    const auto lock = coordinator.Store().Lock(Clock::now() + 1s);
    auto starter = CurrentProcessIdentity();
    ++starter.generation;
    const auto incarnation = NewHostIncarnation();
    record = {{coordinator.Store().Endpoint(incarnation), coordinator.Store().Directory().filename().string(),
               incarnation, starter},
              std::nullopt};
    coordinator.Store().Write(record);
  }

  ScopeGuard cleanup([&]() noexcept { RemoveFabricatedRecord(coordinator.Store(), record); });
  EXPECT_THROW(coordinator.Connect(true), Exception);
  EXPECT_EQ(coordinator.Store().Read()->child.incarnation, record.child.incarnation);
  EXPECT_FALSE(coordinator.Store().Read()->process);
  EXPECT_FALSE(std::filesystem::exists(coordinator.Store().BootstrapPath(record)));
}

#ifndef _WIN32
TEST_F(BootstrapDescriptorTest, StoppedLauncherHonorsStartupDeadlineAndIsReaped) {
  const HostStateStore store(descriptor_.child.profile_key, root_ / "state");
  const auto lock = store.Lock(Clock::now() + 1s);
  const auto file = root_ / "stopped-launcher";
  Write(file, "stop");
  const auto start = Clock::now();
  EXPECT_THROW(SpawnNativeHost(std::filesystem::path(FOUNDRY_PROCESS_FIXTURE_PATH), file, *lock, start + 1s),
               Exception);
  EXPECT_LT(Clock::now() - start, 3s);
  std::ifstream input(file.string() + ".pid");
  pid_t pid{};
  ASSERT_TRUE(input >> pid);
  int status{};
  EXPECT_EQ(waitpid(pid, &status, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
}

#endif

TEST_F(BootstrapDescriptorTest, CoordinatorNeverTreatsAMissingEndpointAsExitOfAKnownLiveProcess) {
  HostCoordinator coordinator(descriptor_.configuration, ResidentAssets(), {}, 100ms);
  HostLaunchRecord record;
  {
    const auto lock = coordinator.Store().Lock(Clock::now() + 1s);
    const auto incarnation = NewHostIncarnation();
    record = {{coordinator.Store().Endpoint(incarnation), coordinator.Store().Directory().filename().string(),
               incarnation, CurrentProcessIdentity()},
              CurrentProcessIdentity()};
    coordinator.Store().Write(record);
  }

  ScopeGuard cleanup([&]() noexcept { RemoveFabricatedRecord(coordinator.Store(), record); });
  try {
    static_cast<void>(coordinator.Connect(true));
    FAIL() << "A known live process without its endpoint must not permit replacement";
  } catch (const Exception& error) {
    EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_NE(std::string(error.what()).find("deadline"), std::string::npos);
  }

  EXPECT_EQ(coordinator.Store().Read()->process, record.process);
  EXPECT_EQ(coordinator.Store().Read()->child.incarnation, record.child.incarnation);
  EXPECT_FALSE(std::filesystem::exists(coordinator.Store().BootstrapPath(record)));
}

}  // namespace
}  // namespace fl::hosting
