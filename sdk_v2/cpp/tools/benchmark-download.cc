// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "download/blob_download_state.h"
#include "download/blob_downloader.h"
#include "download/file_writer.h"
#include "logger.h"
#include "utils.h"

#include <azure/core/base64.hpp>
#include <azure/core/cryptography/hash.hpp>
#include <azure/core/http/transport.hpp>
#include <azure/core/io/body_stream.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr int64_t kChunkSize = 2 * 1024 * 1024;

// The immutable range body and its service checksum are prepared outside the timed region.
// Exercises Azure response parsing, workers, hashing, file I/O, and receipts without network variance.
class LocalTransport final : public Azure::Core::Http::HttpTransport {
 public:
  explicit LocalTransport(int64_t size) : size_(size), payload_(static_cast<size_t>(kChunkSize)) {
    uint32_t value = 0x12345678;
    for (auto& byte : payload_) {
      value ^= value << 13;
      value ^= value >> 17;
      value ^= value << 5;
      byte = static_cast<uint8_t>(value);
    }

    Azure::Core::Cryptography::Md5Hash hash;
    checksum_ = Azure::Core::Convert::Base64Encode(hash.Final(payload_.data(), payload_.size()));
  }

  std::unique_ptr<Azure::Core::Http::RawResponse> Send(Azure::Core::Http::Request& request,
                                                       const Azure::Core::Context&) override {
    using namespace Azure::Core::Http;
    const bool head = request.GetMethod() == HttpMethod::Head;
    auto response = std::make_unique<RawResponse>(1, 1, head ? HttpStatusCode::Ok : HttpStatusCode::PartialContent,
                                                  "OK");
    response->SetHeader("etag", "\"benchmark\"");
    response->SetHeader("last-modified", "Wed, 14 Oct 2026 00:00:00 GMT");
    response->SetHeader("x-ms-creation-time", "Wed, 14 Oct 2026 00:00:00 GMT");
    response->SetHeader("x-ms-server-encrypted", "true");
    response->SetHeader("x-ms-blob-type", "BlockBlob");
    response->SetHeader("content-length", std::to_string(head ? size_ : kChunkSize));
    response->SetBodyStream(std::make_unique<Azure::Core::IO::MemoryBodyStream>(nullptr, 0));
    if (!head) {
      const auto headers = request.GetHeaders();
      const auto& range = headers.at("x-ms-range");
      const auto dash = range.find('-');
      const auto offset = std::stoll(range.substr(6, dash - 6));
      const auto end = std::stoll(range.substr(dash + 1));
      if (offset < 0 || end >= size_ || end - offset + 1 != kChunkSize ||
          headers.at("x-ms-range-get-content-md5") != "true" || headers.at("if-match") != "\"benchmark\"") {
        throw std::runtime_error("Unexpected benchmark range request");
      }

      response->SetHeader("content-range",
                          "bytes " + std::to_string(offset) + "-" + std::to_string(end) + "/" + std::to_string(size_));
      response->SetHeader("content-md5", checksum_);
      response->SetBodyStream(std::make_unique<Azure::Core::IO::MemoryBodyStream>(payload_));
      get_count.fetch_add(1);
    }

    return response;
  }

  std::atomic<int64_t> get_count{0};

  const std::vector<uint8_t>& Payload() const { return payload_; }

 private:
  int64_t size_;
  std::vector<uint8_t> payload_;
  std::string checksum_;
};

double Seconds(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double>(end - start).count();
}

void BenchmarkIo(const std::filesystem::path& directory, int64_t bytes, int runs, int concurrency,
                 const std::vector<uint8_t>& payload, fl::ILogger& logger) {
  Azure::Core::Cryptography::Md5Hash expected_hash;
  const auto expected = expected_hash.Final(payload.data(), payload.size());
  for (int run = 0; run < runs; ++run) {
    const auto file = directory / ("io-benchmark-" + std::to_string(run) + ".bin");
    if (std::filesystem::exists(file)) {
      throw std::runtime_error("Benchmark output already exists; use a fresh output directory");
    }

    const auto start = Clock::now();
    fl::FileWriter writer(logger);
    writer.Open(file, bytes);
    std::atomic<int64_t> next{0};
    std::vector<std::future<void>> workers;
    for (int worker = 0; worker < concurrency; ++worker) {
      workers.push_back(std::async(std::launch::async, [&]() {
        for (auto offset = next.fetch_add(kChunkSize); offset < bytes; offset = next.fetch_add(kChunkSize)) {
          for (size_t piece = 0; piece < payload.size(); piece += 64 * 1024) {
            writer.WriteAt(offset + static_cast<int64_t>(piece), payload.data() + piece, 64 * 1024);
          }
        }
      }));
    }

    for (auto& worker : workers) {
      worker.get();
    }

    const auto written = Clock::now();
    writer.Finish();
    const auto closed = Clock::now();
    std::ifstream input(file, std::ios::binary);
    std::array<uint8_t, 64 * 1024> buffer;
    for (int64_t offset = 0; offset < bytes; offset += kChunkSize) {
      Azure::Core::Cryptography::Md5Hash hash;
      for (int64_t piece = 0; piece < kChunkSize; piece += static_cast<int64_t>(buffer.size())) {
        if (!input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()))) {
          throw std::runtime_error("Benchmark readback failed");
        }

        hash.Append(buffer.data(), buffer.size());
      }

      if (hash.Final() != expected) {
        throw std::runtime_error("Benchmark readback checksum mismatch");
      }
    }

    const auto end = Clock::now();
    input.close();
    std::printf(
        "{\"mode\":\"io\",\"run\":%d,\"bytes\":%lld,\"concurrency\":%d,"
        "\"write_s\":%.6f,\"close_s\":%.6f,\"readback_s\":%.6f,\"total_s\":%.6f}\n",
        run, static_cast<long long>(bytes), concurrency, Seconds(start, written), Seconds(written, closed),
        Seconds(closed, end), Seconds(start, end));
    std::fflush(stdout);
    std::filesystem::remove(file);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc != 6 && argc != 7) {
      throw std::runtime_error(
          "Usage: benchmark-download <output-dir> <bytes> <runs> <concurrency> <local|live|io> [blob-name]\n"
          "Live mode reads the container SAS URI from FOUNDRY_BENCHMARK_SAS; bytes must match the blob size.");
    }

    const auto directory = std::filesystem::absolute(argv[1]);
    const auto bytes = std::stoll(argv[2]);
    const double mib = static_cast<double>(bytes) / (1024 * 1024);
    const int runs = std::stoi(argv[3]);
    const int concurrency = std::stoi(argv[4]);
    const std::string mode = argv[5];
    if (bytes <= 0 || bytes > 64 * 1024 * 1024 * int64_t{1024} || runs <= 0 || concurrency <= 0 ||
        (mode != "local" && mode != "live" && mode != "io") || (mode != "live" && bytes % kChunkSize != 0)) {
      throw std::runtime_error(
          "Expected positive size (at most 64 GiB), runs, and concurrency; local and io sizes must be 2 MiB aligned");
    }

    std::string sas = "https://benchmark.blob.core.windows.net/container";
    std::string blob = "benchmark.bin";
    std::shared_ptr<LocalTransport> transport;
    if (mode != "live") {
      transport = std::make_shared<LocalTransport>(bytes);
    } else {
      const auto value = fl::Utils::GetEnv("FOUNDRY_BENCHMARK_SAS");
      if (argc != 7 || !value || value->empty()) {
        throw std::runtime_error("Live mode requires a blob name and FOUNDRY_BENCHMARK_SAS");
      }

      sas = *value;
      blob = argv[6];
    }

    std::filesystem::create_directories(directory);
    fl::StderrLogger logger;
    if (mode == "io") {
      BenchmarkIo(directory, bytes, runs, concurrency, transport->Payload(), logger);
      return 0;
    }

    fl::AzureBlobDownloader downloader(logger, transport);
    for (int run = 0; run < runs; ++run) {
      const auto file = directory / ("download-benchmark-" + std::to_string(run) + ".bin");
      if (std::filesystem::exists(file) ||
          std::filesystem::exists(fl::BlobDownloadState::GetStateFilePath(file)) ||
          std::filesystem::exists(fl::BlobDownloadState::GetVerifiedStateFilePath(file))) {
        throw std::runtime_error("Benchmark output already exists; use a fresh output directory");
      }

      std::atomic<bool> reached_end{false};
      Clock::time_point transferred;
      const auto start = Clock::now();
      downloader.DownloadBlob(sas, blob, file.string(), concurrency, [&](int64_t written) {
        if (written == bytes && !reached_end.exchange(true)) {
          transferred = Clock::now();
        }
      });
      const auto end = Clock::now();
      if (!reached_end || std::filesystem::file_size(file) != static_cast<uintmax_t>(bytes) ||
          !std::filesystem::exists(fl::BlobDownloadState::GetVerifiedStateFilePath(file))) {
        throw std::runtime_error("Download did not complete the requested benchmark size and verification");
      }

      const double total = Seconds(start, end);
      std::printf(
          "{\"mode\":\"%s\",\"run\":%d,\"bytes\":%lld,\"concurrency\":%d,\"transfer_s\":%.6f,"
          "\"finalize_s\":%.6f,\"total_s\":%.6f,\"mib_per_s\":%.3f,\"range_requests\":%lld}\n",
          mode.c_str(), run, static_cast<long long>(bytes), concurrency, Seconds(start, transferred),
          Seconds(transferred, end), total, static_cast<double>(mib) / total,
          static_cast<long long>(transport ? transport->get_count.exchange(0) : -1));
      std::fflush(stdout);
      std::filesystem::remove(file);
      std::filesystem::remove(fl::BlobDownloadState::GetVerifiedStateFilePath(file));
    }

    return 0;
  } catch (const std::exception& error) {
    // Azure exceptions can embed SAS URLs; never print them from a benchmark using credentials.
    if (fl::Utils::GetEnv("FOUNDRY_BENCHMARK_SAS")) {
      std::fprintf(stderr, "Download benchmark failed; details withheld because a SAS credential is configured\n");
    } else {
      std::fprintf(stderr, "Download benchmark failed: %s\n", error.what());
    }
    return 1;
  }
}
