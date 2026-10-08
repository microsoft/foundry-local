# Inference with the Foundry Local SDK (v2)

The Foundry Local SDK runs inference in your application, using a locally loaded model.
Whether you are building a chat assistant, generating embeddings, or transcribing audio,
the same small set of concepts describes the work:

```text
Model
  |
  +-- Session A: its own state and settings
  |     Request [input items] --> inference --> Response [output items]
  |                                |
  |                                +--> streamed output items
  |
  +-- Session B: independent state and settings
        Request [input items] --> inference --> Response [output items]
```

This guide focuses on inference, not model discovery or downloading. Examples assume
you have initialized the SDK and have an appropriate **already loaded** model. Keep its
manager alive throughout inference. See your language's README for setup.

## The building blocks

| Component | What it does | When to use it |
|-----------|--------------|----------------|
| **Model** | Represents the model used to perform inference. Loading makes its runtime resources available. | Load once, then use it with one or more sessions. |
| **Session** | Connects requests to a model and manages inference state and session settings. | Keep one chat session per conversation; use the session type that matches the model's task. |
| **Request** | Describes one invocation: input items and optional settings for that invocation. | Create a request for a new user message, a batch of text to embed, or audio to transcribe. |
| **Response** | Contains the invocation's output items, finish reason, and usage information. | Read the complete result after inference finishes, even when you also consume streamed output. |
| **Item** | A typed unit of input or output: a message, text, audio, an image, a tensor, a tool call, and so on. | Inspect its type before reading its content. A request or response can contain multiple items. |
| **ItemQueue** | An ordered, thread-safe queue of items shared between a producer and a consumer. | Carries native streaming output, and supplies incremental input such as live audio. |

### A session owns state, not the conversation's model

A loaded model can serve multiple sessions. Their conversation histories are separate:
two sessions using the same model do **not** share chat messages.

For chat, keep the session and send only the **new input** on each turn:

```text
Same ChatSession:
  Request 1: "What is the capital of France?"
  Response 1: "Paris."
  Request 2: "And what country is it in?"
  Response 2: uses the previous question and answer as context
```

Do not resend the whole conversation on every request to a stateful `ChatSession`.
Create a new session to start an independent conversation. Chat sessions also expose
turn-count and undo-turn operations for applications that need to rewind.

State management depends on the task: chat retains conversation history, audio manages
an active transcription, and embeddings are stateless. For predictable conversation
ordering, complete one chat request before starting the next on the same session.
Use separate sessions for independent conversations; sharing a model does not guarantee
that all requests will execute simultaneously.

### Items describe the data, not just strings

Choose a session and input items that the model supports:

| Session | Typical request items | Complete response items | Streamed output |
|---------|-----------------------|-------------------------|-----------------|
| `ChatSession` | Messages; tool results when using tools | Assistant messages and/or tool calls | Text items; complete tool-call items when using tools |
| `EmbeddingsSession` | Text items, one per input | Tensor items containing embedding vectors, one per input | Use the complete response, not token streaming |
| `AudioSession` | An audio item, or an audio format descriptor followed by an input queue to provide streamed audio chunks. | A speech-result item with the transcript and segment details | Speech-segment items |

### Current item types

These are the [native item type names](cpp/include/foundry_local/foundry_local_c.h);
class and factory names vary by language. Input, output, and message-part usage are
distinguished below.

| Item type | Brief description | Valid session usage |
|-----------|-------------------|---------------------|
| `UNKNOWN` | Unspecified type sentinel, not a usable data item. | None. |
| `BYTES` | Raw binary data. | `AudioSession`: PCM input chunks inside an input queue, not top-level request items. |
| `TENSOR` | A typed, multidimensional array. | `EmbeddingsSession`: output embedding vectors. |
| `TEXT` | Ordinary text or model-generated reasoning text. | `EmbeddingsSession`: input text; `ChatSession`: message parts and streamed text. |
| `MESSAGE` | A role and typed content parts. | `ChatSession`: input messages and complete assistant-message output. |
| `IMAGE` | Image bytes or a URI. | `ChatSession`: input message part, with a model that supports images. |
| `AUDIO` | Audio bytes, a URI, or a streaming format descriptor. | `AudioSession`: input; `ChatSession`: input message part, with a model that supports audio. |
| `SPEECH_SEGMENT` | Recognized speech text with optional timing and word details. | `AudioSession`: streamed output and segments within the final speech result; output-only. |
| `SPEECH_RESULT` | The complete transcript and its segment details. | `AudioSession`: complete response output; output-only. |
| `TOOL_CALL` | A tool name, call ID, and arguments. | `ChatSession`: complete or streamed output; input when supplying prior assistant tool calls. |
| `TOOL_RESULT` | A tool's result, associated with a call ID. | `ChatSession`: input after your application executes a tool call. |
| `QUEUE` | An ordered queue of items. | `AudioSession`: streamed input alongside an audio descriptor; native output delivery for `ChatSession` and `AudioSession` callbacks. |

`DEFAULT` and `REASONING` are `TEXT` subtypes, not separate item types.

> **Note:** `TEXT` also has an `OPENAI_JSON` subtype used by OpenAI compatibility
> adapters, including the web service and in-process OpenAI-style clients. It carries
> OpenAI-shaped request/response JSON for chat, embeddings, and audio transcription.
> It is not needed for normal typed session usage.

A **message** is itself an item. It has a role (such as system, user, or assistant)
and contains typed parts. A plain-text user message has one text part; a multimodal
message might contain text and an image. Having an image or audio item type in the
API does not mean every chat model accepts it.

A **tool call** is a request from the model to your application, not an automatically
executed function. Your application runs the tool and sends a tool-result item, with
the matching call ID, in the next request. See the [tool-calling example](cpp/examples/tool_calling/main.cc).

## Run a request, then stream a follow-up

The examples below perform the same two-turn conversation. The first request returns
a complete response; the second streams incremental output on the **same session**.
They are inference fragments, not standalone programs: `model` is your loaded chat
model, and async fragments run in an async context.

The typed inference API is available in C++, C#, Python, JavaScript/TypeScript, and
Rust. The [Java preview](#java-preview-stream-audio) currently exposes a specialized
audio API instead of these generic request/item types.

### C++

`ProcessRequest` blocks until completion. Register a callback to receive each streamed
item while it runs. The current C++ callback receives an **owning `Item`** directly;
you do not need to pop the native output queue yourself.

```cpp
#include <foundry_local/foundry_local_cpp.h>
#include <iostream>

using namespace foundry_local;

void Chat(IModel& model) {
  ChatSession session(model);
  RequestOptions options;
  options.search.max_output_tokens = 128;
  session.SetOptions(options);

  Request first{UserMessage("What is the capital of France?")};
  Response response = session.ProcessRequest(first);
  for (const Item& item : response.GetItems()) {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      for (const Item& part : item.GetMessage().parts) {
        if (part.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
          std::cout << part.GetText().text;
        }
      }
    }
  }
  std::cout << "\n";

  session.SetStreamingCallback([](Item item) -> int {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
      std::cout << item.GetText().text << std::flush;
    }
    return 0;
  });

  Request follow_up{UserMessage("And what country is it in?")};
  Response final = session.ProcessRequest(follow_up);
  std::cout << "\nTotal tokens: " << final.GetUsage().total_tokens << "\n";
}
```

Return `0` from the callback to continue, or a nonzero value to cancel. It runs on a
native worker thread: keep it short, and marshal UI work to your UI thread. The item
releases its resources when the callback returns unless you move it into another owner.
Clear the callback with `session.SetStreamingCallback(nullptr)` to stop receiving
incremental items on subsequent requests.

Full example: [basic chat](cpp/examples/basic_chat/main.cc).

### C#

The binding adapts native callbacks into an async item stream. Enable streaming before
using `ProcessStreamingRequestAsync`, and use `await using` for the stream's lifetime.

```csharp
using System;
using System.Threading.Tasks;
using Microsoft.AI.Foundry.Local;

async Task ChatAsync(IModel model)
{
    using var session = new ChatSession(model);
    using var first = new Request();
    first.AddItem(MessageItem.User("What is the capital of France?"));

    using (var response = await session.ProcessRequestAsync(first))
    {
        foreach (var item in response)
        {
            if (item is MessageItem message)
            {
                foreach (var part in message.Parts)
                {
                    if (part is TextItem text)
                        Console.Write(text.Text);
                }
            }
        }
        Console.WriteLine();
    }

    session.SetStreaming(true);
    using var followUp = new Request();
    followUp.AddItem(MessageItem.User("And what country is it in?"));

    await using var stream = session.ProcessStreamingRequestAsync(followUp);
    await foreach (var item in stream)
    {
        using (item)
        {
            if (item is TextItem text)
                Console.Write(text.Text);
        }
    }
    using var final = await stream.FinalResponse;
    Console.WriteLine($"\nFinished: {final.FinishReason}");
}
```

Items yielded by the stream are owned by the caller and should be disposed. Items
enumerated from a complete response are borrowed views: read them while the response
is alive, and do not dispose them separately.

Full non-streaming example: [basic chat](cs/examples/BasicChat/Program.cs).

### Python

Python exposes a synchronous iterator over streamed items, with inference running on
a background worker. Use context managers for requests, responses, and streams.

```python
from foundry_local_sdk import ChatSession, MessageItem, Request, TextItem

def chat(model):
    with ChatSession(model) as session:
        with Request().add_item(
            MessageItem.user("What is the capital of France?")
        ) as first:
            with session.process_request(first) as response:
                for item in response:
                    if isinstance(item, MessageItem):
                        for part in item.parts:
                            if isinstance(part, TextItem):
                                print(part.text, end="")
                print()

        session.set_streaming(True)
        with Request().add_item(
            MessageItem.user("And what country is it in?")
        ) as follow_up:
            with session.process_streaming_request(follow_up) as stream:
                for item in stream:
                    with item:
                        if isinstance(item, TextItem):
                            print(item.text, end="", flush=True)
                with stream.final_response as final:
                    print(f"\nFinished: {final.finish_reason}")
```

`final_response` is available after the stream is fully consumed. Read response items
inside the response's `with` block; streamed items have their own lifetime.

Full example: [chat completion](python/examples/chat_completion.py).

### JavaScript / TypeScript

The ESM package exposes streaming as an `AsyncIterable`. There is no separate
streaming-enable step. Items and responses are plain data objects; dispose the session
when you are done.

```javascript
import { ChatSession, Item, Request } from "foundry-local-sdk";

async function chat(model) {
  const session = new ChatSession(model);
  try {
    const first = new Request()
      .addItem(Item.userMessage("What is the capital of France?"));
    const response = await session.processRequest(first);
    for (const item of response.output) {
      if (item.type === "message") {
        if (item.parts !== undefined) {
          for (const part of item.parts) {
            if (part.type === "text") process.stdout.write(part.text);
          }
        } else if (typeof item.content === "string") {
          process.stdout.write(item.content);
        }
      }
    }
    process.stdout.write("\n");

    const followUp = new Request()
      .addItem(Item.userMessage("And what country is it in?"));
    const stream = session.processStreamingRequest(followUp);
    for await (const item of stream) {
      if (item.type === "text") process.stdout.write(item.text);
    }
    const final = await stream.response;
    console.log(`\nFinished: ${final.finishReason}`);
  } finally {
    session.dispose();
  }
}
```

Full non-streaming example: [basic chat](js/examples/basic_chat.mjs).

### Rust

Use the typed `ChatSession` with Tokio, and `tokio_stream::StreamExt` to consume
incremental items. Requests are passed by value; responses contain owned data.

```rust
use foundry_local_sdk::{ChatSession, Item, Model, Request};
use std::io::{self, Write};
use tokio_stream::StreamExt;

async fn chat(model: &Model) -> Result<(), Box<dyn std::error::Error>> {
    let session = ChatSession::new(model).await?;
    let first = Request::from_items(vec![Item::user_message(vec![
        Item::text("What is the capital of France?"),
    ])]);
    let response = session.process_request(first).await?;
    println!("{}", response.text());

    let follow_up = Request::from_items(vec![Item::user_message(vec![
        Item::text("And what country is it in?"),
    ])]);
    let mut stream = session.process_streaming_request(follow_up);
    while let Some(item) = stream.next().await {
        let item = item?;
        if let Some(text) = item.as_text() {
            print!("{text}");
            io::stdout().flush()?;
        }
    }
    let final_response = stream.response().await?;
    println!("\nFinished: {:?}", final_response.finish_reason);
    Ok(())
}
```

Rust handles release their resources on drop. Cloning a session shares its existing
state; it does not create a new conversation. Use `ChatSession::new` for that.

Full example: [chat completion](rust/examples/chat_completion.rs).

### The same pattern for embeddings

For embeddings, change the session and item types, not the overall flow. This C++
fragment assumes `embedding_model` is a loaded embeddings model:

```cpp
EmbeddingsSession session(embedding_model);
Request request{Item::Text("The cat sat on the mat."), Item::Text("A kitten rested on the rug.")};
Response response = session.ProcessRequest(request);

for (const Item& item : response.GetItems()) {
  if (item.GetType() == FOUNDRY_LOCAL_ITEM_TENSOR) {
    TensorContent tensor = item.GetTensor();
    std::cout << "Embedding dimensions: " << tensor.shape.at(0) << "\n";
  }
}
```

Each output tensor holds one input's embedding vector. C++ also offers `Embed` helpers
that return vectors directly; see the [embeddings example](cpp/examples/embeddings/main.cc).

## Streaming output and the final response

Streaming changes **when you receive output**, not the request/response model:

1. Submit one request.
2. Consume typed items as inference produces them.
3. Read the final response after completion for aggregated output, finish reason, and
   usage information where applicable.

Chat text items are incremental pieces, not complete assistant messages. Do not append
the final message to those pieces again, or you will display the answer twice.
Tool-enabled chat can stream tool-call items too; audio streams speech-segment items
rather than chat text items.

Typed audio streaming returns `SPEECH_SEGMENT` items, and the complete response contains
a `SPEECH_RESULT` item. Each streamed segment carries decoded text in its `text` field;
it is not a `TEXT` item. The current implementation sets the segment's `kind` to `NONE`
because the models emit incremental decoded text without partial/final utterance
boundaries. Java maps this segment kind to `SpeechEvent.Kind.TOKEN`; it still consumes
native `SPEECH_SEGMENT` items.

If a model/runtime provides `Partial` speech hypotheses, their text is cumulative for
the current segment: replace that hypothesis rather than append it. Use the final
speech result as the complete transcript.

At the C API level, each callback receives an event with an **output `ItemQueue`**;
one item is added per callback. The consumer pops it and owns the popped item.
The C++ item callback and the other bindings' iterators handle this queue plumbing for
you. You normally create your own queue only for **streamed input**.

For cancellation, C++ callbacks can return nonzero, C# accepts a `CancellationToken`,
and JavaScript accepts `{ signal: abortController.signal }`. Leaving a Python streaming
context early cancels unfinished work; dropping a Rust item stream cancels generation.
Cancellation can race with completion, so handle the binding's cancellation/error
result rather than assuming a final successful response will always exist.

## Stream input with an ItemQueue

Some inputs are not available all at once. For example, microphone audio arrives in
chunks while transcription is running:

```text
audio capture --> push byte items --> input ItemQueue --> AudioSession
                                                           |
                                     speech items <--------+
                                     final Response <------+
```

For a streaming-capable audio model, supply a request containing:

1. An audio format descriptor.
2. An input queue that your producer fills with byte items.

Call **`MarkFinished` / `mark_finished` / `markFinished`** when no more input will arrive.
An empty queue means "nothing available yet"; a finished, drained queue means "end of
input." Without the finish signal, inference may keep waiting.

This Python example assumes `audio_model` is a loaded streaming-ASR model and
`pcm_chunks` yields signed PCM16 little-endian audio at 16 kHz, mono. The streaming
request starts a background inference worker before the producer begins pushing.

```python
from foundry_local_sdk import (
    AudioItem, AudioSession, BytesItem, ItemQueue, Request,
    SpeechResultItem, SpeechSegmentItem,
)

with AudioSession(audio_model) as session, ItemQueue() as input_queue:
    session.set_streaming(True)
    with Request() as request:
        request.add_item(AudioItem.create_format_descriptor("pcm", 16000, 1))
        request.add_item(input_queue, transfer_ownership=False)

        with session.process_streaming_request(request) as stream:
            try:
                for chunk in pcm_chunks:
                    input_queue.push(BytesItem(chunk))
            finally:
                input_queue.mark_finished()

            for item in stream:
                with item:
                    if isinstance(item, SpeechSegmentItem):
                        print(item.text, end="", flush=True)
            with stream.final_response as response:
                for item in response:
                    if isinstance(item, SpeechResultItem):
                        print(f"\nTranscript: {item.text}")
```

Here the application reads the buffered output after feeding input. For live display
while capture continues, consume output concurrently with the input producer, and
coordinate their errors and cancellation. The [C++ real-time audio example](cpp/examples/realtime_audio/main.cc)
shows a producer thread and a streaming output callback.

Queue operations have equivalent names across the typed bindings:

| Language | Create and attach an input queue | Add data / signal end of input |
|----------|----------------------------------|--------------------------------|
| C++ | `ItemQueue queue; request.AddItem(queue, false);` | `queue.Push(std::move(item)); queue.MarkFinished();` |
| C# | `using var queue = new ItemQueue(); request.AddItem(queue, takeOwnership: false);` | `queue.Push(item); queue.MarkFinished();` |
| Python | `queue = ItemQueue(); request.add_item(queue, transfer_ownership=False)` | `queue.push(item); queue.mark_finished()` |
| JS/TS | `const queue = new ItemQueue(); request.addItem(queue);` | `queue.push(item); queue.markFinished();` |
| Rust | `let queue = session.create_input_queue()?;` then `request.with_input_queue(queue.clone())` | `queue.push(&item)?; queue.mark_finished();` |

Keep the queue alive until inference and the producer have stopped. In C++, C#, and
Python, pushing transfers the item's native ownership to the queue. JavaScript buffers
are pinned, not copied: do not modify queued buffers while native inference can read
them. Rust pushes a native copy of its item value.

### Java preview: stream audio

Java's current preview supports streaming ASR through `AudioSession` and
`Transcription`. It does **not** yet expose generic `Request`, `Response`, or `ItemQueue`
classes. `writePcm` feeds the underlying input queue, the listener receives
`SpeechEvent`s, and `await` returns the complete `TranscriptionResult`.

This fragment assumes a loaded streaming-ASR `model` and an `Iterable<byte[]>`
named `pcmChunks` containing PCM16LE, 16 kHz, mono audio, with at most one second of
audio per chunk:

```java
import com.microsoft.foundry.local.PcmFormat;
import com.microsoft.foundry.local.SpeechEvent;

try (var session = model.createAudioSession();
     var transcription = session.streamPcm(PcmFormat.SPEECH, event -> {
         if (event.kind() == SpeechEvent.Kind.TOKEN) {
             System.out.print(event.text());
         }
     })) {
    for (byte[] chunk : pcmChunks) {
        transcription.writePcm(chunk);
    }
    transcription.finishInput();
    var result = transcription.await();
    System.out.println("\nTranscript: " + result.text());
}
```

Listeners run on native callback threads; do not call lifecycle or input methods from
them. Close each transcription before starting the next on the same audio session.
See the [Java guide](java/README.md) for preview limitations and input backpressure.

## Settings and cleanup

- **Session settings are defaults.** Apply options such as output-token limits to the
  session when they should apply to every request. Per-request settings override the
  corresponding defaults for that invocation; they do not change subsequent requests.
  The C++ example above sets a session-wide limit.
- **Read the finish reason.** A response can end because of a normal stop, an output
  length limit, or a tool call. Do not assume all returned output is a complete answer.
  Catch exceptions or handle `Result` errors at your application's boundary.
- **Finish or cancel active work before cleanup.** Keep requests, input queues, and any
  borrowed input storage valid for as long as inference uses them. Keep callbacks short;
  do not start another request or dispose the session from a native callback.
- **Close sessions before unloading the model.** Reuse the loaded model for further
  sessions if needed, then unload it when it is no longer needed. Release the manager
  last. C++ uses RAII, C# uses `using`, Python uses `with`, JS/TS uses `dispose`, Java
  uses try-with-resources, and Rust uses drop.
- **Respect item lifetimes.** C++, C#, and Python complete-response items are views tied
  to their response. Copy data you need to retain before releasing it. Streamed items
  have independent ownership. JS/TS and Rust responses contain owned data snapshots.

## Next steps

- [C++ examples](cpp/examples): chat, tool calling, embeddings, and real-time audio.
- [C++ guide](cpp/README.md), [C# guide](cs/README.md), [Python guide](python/README.md),
  [JavaScript/TypeScript guide](js/README.md), [Rust guide](rust/README.md), and
  [Java preview guide](java/README.md): language-specific setup and API details.
- [C++ API](cpp/include/foundry_local/foundry_local_cpp.h) and
  [C API](cpp/include/foundry_local/foundry_local_c.h): public types, callback contracts,
  and low-level integrations.
