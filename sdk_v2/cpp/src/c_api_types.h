// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

/// @file c_api_types.h
/// Opaque handle types for the C ABI. Each flXxx is an *opaque tag* — there
/// is no definition anywhere; only a forward declaration. The runtime object
/// behind a flXxx* is an instance of its HandleImpl type. Model, session and
/// preflight handles refer to API backend adapters, never directly to inference
/// implementation objects. Value/item handles still refer to local fl:: objects.
///
/// Use AsHandle / AsImpl to encode and decode the matching implementation
/// base pointer. An opaque handle is not an implementation subclass and must
/// never be downcast through an inheritance relationship that does not exist.

#include "api/backend.h"
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
struct flRequestPreflight;

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
  using type = fl::api::ModelBackend;
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
  using type = fl::api::SessionBackend;
};
template <>
struct HandleImpl<flRequestPreflight> {
  using type = fl::api::PreflightBackend;
};

}  // namespace fl::detail

// Convert an implementation pointer to its opaque handle. Always pick the matching
// handle type explicitly, e.g. AsHandle<flItem>(item_ptr).
template <typename Handle, typename Impl>
inline Handle* AsHandle(Impl* p) noexcept {
  static_assert(std::is_base_of_v<typename fl::detail::HandleImpl<Handle>::type, Impl>,
                "Impl must derive from the handle's underlying fl:: type");
  return reinterpret_cast<Handle*>(static_cast<typename fl::detail::HandleImpl<Handle>::type*>(p));
}

template <typename Handle, typename Impl>
inline const Handle* AsHandle(const Impl* p) noexcept {
  static_assert(std::is_base_of_v<typename fl::detail::HandleImpl<Handle>::type, Impl>,
                "Impl must derive from the handle's underlying fl:: type");
  return reinterpret_cast<const Handle*>(static_cast<const typename fl::detail::HandleImpl<Handle>::type*>(p));
}

// Convert an opaque handle to its internal type. Optionally specify a derived
// type for polymorphic hierarchies (Item -> TextItem).
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
