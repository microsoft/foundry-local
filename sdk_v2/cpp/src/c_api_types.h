// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

/// @file c_api_types.h
/// Opaque handle types for the C ABI. Each flXxx is an *opaque tag* — there
/// is no definition anywhere; only a forward declaration. The runtime object
/// behind a flXxx* is always an instance of the corresponding fl::Xxx (or one
/// of its subclasses for polymorphic hierarchies like fl::Item / fl::Session).
///
/// Use the AsHandle / AsImpl helpers below to convert between internal
/// fl::Xxx* and ABI flXxx* pointers. Do NOT try to static_cast between them —
/// the types are unrelated as far as the language is concerned, and any cast
/// through an inheritance relationship that does not actually exist at
/// runtime is undefined behavior. UBSan's vptr check catches it for the
/// polymorphic types (fl::Item / fl::Session) where the runtime object is
/// always a sibling subclass like fl::TextItem or fl::ChatSession.

#include "configuration.h"
#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "inferencing/session/session.h"
#include "items/item.h"
#include "items/item_queue.h"
#include "model.h"
#include "model_info.h"
#include "util/key_value_pairs.h"

#include <type_traits>

// Opaque handle types — forward declarations only. There is no definition
// anywhere; flXxx exists purely as a distinct pointer type for the C ABI.
// (The public header foundry_local_c.h forward-declares these too via
// FL_TYPE; redeclaring them here is harmless.)
struct flKeyValuePairs;
struct flConfiguration;
struct flModelInfo;
struct flModel;
struct flItem;
struct flItemQueue;
struct flRequest;
struct flResponse;
struct flSession;

namespace fl::detail {

// Map handle type -> implementation type.
template <typename Handle>
struct HandleImpl;

template <>
struct HandleImpl<flKeyValuePairs> {
  using type = fl::KeyValuePairs;
};
template <>
struct HandleImpl<flConfiguration> {
  using type = fl::Configuration;
};
template <>
struct HandleImpl<flModelInfo> {
  using type = fl::ModelInfo;
};
template <>
struct HandleImpl<flModel> {
  using type = fl::Model;
};
template <>
struct HandleImpl<flItem> {
  using type = fl::Item;
};
template <>
struct HandleImpl<flItemQueue> {
  using type = fl::ItemQueue;
};
template <>
struct HandleImpl<flRequest> {
  using type = fl::Request;
};
template <>
struct HandleImpl<flResponse> {
  using type = fl::Response;
};
template <>
struct HandleImpl<flSession> {
  using type = fl::Session;
};

}  // namespace fl::detail

namespace fl::detail {

inline constexpr bool IsRequestPreflightVersionSupported(
    uint32_t version,
    uint32_t current_api_version = FOUNDRY_LOCAL_API_VERSION) noexcept {
  return version >= FOUNDRY_LOCAL_REQUEST_PREFLIGHT_MIN_VERSION &&
         version <= current_api_version;
}

}  // namespace fl::detail

// Convert an internal fl::Xxx* to its opaque handle. Always pick the matching
// handle type explicitly, e.g. AsHandle<flItem>(item_ptr).
template <typename Handle, typename Impl>
inline Handle* AsHandle(Impl* p) noexcept {
  static_assert(std::is_base_of_v<typename fl::detail::HandleImpl<Handle>::type, Impl>,
                "Impl must derive from the handle's underlying fl:: type");
  return reinterpret_cast<Handle*>(p);
}

template <typename Handle, typename Impl>
inline const Handle* AsHandle(const Impl* p) noexcept {
  static_assert(std::is_base_of_v<typename fl::detail::HandleImpl<Handle>::type, Impl>,
                "Impl must derive from the handle's underlying fl:: type");
  return reinterpret_cast<const Handle*>(p);
}

// Convert an opaque handle to its internal type. Optionally specify a derived
// type for polymorphic hierarchies (Item -> TextItem, Session -> ChatSession).
// Without an explicit Impl the result is a pointer to the base fl:: type.
template <typename Impl = void, typename Handle>
inline auto AsImpl(Handle* h) noexcept {
  using Base = typename fl::detail::HandleImpl<Handle>::type;
  if constexpr (std::is_void_v<Impl>) {
    return reinterpret_cast<Base*>(h);
  } else {
    static_assert(std::is_base_of_v<Base, Impl>,
                  "Impl must derive from the handle's underlying fl:: type");
    // Two-step: handle->Base is the legitimate decoding; Base->Impl is a
    // genuine downcast that the caller must guarantee is correct (e.g.
    // because the item's discriminator was checked, or because the call
    // always returns this concrete type).
    return static_cast<Impl*>(reinterpret_cast<Base*>(h));
  }
}

template <typename Impl = void, typename Handle>
inline auto AsImpl(const Handle* h) noexcept {
  using Base = typename fl::detail::HandleImpl<Handle>::type;
  if constexpr (std::is_void_v<Impl>) {
    return reinterpret_cast<const Base*>(h);
  } else {
    static_assert(std::is_base_of_v<Base, Impl>,
                  "Impl must derive from the handle's underlying fl:: type");
    return static_cast<const Impl*>(reinterpret_cast<const Base*>(h));
  }
}
