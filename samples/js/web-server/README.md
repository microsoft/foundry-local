# Web server

This sample loads a chat model, starts the built-in Foundry Local web server, discovers the bound URL from the
manager, calls the OpenAI-compatible `/v1/chat/completions` endpoint with Node's built-in `fetch`, and then stops the
server and releases native resources.

```bash
npm install
npm start
```

The default model alias is `qwen2.5-0.5b`. Pass another chat model alias as the first argument.
