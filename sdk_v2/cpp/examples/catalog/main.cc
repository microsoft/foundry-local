// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Example: Multiple separately addressable catalogs.
// Demonstrates registering several named catalogs, enumerating them, resolving
// each one by name, the default-catalog behavior, and the error raised for an
// unknown name. This exercises the catalog-addressing surface without any
// network access or model download.

#include <foundry_local/foundry_local_cpp.h>

#include <iostream>
#include <string>

using namespace foundry_local;

namespace {

// Print the names of every catalog registered on the manager, in add-order.
void PrintCatalogNames(const Manager& manager) {
  std::vector<std::string> names = manager.ListCatalogNames();
  std::cout << "Registered catalogs (" << names.size() << "):\n";
  for (const auto& name : names) {
    std::cout << "  - " << name << "\n";
  }
}

// Demonstrate the default catalog when no source is configured: the built-in
// Azure Foundry catalog is registered under the reserved name "public".
void DefaultCatalog() {
  std::cout << "\n--- Default catalog (no sources added) ---\n";
  Manager manager(Configuration("catalog_demo_default"));

  PrintCatalogNames(manager);

  // The no-argument GetCatalog() returns the default (first-registered) catalog.
  ICatalog& def = manager.GetCatalog();
  std::cout << "Default catalog resolved: " << (&def ? "yes" : "no") << "\n";
}

// Demonstrate several named catalogs addressed individually.
void NamedCatalogs() {
  std::cout << "\n--- Named catalogs ---\n";

  Configuration config("catalog_demo_named");
  config.AddCatalog("first", "https://example.com/first")
      .AddCatalog("second", "https://example.com/second");
  Manager manager(std::move(config));

  PrintCatalogNames(manager);

  // Resolve each catalog by name. Repeated lookups return the same object.
  ICatalog& first = manager.GetCatalog("first");
  ICatalog& second = manager.GetCatalog("second");
  std::cout << "first and second are distinct: " << (&first != &second ? "yes" : "no") << "\n";

  // The no-argument GetCatalog() returns the default catalog, which corresponds to
  // the first registered name.
  ICatalog& def = manager.GetCatalog();
  std::cout << "Default catalog resolved: " << (&def ? "yes" : "no") << "\n";
  std::cout << "Default corresponds to first registered name: "
            << (manager.ListCatalogNames().front() == "first" ? "yes" : "no") << "\n";

  // Repeated lookups of the same name return the same cached object.
  ICatalog& first_again = manager.GetCatalog("first");
  std::cout << "Repeated GetCatalog(\"first\") is cached: "
            << (&first == &first_again ? "yes" : "no") << "\n";

  // An unknown name raises an Error.
  try {
    manager.GetCatalog("does-not-exist");
    std::cout << "ERROR: expected an exception for unknown catalog name\n";
  } catch (const Error& ex) {
    std::cout << "Unknown name correctly rejected: " << ex.what() << "\n";
  }
}

}  // namespace

int main() {
  try {
    DefaultCatalog();
    NamedCatalogs();
    std::cout << "\nDone.\n";
  } catch (const Error& ex) {
    std::cerr << "Unexpected error: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
