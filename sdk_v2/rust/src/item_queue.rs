//! The [`ItemQueue`] handle: a thread-safe stream of [`Item`]s.
//!
//! Unlike [`Item`]/[`Request`](crate::Request)/[`Response`](crate::Response),
//! `ItemQueue` is *handle-backed*: it wraps a native `flItemQueue` and shares it
//! across clones (an `Arc`), because the queue has a lifetime and identity that
//! must be preserved. Create one from a [`Session`](crate::Session) via
//! [`Session::create_input_queue`](crate::Session::create_input_queue) and attach
//! it to a [`Request`](crate::Request) for incremental / streaming input.

use std::sync::Arc;

use crate::detail::session::NativeItemQueue;
use crate::error::Result;
use crate::item::Item;

/// A thread-safe, multi-producer / multi-consumer queue of [`Item`]s.
///
/// Cloning an `ItemQueue` yields another handle to the *same* underlying native
/// queue, so items pushed through one clone are visible to all.
#[derive(Clone)]
pub struct ItemQueue {
    inner: Arc<NativeItemQueue>,
}

impl ItemQueue {
    pub(crate) fn from_native(inner: Arc<NativeItemQueue>) -> Self {
        Self { inner }
    }

    /// Borrow the underlying native queue (for request wiring within the crate).
    pub(crate) fn native(&self) -> &NativeItemQueue {
        &self.inner
    }

    /// Consume this handle, yielding the shared native queue.
    pub(crate) fn into_native(self) -> Arc<NativeItemQueue> {
        self.inner
    }

    /// Push an item onto the queue, transferring a native copy into it.
    pub fn push(&self, item: &Item) -> Result<()> {
        self.inner.push_value(item)
    }

    /// Pop the next available item, or `None` if the queue is currently empty.
    pub fn try_pop(&self) -> Option<Item> {
        self.inner.try_pop_value()
    }

    /// The number of items currently buffered.
    pub fn len(&self) -> usize {
        self.inner.size()
    }

    /// Whether the queue currently holds no items.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Signal that no more items will be pushed. A consumer draining the queue
    /// can then stop once it is empty.
    pub fn mark_finished(&self) {
        self.inner.mark_finished();
    }

    /// Whether [`mark_finished`](Self::mark_finished) has been called.
    pub fn is_finished(&self) -> bool {
        self.inner.is_finished()
    }
}

impl std::fmt::Debug for ItemQueue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ItemQueue")
            .field("len", &self.len())
            .field("finished", &self.is_finished())
            .finish()
    }
}
