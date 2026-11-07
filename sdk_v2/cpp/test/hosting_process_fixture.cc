// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/process_watch.h"

#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifndef _WIN32
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

int main(int argc, char* argv[]) {
  try {
#ifndef _WIN32
    if (argc == 4 && std::string_view(argv[1]) == "--bootstrap") {
      std::ifstream input(argv[2]);
      std::string command;
      input >> command;
      if (command == "stop") {
        std::ofstream(std::string(argv[2]) + ".pid") << getpid();
        raise(SIGSTOP);
        return 0;
      }
      return 2;
    }
#else
    (void)argc;
    (void)argv;
#endif
#ifdef __linux__
    // Exercise /proc stat parsing with a comm containing spaces and closing parentheses.
    if (prctl(PR_SET_NAME, "watch ) (test)", 0, 0, 0) != 0) {
      return 2;
    }
#endif
    const auto identity = fl::hosting::CurrentProcessIdentity();
    std::cout << identity.pid << ' ' << identity.generation << '\n'
              << std::flush;
    const auto command = std::cin.get();
#ifndef _WIN32
    if (command == 'f') {
      const auto child = fork();
      if (child < 0) {
        return 3;
      }
      if (child == 0) {
        const char ready = 'h';
        if (write(STDOUT_FILENO, &ready, 1) != 1) {
          _exit(4);
        }

        char value{};
        ssize_t size;
        do {
          size = read(STDIN_FILENO, &value, 1);
        } while (size < 0 && errno == EINTR);

        _exit(size == 0 ? 0 : 5);
      }
    }
#else
    (void)command;
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
