# Verify WinML 2.0 Execution Providers (C#)

This sample verifies that WinML 2.0 execution providers are correctly discovered,
downloaded, and registered using the Foundry Local C# SDK. It uses registered WinML
EP-backed model variants and finishes with one native streaming chat check.

## Prerequisites

- Windows with a compatible GPU
- .NET 9.0 SDK

## Build & Run

This sample references SDK v2 from source. Follow the [shared setup](../README.md)
to build the matching native runtime with WinML support or select its runtime
package. Native chat uses `ChatSession` and typed items, not Betalgo.

```bash
dotnet run
```
