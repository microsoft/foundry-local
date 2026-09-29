// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Example: KEV typed decisions with Foundry Local Core.
//
// Usage:
//   kev_decision_example <package_path> [provider]
//
// Examples:
//   kev_decision_example /models/kev-4b-mixed-middle-mlp
//   kev_decision_example /models/kev-4b-mixed-middle-mlp cuda

#include "contracts/non_generative.h"
#include "inferencing/predictive/non_generative_runtime.h"

#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace {

void PrintUsage(const char* executable) {
  std::cerr << "Usage: " << executable << " <package_path> [provider]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 3) {
    PrintUsage(argv[0]);
    return 2;
  }

  const std::string package_path = argv[1];
  const std::string provider = argc == 3 ? argv[2] : "";

  try {
    fl::NonGenerativeRequest request;
    request.state = {
        {"weather", "heavy rain"},
        {"destination", "office"},
    };

    fl::NonGenerativeQuestion question;
    question.type = "noul";
    question.instructions = "Should I take an umbrella?";
    request.questions.emplace_back("umbrella", std::move(question));

    fl::DecisionRuntime runtime(package_path, provider);
    const auto answers = runtime.Decide(request);

    std::cout << "KEV answers:\n" << answers.dump(2) << '\n';
  } catch (const std::exception& error) {
    std::cerr << "KEV inference failed: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
