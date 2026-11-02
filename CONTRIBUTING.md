## Contributing

This project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit https://cla.opensource.microsoft.com.

When you submit a pull request, a CLA bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., status check, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.

## GitHub Actions pinning

Pin GitHub Actions and reusable workflows to full-length commit SHAs with version comments, for example:

```yaml
uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
```

The Lint workflow uses [pinact](https://github.com/suzuki-shunsuke/pinact) to enforce this on pull requests,
pushes to `main`, and manual runs without validation API calls or file modifications.
After installing pinact v4.0.0, run the same read-only check locally:

```sh
pinact run --fix=false --no-api
```

The workflow also runs nightly at 06:23 UTC, using authenticated GitHub API calls to verify that version
comments match their commits. To reproduce this read-only verification locally, provide a GitHub token
via `GITHUB_TOKEN` and run:

```sh
pinact run --fix=false --verify
```

Dependabot checks for GitHub Actions updates weekly and maintains the SHA pins and version comments.

## Trademarks

This project may contain trademarks or logos for projects, products, or services. Authorized use of Microsoft 
trademarks or logos is subject to and must follow 
[Microsoft's Trademark & Brand Guidelines](https://www.microsoft.com/en-us/legal/intellectualproperty/trademarks/usage/general).
Use of Microsoft trademarks or logos in modified versions of this project must not cause confusion or imply Microsoft sponsorship.
Any use of third-party trademarks or logos are subject to those third-party's policies.
