// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

use std::io::{self, Read};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

struct HttpRequest {
    method: String,
    url: String,
}

struct HttpResponseHead {
    status_code: u16,
    headers: Vec<(String, Vec<u8>)>,
}

trait ModelHttpTransport: Send + Sync + 'static {
    fn create_operation(&self, request: HttpRequest) -> io::Result<Box<dyn ModelHttpOperation>>;
}

trait ModelHttpOperation: Send + Sync + 'static {
    fn send(&self) -> io::Result<HttpResponseHead>;
    fn read(&self, buffer: &mut [u8]) -> io::Result<usize>;
    fn cancel(&self);
}

// This prototype implements the proposed traits, not an existing SDK registration API.
struct MemoryTransport;

struct MemoryOperation {
    canceled: AtomicBool,
    state: Mutex<(bool, io::Cursor<Vec<u8>>)>,
}

impl ModelHttpTransport for MemoryTransport {
    fn create_operation(&self, request: HttpRequest) -> io::Result<Box<dyn ModelHttpOperation>> {
        if request.method != "GET" || request.url != "https://example.invalid/model" {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "unsupported request",
            ));
        }

        Ok(Box::new(MemoryOperation {
            canceled: AtomicBool::new(false),
            state: Mutex::new((false, io::Cursor::new(b"hello transport".to_vec()))),
        }))
    }
}

impl MemoryOperation {
    fn check_canceled(&self) -> io::Result<()> {
        if self.canceled.load(Ordering::Acquire) {
            return Err(io::Error::new(
                io::ErrorKind::Interrupted,
                "operation canceled",
            ));
        }

        Ok(())
    }
}

impl ModelHttpOperation for MemoryOperation {
    fn send(&self) -> io::Result<HttpResponseHead> {
        self.check_canceled()?;
        let mut state = self
            .state
            .lock()
            .map_err(|_| io::Error::other("operation lock poisoned"))?;
        if state.0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "request already sent",
            ));
        }

        state.0 = true;
        Ok(HttpResponseHead {
            status_code: 200,
            headers: vec![
                ("x-example".into(), b"first".to_vec()),
                ("x-example".into(), b"second".to_vec()),
            ],
        })
    }

    fn read(&self, buffer: &mut [u8]) -> io::Result<usize> {
        self.check_canceled()?;
        let mut state = self
            .state
            .lock()
            .map_err(|_| io::Error::other("operation lock poisoned"))?;
        if !state.0 || buffer.is_empty() {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid read"));
        }

        state.1.read(buffer)
    }

    fn cancel(&self) {
        // A network implementation must also interrupt its socket or cancel its pending future.
        self.canceled.store(true, Ordering::Release);
    }
}

fn request() -> HttpRequest {
    HttpRequest {
        method: "GET".into(),
        url: "https://example.invalid/model".into(),
    }
}

fn main() -> io::Result<()> {
    let transport: Arc<dyn ModelHttpTransport> = Arc::new(MemoryTransport);
    let operation = transport.create_operation(request())?;
    let head = operation.send()?;
    assert_eq!(head.status_code, 200);
    assert_eq!(head.headers.len(), 2);
    let mut output = Vec::new();
    let mut buffer = [0; 3];
    loop {
        let count = operation.read(&mut buffer)?;
        if count == 0 {
            break;
        }

        output.extend_from_slice(&buffer[..count]);
    }

    assert_eq!(output, b"hello transport");
    let canceled = transport.create_operation(request())?;
    canceled.cancel();
    assert_eq!(
        canceled.send().err().unwrap().kind(),
        io::ErrorKind::Interrupted
    );
    println!("Rust transport: streamed bytes, duplicate headers, and pre-send cancellation");
    Ok(())
}
