# Custom model HTTP transport

The native example registers an application-owned transport through the public C++ configuration wrapper.
It supplies an empty catalog through short in-memory reads, then checks cancellation and ownership.
It does not download model weights or demonstrate resume behavior.
An optional first argument selects the sample data directory; the default stays beside the executable.
See the [contract and binding guide](../../docs/model-http-transport.md) for ownership and cancellation rules.

Build through the repository wrapper:

```sh
python sdk_v2/cpp/build.py --configure --build --config RelWithDebInfo --parallel 4
sdk_v2/cpp/build/Linux/RelWithDebInfo/bin/custom_transport_example
```

Use the corresponding platform directory on Windows or macOS.

## Binding prototypes

These standalone prototypes demonstrate the adapter shape.
They do **not** register a transport with the current Rust or JavaScript SDK.
They do not require a published SDK package or model files.

```sh
node sdk_v2/cpp/examples/custom_transport/bindings/node.mjs
rustc --edition 2021 sdk_v2/cpp/examples/custom_transport/bindings/rust.rs \
  -o sdk_v2/cpp/build/rust_transport_example
sdk_v2/cpp/build/rust_transport_example
```

The Node prototype uses its own `node:http` client against a loopback server.
It preserves duplicate headers and reads partial response chunks.
It also demonstrates HTTP errors, pre-send cancellation, and cancellation during blocked requests and reads.
It checks POST bodies and an operation deadline.
It does not use Foundry's default network implementation.

The Rust prototype implements the proposed traits with an in-memory response.
Its request type contains only the method and URL; the production type also needs headers, body bytes, and a timeout.
It demonstrates trait ownership, partial reads, duplicate headers, and pre-send cancellation.
A real network operation must interrupt blocked I/O; an atomic flag alone is insufficient.

The guide also describes C# and Python adapters, including their callback and buffer lifetimes.
Production adapters still need native bridge code, lifecycle tests, and runtime-shutdown behavior.
