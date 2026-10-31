# Microsoft AI Foundry Local Runtime

The Microsoft AI Foundry Local Runtime NuGet package provides the native runtime libraries used by the Foundry Local SDK.

The package includes platform-specific native assets for Windows x64, Windows ARM64, Linux x64, Linux ARM64, and macOS ARM64. Windows packages also include the `foundry_local.lib` import library for applications that link against `foundry_local.dll`.

## C and C++ Applications

No source build is required. Restore `Microsoft.AI.Foundry.Local.Runtime` and its
dependencies directly with the NuGet CLI. This is also the native dependency of the
`Microsoft.AI.Foundry.Local` C# SDK package, without the managed SDK dependencies.

```bash
nuget install Microsoft.AI.Foundry.Local.Runtime -OutputDirectory packages
```

The Runtime package includes public headers under `build/native/include/` and
native libraries under `runtimes/<rid>/native/`. Add the header directory to your
include path, link the library for your target RID, and deploy the matching native
dependencies from the restored packages alongside your application.
The C API uses `foundry_local/foundry_local_c.h`; the header-only C++17 wrapper uses
`foundry_local/foundry_local_cpp.h`. A .NET runtime is not required by native applications.

For SDK usage and development documentation, see the [Foundry Local repository](https://github.com/microsoft/Foundry-Local).
