// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Example: Interactive exploration of multiple separately addressable catalogs.
//
// This is a small REPL. You register named catalogs, enumerate them, select one,
// and query its models — demonstrating that each catalog is addressed and served
// independently (no aggregation across catalogs).
//
// Catalogs are configured up front (before the Manager is created), so the REPL
// rebuilds the Manager whenever you add a catalog. The `models` command performs
// a live network query against the selected catalog's URL.

#include <foundry_local/foundry_local_cpp.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace foundry_local;

namespace {

// A registered catalog source.
struct Source {
  std::string name;
  std::string url;
};

void PrintHelp() {
  std::cout <<
      "\nCommands:\n"
      "  list                     List registered catalog names\n"
      "  add <name> <url>         Register a named catalog (rebuilds the manager)\n"
      "  use <name>               Select a catalog as the current one\n"
      "  name                     Show the current catalog's reported name\n"
      "  models                   List models from the current catalog (live query)\n"
      "  help                     Show this help\n"
      "  quit                     Exit\n\n";
}

// Build a Manager from the registered sources. With no sources, the built-in
// "public" catalog is the default.
std::unique_ptr<Manager> BuildManager(const std::vector<Source>& sources) {
  Configuration config("catalog_repl");
  for (const auto& s : sources) {
    config.AddCatalog(s.name, s.url);
  }
  return std::make_unique<Manager>(std::move(config));
}

void ListCatalogs(const Manager& manager) {
  std::vector<std::string> names = manager.ListCatalogNames();
  std::cout << "Registered catalogs (" << names.size() << "):\n";
  for (const auto& name : names) {
    std::cout << "  - " << name << "\n";
  }
}

void ListModels(Manager& manager, const std::string& current) {
  try {
    ICatalog& catalog = current.empty() ? manager.GetCatalog() : manager.GetCatalog(current);
    std::cout << "Querying '" << (current.empty() ? std::string("<default>") : current)
              << "'...\n";
    ModelList models = catalog.GetModels();
    std::cout << "Models (" << models.size() << "):\n";
    for (const auto& model : models.Models()) {
      ModelInfo info = model->GetInfo();
      std::cout << "  - " << info.Alias() << "  (" << info.Id() << ")\n";
    }
  } catch (const Error& ex) {
    std::cout << "Query failed: " << ex.what() << "\n";
  }
}

}  // namespace

int main() {
  std::vector<Source> sources;
  std::unique_ptr<Manager> manager = BuildManager(sources);
  std::string current;  // empty = default catalog

  std::cout << "Interactive catalog demo. Type 'help' for commands.\n";
  ListCatalogs(*manager);

  std::string line;
  while (true) {
    std::cout << "\n[" << (current.empty() ? "default" : current) << "] > " << std::flush;
    if (!std::getline(std::cin, line)) {
      break;  // EOF
    }

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd.empty()) {
      continue;
    } else if (cmd == "quit" || cmd == "exit") {
      break;
    } else if (cmd == "help") {
      PrintHelp();
    } else if (cmd == "list") {
      ListCatalogs(*manager);
    } else if (cmd == "add") {
      std::string name, url;
      iss >> name >> url;
      if (name.empty() || url.empty()) {
        std::cout << "Usage: add <name> <url>\n";
        continue;
      }
      try {
        sources.push_back({name, url});
        manager.reset();  // Manager is a singleton — destroy the old one first.
        manager = BuildManager(sources);
        current.clear();
        std::cout << "Added '" << name << "'. Manager rebuilt.\n";
        ListCatalogs(*manager);
      } catch (const Error& ex) {
        sources.pop_back();
        manager.reset();
        manager = BuildManager(sources);
        std::cout << "Failed to add catalog: " << ex.what() << "\n";
      }
    } else if (cmd == "use") {
      std::string name;
      iss >> name;
      if (name.empty()) {
        std::cout << "Usage: use <name>\n";
        continue;
      }
      try {
        (void)manager->GetCatalog(name);  // validate the name exists
        current = name;
        std::cout << "Current catalog: " << current << "\n";
      } catch (const Error& ex) {
        std::cout << "Unknown catalog: " << ex.what() << "\n";
      }
    } else if (cmd == "name") {
      ICatalog& catalog = current.empty() ? manager->GetCatalog() : manager->GetCatalog(current);
      std::cout << "Reported name: " << catalog.GetName() << "\n";
    } else if (cmd == "models") {
      ListModels(*manager, current);
    } else {
      std::cout << "Unknown command: " << cmd << " (type 'help')\n";
    }
  }

  std::cout << "Bye.\n";
  return 0;
}
