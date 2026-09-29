# Running Local Model Tests

## Configuration

The test model cache directory name is configured in `sdk/cs/test/FoundryLocal.Tests/appsettings.Test.json`:

```json
{
  "TestModelCacheDirName": "test-data-shared"
}
```

If the value is a directory name it will be resolved as <repository-root>/../{TestModelCacheDirName}.
Otherwise the value will be resolved using Path.GetFullPath, which allows for absolute paths or
relative paths based on the current working directory.

## Run the tests

The tests will automatically find the models in the configured test model cache directory.

```bash
cd /path/to/parent-dir/foundry-local-sdk/sdk/cs/test/FoundryLocal.Tests
dotnet test Microsoft.AI.Foundry.Local.Tests.csproj --configuration Release
```
