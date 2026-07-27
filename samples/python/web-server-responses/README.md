# Foundry Local Python Responses Web-Service Sample

This sample starts the Foundry Local OpenAI-compatible web service, then calls the Responses API with the official OpenAI Python client.

It demonstrates:

- A non-streaming `/v1/responses` call
- A streaming `/v1/responses` call
- A function/tool-calling round trip using `previous_response_id`

## What gets installed

Install the sample dependencies from `requirements.txt`:

```bash
pip install -r requirements.txt
```

That installs:

- `foundry-local-sdk`, which bundles WinML hardware acceleration on Windows automatically
- `openai`

The sample downloads/registers Foundry Local execution providers and downloads the `qwen2.5-0.5b` model the first time it runs.

## Run the sample

From this directory:

```bash
python -m venv .venv
.\.venv\Scripts\activate
pip install -r requirements.txt
python src\app.py
```

On macOS or Linux, activate the virtual environment with:

```bash
source .venv/bin/activate
```

The sample starts the local web service, sends Responses API requests to `http://localhost:<port>/v1`, prints the model output, and then unloads the model and stops the web service.
