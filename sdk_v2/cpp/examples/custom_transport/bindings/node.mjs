// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

import assert from "node:assert/strict";
import http from "node:http";
import https from "node:https";
import { once } from "node:events";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

// This adapter prototype has no SDK registration. A future addon bridge calls these methods from native workers.
export class NodeHttpOperation {
  #input;
  #controller = new AbortController();
  #request;
  #response;
  #reader;
  #pending = Buffer.alloc(0);
  #timer;
  #sent = false;

  constructor(input) {
    this.#input = input;
  }

  async send() {
    if (this.#sent) throw new Error("Send can run only once");
    this.#sent = true;
    this.#controller.signal.throwIfAborted();
    const url = new URL(this.#input.url);
    if (url.protocol !== "http:" && url.protocol !== "https:") throw new Error("Unsupported URL scheme");
    const client = url.protocol === "https:" ? https : http;
    const headers = this.#input.headers.flatMap(([name, value]) => [
      name, typeof value === "string" ? value : Buffer.from(value).toString("latin1"),
    ]);
    const hasHeader = (name) => this.#input.headers.some(([key]) => key.toLowerCase() === name);
    // Node does not add Host when the caller supplies the raw header array.
    if (!hasHeader("host")) headers.push("Host", url.host);
    if (this.#input.body?.length > 0 && !hasHeader("content-length") && !hasHeader("transfer-encoding")) {
      headers.push("Content-Length", String(this.#input.body.length));
    }

    const timeoutMs = this.#input.timeoutMs ?? 0;
    if (timeoutMs > 0) {
      this.#timer = setTimeout(() => this.#controller.abort(new Error("Transport deadline exceeded")), timeoutMs);
    }

    try {
      this.#response = await new Promise((resolve, reject) => {
        this.#request = client.request(url, {
          method: this.#input.method,
          headers,
          signal: this.#controller.signal,
        }, resolve);
        this.#request.on("error", reject);
        this.#request.end(this.#input.body);
      });
      this.#reader = this.#response[Symbol.asyncIterator]();
      const responseHeaders = [];
      for (let i = 0; i < this.#response.rawHeaders.length; i += 2) {
        responseHeaders.push([this.#response.rawHeaders[i], Buffer.from(this.#response.rawHeaders[i + 1], "latin1")]);
      }

      return { statusCode: this.#response.statusCode, headers: responseHeaders };
    } catch (error) {
      clearTimeout(this.#timer);
      this.#controller.signal.throwIfAborted();
      throw error;
    }
  }

  async read(capacity) {
    if (!Number.isSafeInteger(capacity) || capacity <= 0) throw new Error("Read capacity must be positive");
    if (!this.#reader) throw new Error("Send must complete before Read");
    this.#controller.signal.throwIfAborted();
    while (this.#pending.length === 0) {
      let chunk;
      try {
        chunk = await this.#reader.next();
      } catch (error) {
        this.#controller.signal.throwIfAborted();
        throw error;
      }

      const { value, done } = chunk;
      this.#controller.signal.throwIfAborted();
      if (done) {
        clearTimeout(this.#timer);
        return Buffer.alloc(0);
      }

      this.#pending = value;
    }

    const output = this.#pending.subarray(0, capacity);
    this.#pending = this.#pending.subarray(output.length);
    return output;
  }

  cancel() {
    this.#controller.abort();
  }

  close() {
    clearTimeout(this.#timer);
    this.#controller.abort();
    this.#response?.destroy();
    this.#request?.destroy();
  }
}

async function demonstrate() {
  const server = http.createServer((request, response) => {
    if (request.url === "/blocked-headers") return;
    if (request.url === "/blocked-body") {
      response.writeHead(200);
      response.flushHeaders();
      return;
    }

    if (request.url === "/denied") {
      response.writeHead(403);
      response.end("denied");
      return;
    }

    if (request.url === "/post") {
      request.pipe(response);
      return;
    }

    response.writeHead(200, { "X-Example": ["first", "second"] });
    response.write("hello ");
    const timer = setTimeout(() => response.end("transport"), 10);
    response.on("close", () => clearTimeout(timer));
  });
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const input = {
    method: "GET",
    url: `http://127.0.0.1:${server.address().port}/model`,
    headers: [],
    body: Buffer.alloc(0),
    timeoutMs: 2000,
  };
  const operation = new NodeHttpOperation(input);
  const canceled = new NodeHttpOperation(input);
  const blockedSend = new NodeHttpOperation({ ...input, url: input.url.replace("/model", "/blocked-headers") });
  const blockedRead = new NodeHttpOperation({ ...input, url: input.url.replace("/model", "/blocked-body") });
  const denied = new NodeHttpOperation({ ...input, url: input.url.replace("/model", "/denied") });
  const post = new NodeHttpOperation({
    ...input, method: "POST", url: input.url.replace("/model", "/post"), body: Buffer.from("catalog query"),
  });
  const deadline = new NodeHttpOperation({
    ...input, url: input.url.replace("/model", "/blocked-headers"), timeoutMs: 20,
  });
  try {
    const head = await operation.send();
    assert.equal(head.statusCode, 200);
    assert.equal(head.headers.filter(([name]) => name.toLowerCase() === "x-example").length, 2);
    const parts = [];
    for (;;) {
      const bytes = await operation.read(3);
      if (bytes.length === 0) break;
      assert.ok(bytes.length <= 3);
      parts.push(bytes);
    }

    assert.equal(Buffer.concat(parts).toString(), "hello transport");
    canceled.cancel();
    await assert.rejects(canceled.send(), { name: "AbortError" });
    const requestReceived = once(server, "request");
    const sendRejected = assert.rejects(blockedSend.send(), { name: "AbortError" });
    await requestReceived;
    blockedSend.cancel();
    await sendRejected;
    await blockedRead.send();
    const readRejected = assert.rejects(blockedRead.read(3), { name: "AbortError" });
    blockedRead.cancel();
    await readRejected;
    assert.equal((await denied.send()).statusCode, 403);
    assert.equal((await post.send()).statusCode, 200);
    assert.equal((await post.read(100)).toString(), "catalog query");
    await assert.rejects(deadline.send(), { message: "Transport deadline exceeded" });
    console.log("Node transport: streamed bytes, duplicate headers, POST, HTTP errors, cancellation, and deadlines");
  } finally {
    operation.close();
    canceled.close();
    blockedSend.close();
    blockedRead.close();
    denied.close();
    post.close();
    deadline.close();
    server.close();
    await once(server, "close");
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  await demonstrate();
}
