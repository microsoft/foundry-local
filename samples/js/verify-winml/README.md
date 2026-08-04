# Verify WinML 2.0 Execution Providers (JavaScript)

This sample verifies that WinML 2.0 execution providers are correctly discovered,
downloaded, and registered using the Foundry Local JavaScript SDK. It uses registered
WinML EP-backed model variants and finishes with one native streaming chat check.

## Prerequisites

- Windows with a compatible GPU
- Node.js 20+

## Setup

`package.json` installs the unified `foundry-local-sdk` package, which includes
WinML support on Windows. To validate a local v2 build, use the sample runner
from the parent directory; it installs the locally packed SDK tarball:

```bash
pwsh ../test-v2.ps1 -Sample verify-winml -Run
```

## Run

```bash
npm start
```
