# C# samples local-packages discipline

## Layout
- C# samples consume the SDK from `local-packages/` via `samples/cs/nuget.config`.
- `<packageSourceMapping>` confines `Microsoft.AI.Foundry.Local*` to the `local-sdk` source — never falls through to nuget.org.
- `samples/cs/Directory.Packages.props` pins those packages with the floating `*-*` range.

## Sentinel-version trap (banned)
- A pre-release version like `2.0.0-dev.999999999999` sorts above every real timestamped `0.5.0-dev.local.<UTC>` build and silently shadows newer code under `*-*`.
- Symptom: samples build green but reference an API surface that no longer exists in source (e.g. `StreamingResponse.FinalResponse` missing because the resolved package is months stale).
- Never publish or `dotnet pack` with an all-9s sentinel version into `local-packages/`. If one appears, delete from `local-packages/` AND from the NuGet cache (`D:\.nuget\microsoft.ai.foundry.local*\<sentinel>/`) — restore will re-extract from disk otherwise.

## Rebuild recipe (sdk_v2)
From `sdk_v2/cs/`:
```
dotnet pack src/Microsoft.AI.Foundry.Local.csproj -o ../../local-packages /p:IsPacking=true /p:TreatWarningsAsErrors=false -c Release
dotnet pack src/Microsoft.AI.Foundry.Local.csproj -o ../../local-packages /p:IsPacking=true /p:UseWinML=true /p:TreatWarningsAsErrors=false -c Release
```
- `IsPacking=true` auto-sets `Version=0.5.0-dev.local.<yyyyMMddHHmmss>` (see `Microsoft.AI.Foundry.Local.csproj`).
- `UseWinML=true` flips `PackageId`/`AssemblyName` to `Microsoft.AI.Foundry.Local.WinML`, single-targets `net9.0-windows10.0.26100.0`.
- Cross-platform + WinML are independent packages — most samples reference WinML on Windows and the cross-platform package elsewhere, so both must be packed.

## Full clean-rebuild
```
Remove-Item local-packages\*.nupkg, local-packages\*.snupkg -Force
Remove-Item D:\.nuget\microsoft.ai.foundry.local* -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem samples\cs -Recurse -Directory -Include bin,obj | Remove-Item -Recurse -Force
# then re-pack (recipe above) and rebuild samples
```
