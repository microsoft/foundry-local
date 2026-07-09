# Running Local Model Tests

Set `FOUNDRY_TEST_DATA_DIR` to a local model cache path before running tests.
The path can point to any directory layout that contains the models expected by
the test suite.

## Run the tests

```bash
export FOUNDRY_TEST_DATA_DIR=/path/to/model-cache
cd /path/to/Foundry-Local/sdk_v2/cs/test/FoundryLocal.Tests
dotnet test Microsoft.AI.Foundry.Local.Tests.csproj --configuration Release
```
