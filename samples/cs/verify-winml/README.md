# Verify WinML 2.0 Execution Providers (C#)

This sample verifies that WinML 2.0 execution providers are correctly discovered,
downloaded, and registered using the Foundry Local C# SDK. It uses registered WinML
EP-backed model variants and finishes with one native streaming chat check.

## Prerequisites

- Windows with a compatible GPU
- .NET 9.0 SDK

## Build & Run

This sample uses the `Microsoft.AI.Foundry.Local` SDK package selected by
the shared central package versions. The SDK package owns its native
WinML runtime dependency on Windows, so it restores the matching
runtime package transitively.

```bash
dotnet run
```
