// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/sha256.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#include "exception.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt")

namespace fl {

namespace {

std::string HexEncode(const unsigned char* digest, size_t digest_len) {
  std::ostringstream hex;
  hex << std::hex << std::uppercase << std::setfill('0');
  for (size_t i = 0; i < digest_len; ++i) {
    hex << std::setw(2) << static_cast<int>(digest[i]);
  }
  return hex.str();
}

void ThrowBCryptError(const char* call, NTSTATUS status) {
  std::ostringstream oss;
  oss << call << " failed (NTSTATUS=0x" << std::hex << std::uppercase << std::setfill('0')
      << std::setw(8) << static_cast<unsigned long>(status) << ")";
  FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, oss.str());
}

}  // namespace

std::string Sha256File(const std::filesystem::path& file_path) {
  std::ifstream file(file_path, std::ios::binary);
  if (!file) {
    return {};
  }

  BCRYPT_ALG_HANDLE alg = nullptr;
  NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    ThrowBCryptError("BCryptOpenAlgorithmProvider", status);
  }

  BCRYPT_HASH_HANDLE hash = nullptr;
  status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(alg, 0);  // best-effort cleanup; ignore secondary failure
    ThrowBCryptError("BCryptCreateHash", status);
  }

  // Read file in 64KB chunks to avoid loading entire file into memory
  char buf[65536];
  while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
    status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf), static_cast<ULONG>(file.gcount()), 0);
    if (!BCRYPT_SUCCESS(status)) {
      BCryptDestroyHash(hash);
      BCryptCloseAlgorithmProvider(alg, 0);
      ThrowBCryptError("BCryptHashData", status);
    }
  }

  UCHAR digest[32];
  status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    ThrowBCryptError("BCryptFinishHash", status);
  }

  // Cleanup; failures here cannot be meaningfully reported (no logger in scope) and the
  // hash result is already valid, so swallow them.
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);

  return HexEncode(digest, sizeof(digest));
}

std::string Sha256String(std::string_view value) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    ThrowBCryptError("BCryptOpenAlgorithmProvider", status);
  }

  BCRYPT_HASH_HANDLE hash = nullptr;
  status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(alg, 0);
    ThrowBCryptError("BCryptCreateHash", status);
  }

  for (size_t offset = 0; offset < value.size();) {
    const size_t chunk_size = std::min<size_t>(
        value.size() - offset, static_cast<size_t>((std::numeric_limits<ULONG>::max)()));
    status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data() + offset)),
                            static_cast<ULONG>(chunk_size), 0);
    if (!BCRYPT_SUCCESS(status)) {
      BCryptDestroyHash(hash);
      BCryptCloseAlgorithmProvider(alg, 0);
      ThrowBCryptError("BCryptHashData", status);
    }
    offset += chunk_size;
  }

  UCHAR digest[32];
  status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    ThrowBCryptError("BCryptFinishHash", status);
  }

  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  return HexEncode(digest, sizeof(digest));
}

}  // namespace fl

#else  // POSIX — use OpenSSL EVP API

#include <openssl/evp.h>

namespace fl {

namespace {

std::string HexEncode(const unsigned char* digest, size_t digest_len) {
  std::ostringstream hex;
  hex << std::hex << std::uppercase << std::setfill('0');
  for (size_t i = 0; i < digest_len; ++i) {
    hex << std::setw(2) << static_cast<int>(digest[i]);
  }
  return hex.str();
}

}  // namespace

std::string Sha256File(const std::filesystem::path& file_path) {
  std::ifstream file(file_path, std::ios::binary);
  if (!file) {
    return {};
  }

  auto* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "EVP_MD_CTX_new failed (out of memory)");
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "EVP_DigestInit_ex failed");
  }

  char buf[65536];
  while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
    if (EVP_DigestUpdate(ctx, buf, static_cast<size_t>(file.gcount())) != 1) {
      EVP_MD_CTX_free(ctx);
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "EVP_DigestUpdate failed");
    }
  }

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
    EVP_MD_CTX_free(ctx);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "EVP_DigestFinal_ex failed");
  }
  EVP_MD_CTX_free(ctx);

  return HexEncode(digest, digest_len);
}

std::string Sha256String(std::string_view value) {
  auto* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "EVP_MD_CTX_new failed (out of memory)");
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "EVP_DigestInit_ex failed");
  }

  if (EVP_DigestUpdate(ctx, value.data(), value.size()) != 1) {
    EVP_MD_CTX_free(ctx);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "EVP_DigestUpdate failed");
  }

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
    EVP_MD_CTX_free(ctx);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "EVP_DigestFinal_ex failed");
  }
  EVP_MD_CTX_free(ctx);

  return HexEncode(digest, digest_len);
}

}  // namespace fl

#endif
