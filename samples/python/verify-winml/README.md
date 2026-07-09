# Verify WinML 2.0 Execution Providers

This sample verifies that WinML 2.0 execution providers are correctly discovered,
downloaded, and registered. It then runs inference on a model variant backed by a
registered WinML EP. It finishes with one native streaming chat check.

## Prerequisites

- Windows with a compatible GPU
- Python 3.11+

## Setup

Use a fresh virtual environment for the cleanest setup.

If you want to reuse your existing Python environment instead, delete that
environment's `Lib\site-packages\foundry_local_core` directory before
reinstalling so stale native files are not left behind.

`requirements.txt` installs the single `foundry-local-sdk` package, which bundles
WinML hardware acceleration on Windows automatically. Either install path is enough:

```bash
python -m venv .venv
.venv\Scripts\Activate.ps1
pip install --upgrade -r requirements.txt
```

Or, after removing `Lib\site-packages\foundry_local_core` from your existing
Python environment:

```bash
pip install --upgrade -r requirements.txt
```

## Run

```bash
python src/app.py
```

## What it tests

1. **EP Discovery** — Lists all available execution providers
2. **EP Download & Registration** — Downloads only the WinML EPs relevant to the machine
3. **Model Catalog** — Lists model variants backed by the registered WinML EPs
4. **Streaming Chat** — Runs streaming chat completion on a WinML EP-backed model via native SDK
