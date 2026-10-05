// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fl {

/// Key-value pair storage. Accessible through the C API as flKeyValuePairs.
///
/// Keeps a sorted map for internal lookups and always-synced const char* vectors
/// for the C API's GetKeyValuePairs. Modeled after ORT's OrtKeyValuePairs pattern.
class KeyValuePairs {
 public:
  KeyValuePairs() = default;

  KeyValuePairs(const KeyValuePairs& other) {
    CopyFromMap(other.entries_);
  }

  KeyValuePairs(KeyValuePairs&& other) noexcept : KeyValuePairs{} {
    swap(*this, other);
  }

  KeyValuePairs& operator=(KeyValuePairs other) noexcept {
    swap(*this, other);
    return *this;
  }

  friend void swap(KeyValuePairs& a, KeyValuePairs& b) noexcept {
    using std::swap;
    swap(a.entries_, b.entries_);
    swap(a.keys_, b.keys_);
    swap(a.values_, b.values_);
  }

  /// Replace all entries from a map.
  void CopyFromMap(std::map<std::string, std::string> src) {
    entries_ = std::move(src);
    Sync();
  }

  /// Insert or update a single entry.
  void Add(std::string key, std::string value) {
    if (key.empty()) {
      return;
    }

    auto [it, inserted] = entries_.insert_or_assign(std::move(key), std::move(value));
    if (inserted) {
      const auto& [entry_key, entry_value] = *it;
      keys_.push_back(entry_key.c_str());
      values_.push_back(entry_value.c_str());
    } else {
      Sync();
    }
  }

  void Add(const char* key, const char* value) {
    if (key && value) {
      Add(std::string(key), std::string(value));
    }
  }

  /// Remove an entry by key.
  void Remove(const char* key) {
    if (!key) {
      return;
    }

    auto iter = entries_.find(key);
    if (iter != entries_.end()) {
      entries_.erase(iter);
      Sync();
    }
  }

  /// Lookup a value. Returns nullptr if not found.
  const char* Find(const char* key) const {
    if (!key) {
      return nullptr;
    }

    auto it = entries_.find(key);
    return (it != entries_.end()) ? it->second.c_str() : nullptr;
  }

  /// Map-style subscript — inserts default if key is missing.
  /// Marks C ABI vectors dirty; they are rebuilt lazily in Keys()/Values().
  std::string& operator[](const std::string& key) {
    dirty_ = true;
    return entries_[key];
  }

  std::string& operator[](std::string&& key) {
    dirty_ = true;
    return entries_[std::move(key)];
  }

  /// Map-style find — returns an iterator into the underlying map.
  auto find(const std::string& key) const { return entries_.find(key); }
  auto find(const std::string& key) { return entries_.find(key); }

  /// Check if a key exists.
  size_t count(const std::string& key) const { return entries_.count(key); }

  /// Read-only access to the underlying map.
  const std::map<std::string, std::string>& Entries() const { return entries_; }

  bool empty() const { return entries_.empty(); }
  size_t size() const { return entries_.size(); }

  // Range-for iteration support (delegates to the map).
  auto begin() const { return entries_.begin(); }
  auto end() const { return entries_.end(); }

  // C ABI accessors — rebuilt lazily when dirty.
  const std::vector<const char*>& Keys() const {
    SyncIfDirty();
    return keys_;
  }

  const std::vector<const char*>& Values() const {
    SyncIfDirty();
    return values_;
  }

 private:
  void Sync() {
    keys_.clear();
    values_.clear();
    for (const auto& [k, v] : entries_) {
      keys_.push_back(k.c_str());
      values_.push_back(v.empty() ? nullptr : v.c_str());
    }

    dirty_ = false;
  }

  void SyncIfDirty() const {
    if (dirty_) {
      const_cast<KeyValuePairs*>(this)->Sync();
    }
  }

  std::map<std::string, std::string> entries_;
  mutable std::vector<const char*> keys_;
  mutable std::vector<const char*> values_;
  mutable bool dirty_ = false;
};

/// Return base options with request options overlaid; request values win on collision.
inline KeyValuePairs MergeKeyValuePairs(const KeyValuePairs& base, const KeyValuePairs& request) {
  if (request.empty()) {
    return base;
  }

  auto merged = base;
  for (const auto& [key, value] : request) {
    merged.Add(key, value);
  }

  return merged;
}

}  // namespace fl
