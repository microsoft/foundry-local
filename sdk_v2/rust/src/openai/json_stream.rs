//! Generic JSON-deserializing stream over an unbounded channel of raw strings.

use std::marker::PhantomData;
use std::pin::Pin;
use std::task::{Context, Poll};

use serde::de::DeserializeOwned;

use crate::error::{FoundryLocalError, Result};

/// A stream that deserializes each received string chunk into `T`.
///
/// Empty chunks are silently skipped.
pub struct JsonStream<T> {
    rx: tokio::sync::mpsc::UnboundedReceiver<Result<String>>,
    _marker: PhantomData<T>,
}

impl<T> JsonStream<T> {
    pub(crate) fn new(rx: tokio::sync::mpsc::UnboundedReceiver<Result<String>>) -> Self {
        Self {
            rx,
            _marker: PhantomData,
        }
    }
}

impl<T> Unpin for JsonStream<T> {}

impl<T: DeserializeOwned> futures_core::Stream for JsonStream<T> {
    type Item = Result<T>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        loop {
            match self.rx.poll_recv(cx) {
                Poll::Ready(Some(Ok(chunk))) => {
                    if chunk.is_empty() {
                        continue;
                    }
                    let parsed = serde_json::from_str::<T>(&chunk).map_err(FoundryLocalError::from);
                    return Poll::Ready(Some(parsed));
                }
                Poll::Ready(Some(Err(e))) => return Poll::Ready(Some(Err(e))),
                Poll::Ready(None) => return Poll::Ready(None),
                Poll::Pending => return Poll::Pending,
            }
        }
    }
}
