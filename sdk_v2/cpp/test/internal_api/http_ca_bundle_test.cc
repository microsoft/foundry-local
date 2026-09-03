// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Unit tests for Linux CA bundle resolution.
#include "http/http_client.h"

#include <gtest/gtest.h>

#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

using namespace fl::http;

namespace {

/// Builds a `get_env` callable backed by an in-memory map, mirroring `Utils::GetEnv`'s
/// `std::optional<std::string>` signature (nullopt = unset).
std::function<std::optional<std::string>(const char*)> FakeEnv(
    std::initializer_list<std::pair<const char*, const char*>> vars) {
  auto table = std::make_shared<std::map<std::string, std::string>>();
  for (const auto& [key, value] : vars) {
    (*table)[key] = value;
  }

  return [table](const char* name) -> std::optional<std::string> {
    auto it = table->find(name);
    return it == table->end() ? std::nullopt : std::optional<std::string>(it->second);
  };
}

/// Builds a `path_exists` callable backed by an in-memory set of "present" paths.
std::function<bool(std::string_view)> FakeFilesystem(std::set<std::string> present_paths) {
  auto present = std::make_shared<std::set<std::string>>(std::move(present_paths));
  return [present](std::string_view path) { return present->count(std::string(path)) != 0; };
}

}  // namespace

TEST(ResolveLinuxCaBundleTest, PrefersExplicitSslCertFile) {
  auto get_env = FakeEnv({{"SSL_CERT_FILE", "/custom/ca.pem"}});
  auto path_exists = FakeFilesystem({"/etc/ssl/certs/ca-certificates.crt"});

  EXPECT_EQ(ResolveLinuxCaBundle(get_env, path_exists), "/custom/ca.pem");
}

TEST(ResolveLinuxCaBundleTest, IgnoresEmptyEnvOverridesAndFallsBackToProbing) {
  auto get_env = FakeEnv({{"SSL_CERT_FILE", ""}});
  auto path_exists = FakeFilesystem({"/etc/ssl/certs/ca-certificates.crt"});

  EXPECT_EQ(ResolveLinuxCaBundle(get_env, path_exists), "/etc/ssl/certs/ca-certificates.crt");
}

TEST(ResolveLinuxCaBundleTest, ProbesWellKnownDistroPathsInPriorityOrder) {
  // Simulate an AlmaLinux/RHEL-style host: no Debian/Ubuntu bundle, but the Fedora/RHEL one exists.
  auto get_env = FakeEnv({});
  auto path_exists = FakeFilesystem({"/etc/pki/tls/certs/ca-bundle.crt"});

  EXPECT_EQ(ResolveLinuxCaBundle(get_env, path_exists), "/etc/pki/tls/certs/ca-bundle.crt");
}

TEST(ResolveLinuxCaBundleTest, PrefersDebianPathOverRhelPathWhenBothExist) {
  auto get_env = FakeEnv({});
  auto path_exists = FakeFilesystem({
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/tls/certs/ca-bundle.crt",
  });

  EXPECT_EQ(ResolveLinuxCaBundle(get_env, path_exists), "/etc/ssl/certs/ca-certificates.crt");
}

TEST(ResolveLinuxCaBundleTest, ReturnsEmptyWhenNoOverrideAndNoKnownPathExists) {
  auto get_env = FakeEnv({});
  auto path_exists = FakeFilesystem({});

  EXPECT_EQ(ResolveLinuxCaBundle(get_env, path_exists), "");
}
