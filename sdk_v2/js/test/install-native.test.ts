// Pure installer tests: no network calls or dotnet invocation.
import { EventEmitter } from "node:events";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import type { ClientRequest, IncomingMessage } from "node:http";
import https from "node:https";
import { createRequire } from "node:module";
import { platform, tmpdir } from "node:os";
import { join } from "node:path";
import { Readable } from "node:stream";
import { fileURLToPath } from "node:url";

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const here = fileURLToPath(new URL(".", import.meta.url));
const require = createRequire(import.meta.url);
const AdmZip = require("adm-zip");

// biome-ignore lint/suspicious/noExplicitAny: .cjs module has no type declarations (see test/preload-addon.test.ts).
const installNative = require(join(here, "..", "script", "install-native.cjs")) as any;

const {
  readConfig,
  DEFAULT_FEEDS,
  redactUrl,
  redactUrlsInText,
  downloadWithRetryAndRedirects,
  runHttpMode,
  generateRestoreProjectXml,
  buildDotnetRestoreArgs,
  findRestoredPackageDir,
  collectNativeFilesFromPackageDir,
  buildNugetInstallArgs,
  findNugetPackageDir,
  main,
} = installNative;

const ORIGINAL_ENV = { ...process.env };

function resetEnv(): void {
  for (const key of Object.keys(process.env)) {
    if (key.startsWith("FOUNDRY_LOCAL_")) delete process.env[key];
  }
}

beforeEach(() => {
  resetEnv();
});

afterEach(() => {
  process.env = { ...ORIGINAL_ENV };
  vi.restoreAllMocks();
});

describe("readConfig", () => {
  it("defaults to http mode with the public default feeds", () => {
    const config = readConfig(process.env);
    expect(config.mode).toBe("http");
    expect(config.feeds).toEqual(DEFAULT_FEEDS);
    expect(config.configFile).toBeUndefined();
    expect(config.dotnetCommand).toBe("dotnet");
    expect(config.nugetCommand).toBe(platform() === "win32" ? "nuget.exe" : "nuget");
  });

  it("accepts explicit http, dotnet, and nuget modes", () => {
    process.env.FOUNDRY_LOCAL_NUGET_MODE = "http";
    expect(readConfig(process.env).mode).toBe("http");

    process.env.FOUNDRY_LOCAL_NUGET_MODE = "dotnet";
    expect(readConfig(process.env).mode).toBe("dotnet");

    process.env.FOUNDRY_LOCAL_NUGET_MODE = "nuget";
    expect(readConfig(process.env).mode).toBe("nuget");
  });

  it("rejects an invalid mode value", () => {
    process.env.FOUNDRY_LOCAL_NUGET_MODE = "ftp";
    expect(() => readConfig(process.env)).toThrow(/Invalid FOUNDRY_LOCAL_NUGET_MODE/);
  });

  it("a custom FOUNDRY_LOCAL_NUGET_FEEDS list replaces the public defaults", () => {
    process.env.FOUNDRY_LOCAL_NUGET_FEEDS = "https://feed.example/index.json;https://feed2.example/index.json";
    const config = readConfig(process.env);
    expect(config.feeds).toEqual(["https://feed.example/index.json", "https://feed2.example/index.json"]);
  });

  it("trims whitespace and drops empty entries from the feed list", () => {
    process.env.FOUNDRY_LOCAL_NUGET_FEEDS = " https://feed.example/index.json ; ;https://feed2.example/index.json";
    const config = readConfig(process.env);
    expect(config.feeds).toEqual(["https://feed.example/index.json", "https://feed2.example/index.json"]);
  });

  it("rejects a feeds list that is set but empty after trimming", () => {
    process.env.FOUNDRY_LOCAL_NUGET_FEEDS = " ; ;";
    expect(() => readConfig(process.env)).toThrow(/contains no feed URLs/);
  });

  it("rejects an explicitly empty feeds value instead of restoring defaults", () => {
    process.env.FOUNDRY_LOCAL_NUGET_FEEDS = "";
    expect(() => readConfig(process.env)).toThrow(/contains no feed URLs/);
  });

  it("rejects an invalid URL in the feed list", () => {
    process.env.FOUNDRY_LOCAL_NUGET_FEEDS = "not-a-url";
    expect(() => readConfig(process.env)).toThrow(/invalid URL/);
  });

  it("rejects feed URLs with embedded credentials", () => {
    process.env.FOUNDRY_LOCAL_NUGET_FEEDS = "https://user:secret@feed.example/index.json";
    expect(() => readConfig(process.env)).toThrow(/invalid URL/);
  });

  it("http mode requires HTTPS feeds", () => {
    process.env.FOUNDRY_LOCAL_NUGET_FEEDS = "http://feed.example/index.json";
    expect(() => readConfig(process.env)).toThrow(/must use HTTPS in http mode/);
  });

  it("dotnet mode allows non-HTTPS feeds (dotnet/NuGet owns transport trust there)", () => {
    process.env.FOUNDRY_LOCAL_NUGET_MODE = "dotnet";
    process.env.FOUNDRY_LOCAL_NUGET_FEEDS = "http://feed.example/index.json";
    expect(() => readConfig(process.env)).not.toThrow();
  });

  it("nuget mode allows non-HTTPS feeds (nuget.exe owns transport trust there)", () => {
    process.env.FOUNDRY_LOCAL_NUGET_MODE = "nuget";
    process.env.FOUNDRY_LOCAL_NUGET_FEEDS = "http://feed.example/index.json";
    expect(() => readConfig(process.env)).not.toThrow();
  });

  it("rejects FOUNDRY_LOCAL_NUGET_CONFIG in http mode", () => {
    process.env.FOUNDRY_LOCAL_NUGET_CONFIG = "C:\\NuGet.config";
    expect(() => readConfig(process.env)).toThrow(/FOUNDRY_LOCAL_NUGET_CONFIG is only valid/);
  });

  it("rejects FOUNDRY_LOCAL_DOTNET_COMMAND in http mode", () => {
    process.env.FOUNDRY_LOCAL_DOTNET_COMMAND = "dotnet8";
    expect(() => readConfig(process.env)).toThrow(/FOUNDRY_LOCAL_DOTNET_COMMAND is only valid/);
  });

  it("rejects FOUNDRY_LOCAL_NUGET_COMMAND in http mode", () => {
    process.env.FOUNDRY_LOCAL_NUGET_COMMAND = "custom-nuget";
    expect(() => readConfig(process.env)).toThrow(/FOUNDRY_LOCAL_NUGET_COMMAND is only valid/);
  });

  it("rejects FOUNDRY_LOCAL_NUGET_COMMAND in dotnet mode", () => {
    process.env.FOUNDRY_LOCAL_NUGET_MODE = "dotnet";
    process.env.FOUNDRY_LOCAL_NUGET_COMMAND = "custom-nuget";
    expect(() => readConfig(process.env)).toThrow(/FOUNDRY_LOCAL_NUGET_COMMAND is only valid/);
  });

  it("rejects FOUNDRY_LOCAL_DOTNET_COMMAND in nuget mode", () => {
    process.env.FOUNDRY_LOCAL_NUGET_MODE = "nuget";
    process.env.FOUNDRY_LOCAL_DOTNET_COMMAND = "dotnet8";
    expect(() => readConfig(process.env)).toThrow(/FOUNDRY_LOCAL_DOTNET_COMMAND is only valid/);
  });

  it("accepts FOUNDRY_LOCAL_NUGET_CONFIG and a custom dotnet command in dotnet mode", () => {
    process.env.FOUNDRY_LOCAL_NUGET_MODE = "dotnet";
    process.env.FOUNDRY_LOCAL_NUGET_CONFIG = "C:\\NuGet.config";
    process.env.FOUNDRY_LOCAL_DOTNET_COMMAND = "dotnet8";
    const config = readConfig(process.env);
    expect(config.configFile).toBe("C:\\NuGet.config");
    expect(config.dotnetCommand).toBe("dotnet8");
  });

  it("accepts FOUNDRY_LOCAL_NUGET_CONFIG and a custom nuget command in nuget mode", () => {
    process.env.FOUNDRY_LOCAL_NUGET_MODE = "nuget";
    process.env.FOUNDRY_LOCAL_NUGET_CONFIG = "C:\\NuGet.config";
    process.env.FOUNDRY_LOCAL_NUGET_COMMAND = "C:\\tools\\nuget\\nuget.exe";
    const config = readConfig(process.env);
    expect(config.mode).toBe("nuget");
    expect(config.configFile).toBe("C:\\NuGet.config");
    expect(config.nugetCommand).toBe("C:\\tools\\nuget\\nuget.exe");
  });
});

describe("HTTP feed fallback", () => {
  it.each([false, true])("falls back after NuGet.org TLS failure (proxy unavailable: %s)", async (proxyFails) => {
    const buildDir = join(here, "..", "build");
    mkdirSync(buildDir, { recursive: true });
    const binDir = mkdtempSync(join(buildDir, "feed-fallback-"));
    const requests: string[] = [];
    const zip = new AdmZip();
    zip.addFile("runtimes/win-x64/native/onnxruntime.dll", Buffer.from("runtime"));
    const packageBody: Buffer = zip.toBuffer();
    const feeds = [
      "https://api.nuget.org/v3/index.json",
      "https://packagefeedproxy.microsoft.io/nuget/v3/index.json",
      "https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/nuget/v3/index.json",
    ];
    const successfulFeed = proxyFails ? feeds[2] : feeds[1];
    const baseAddress = "https://packages.example/flat2/";
    const packageUrl = `${baseAddress}microsoft.ml.onnxruntime/1.30.0/microsoft.ml.onnxruntime.1.30.0.nupkg`;

    vi.spyOn(https, "get").mockImplementation((url, _options, callback) => {
      const address = String(url);
      requests.push(address);
      const request = new EventEmitter();
      queueMicrotask(() => {
        if (address === feeds[0] || (proxyFails && address === feeds[1])) {
          request.emit("error", new Error("EPROTO: TLS handshake failure"));
          return;
        }
        const body =
          address === successfulFeed
            ? JSON.stringify({ resources: [{ "@type": "PackageBaseAddress/3.0.0", "@id": baseAddress }] })
            : address === packageUrl
              ? packageBody
              : undefined;
        if (body === undefined) {
          request.emit("error", new Error(`Unexpected request: ${address}`));
          return;
        }
        const response = Readable.from([body]) as IncomingMessage;
        response.statusCode = 200;
        response.headers = {};
        callback?.(response);
      });
      return request as ClientRequest;
    });
    vi.spyOn(console, "log").mockImplementation(() => {});
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});

    try {
      await runHttpMode(
        readConfig(process.env),
        [{ name: "Microsoft.ML.OnnxRuntime", version: "1.30.0", expected: "onnxruntime.dll" }],
        binDir,
        { rid: "win-x64", ext: ".dll" },
      );
      expect(requests).toEqual([...feeds.slice(0, proxyFails ? 3 : 2), packageUrl]);
      expect(readFileSync(join(binDir, "onnxruntime.dll"), "utf8")).toBe("runtime");
      expect(warn).toHaveBeenCalledTimes(proxyFails ? 2 : 1);
    } finally {
      rmSync(binDir, { recursive: true, force: true });
    }
  });
});

describe("redactUrl", () => {
  it("strips the query string", () => {
    expect(redactUrl("https://example.com/pkg.nupkg?sv=2021&sig=SECRET")).toBe("https://example.com/pkg.nupkg");
  });

  describe("redactUrlsInText", () => {
    it("redacts query strings from URLs in command output", () => {
      const output = "error downloading https://blob.example/pkg.nupkg?sig=SECRET and retrying";
      expect(redactUrlsInText(output)).toBe("error downloading https://blob.example/pkg.nupkg and retrying");
    });
  });

  it("strips the fragment", () => {
    expect(redactUrl("https://example.com/pkg.nupkg#token=SECRET")).toBe("https://example.com/pkg.nupkg");
  });

  it("strips embedded credentials", () => {
    const url = `https://${"user"}:${"secret"}@example.com/pkg.nupkg`;
    const redacted = redactUrl(url);
    expect(redacted).toBe("https://example.com/pkg.nupkg");
    expect(redacted).not.toContain("user");
    expect(redacted).not.toContain("secret");
  });

  it("leaves a URL with no query/fragment unchanged", () => {
    expect(redactUrl("https://example.com/pkg.nupkg")).toBe("https://example.com/pkg.nupkg");
  });

  it("returns non-URL input as-is rather than throwing", () => {
    expect(redactUrl("not a url")).toBe("not a url");
  });
});

describe("downloadWithRetryAndRedirects", () => {
  it("follows redirects (absolute and relative) and returns the final body", async () => {
    const requests: string[] = [];
    const responses = [
      { statusCode: 302, location: "https://blob.example/package" },
      { statusCode: 302, location: "/final" },
      { statusCode: 200, body: "{}" },
    ];
    const request = (
      url: string,
      _options: unknown,
      callback: (response: Readable & { statusCode: number; headers: { location?: string | undefined } }) => void,
    ) => {
      requests.push(url);
      const next = responses.shift();
      if (!next) throw new Error("Unexpected request");
      const response = new Readable({
        read() {
          if (next.body) this.push(next.body);
          this.push(null);
        },
      }) as Readable & { statusCode: number; headers: { location?: string | undefined } };
      response.statusCode = next.statusCode;
      response.headers = { location: next.location };
      queueMicrotask(() => callback(response));
      return new EventEmitter();
    };

    const result = await downloadWithRetryAndRedirects("https://feed.example/index.json", { request });

    expect(result).toBe("{}");
    expect(requests).toEqual([
      "https://feed.example/index.json",
      "https://blob.example/package",
      "https://blob.example/final",
    ]);
  });
});

describe("generateRestoreProjectXml", () => {
  it("includes bracketed exact versions for every artifact", () => {
    const xml = generateRestoreProjectXml([
      { name: "Microsoft.ML.OnnxRuntime", version: "1.30.0", expected: "onnxruntime.dll" },
      { name: "Microsoft.ML.OnnxRuntimeGenAI.Foundry", version: "0.17.1", expected: "onnxruntime-genai.dll" },
    ]);
    expect(xml).toContain('<PackageReference Include="Microsoft.ML.OnnxRuntime" Version="[1.30.0]" />');
    expect(xml).toContain('<PackageReference Include="Microsoft.ML.OnnxRuntimeGenAI.Foundry" Version="[0.17.1]" />');
  });

  it("targets net8.0", () => {
    const xml = generateRestoreProjectXml([{ name: "A", version: "1.0.0", expected: "a.dll" }]);
    expect(xml).toContain("<TargetFramework>net8.0</TargetFramework>");
  });
});

describe("buildDotnetRestoreArgs", () => {
  const config = { feeds: ["https://a.example/index.json", "https://b.example/index.json"] };

  it("passes --source per feed and no --configfile when no config file is set", () => {
    const args = buildDotnetRestoreArgs(
      { ...config, configFile: undefined },
      {
        projectPath: "proj.csproj",
        packagesDir: "pkgs",
      },
    );
    expect(args).toEqual([
      "restore",
      "proj.csproj",
      "--packages",
      "pkgs",
      "--no-cache",
      "--source",
      "https://a.example/index.json",
      "--source",
      "https://b.example/index.json",
    ]);
  });

  it("passes --configfile and no --source args when a config file is set", () => {
    const args = buildDotnetRestoreArgs(
      { ...config, configFile: "NuGet.config" },
      {
        projectPath: "proj.csproj",
        packagesDir: "pkgs",
      },
    );
    expect(args).toEqual([
      "restore",
      "proj.csproj",
      "--packages",
      "pkgs",
      "--no-cache",
      "--configfile",
      "NuGet.config",
    ]);
  });

  it("never includes --runtime", () => {
    const args = buildDotnetRestoreArgs(
      { ...config, configFile: undefined },
      {
        projectPath: "proj.csproj",
        packagesDir: "pkgs",
      },
    );
    expect(args).not.toContain("--runtime");
  });
});

describe("findRestoredPackageDir", () => {
  let packagesDir: string;

  beforeEach(() => {
    packagesDir = mkdtempSync(join(tmpdir(), "install-native-test-"));
  });

  afterEach(() => {
    rmSync(packagesDir, { recursive: true, force: true });
  });

  it("finds the lowercased id/version directory", () => {
    const dir = join(packagesDir, "microsoft.ml.onnxruntime", "1.30.0");
    mkdirSync(dir, { recursive: true });
    expect(findRestoredPackageDir(packagesDir, "Microsoft.ML.OnnxRuntime", "1.30.0")).toBe(dir);
  });

  it("throws when the expected package directory is missing", () => {
    expect(() => findRestoredPackageDir(packagesDir, "Nonexistent.Package", "9.9.9")).toThrow(
      /Restored package not found/,
    );
  });
});

describe("buildNugetInstallArgs", () => {
  const config = { feeds: ["https://a.example/index.json", "https://b.example/index.json"] };

  it("passes exact package-only install flags and each configured source", () => {
    const args = buildNugetInstallArgs(
      { ...config, configFile: undefined },
      { id: "Microsoft.ML.OnnxRuntime", version: "1.30.0", outputDir: "pkgs" },
    );
    expect(args).toEqual([
      "install",
      "Microsoft.ML.OnnxRuntime",
      "-Version",
      "1.30.0",
      "-OutputDirectory",
      "pkgs",
      "-NonInteractive",
      "-DirectDownload",
      "-DependencyVersion",
      "Ignore",
      "-Source",
      "https://a.example/index.json",
      "-Source",
      "https://b.example/index.json",
    ]);
  });

  it("passes -ConfigFile and no -Source args when a config file is set", () => {
    const args = buildNugetInstallArgs(
      { ...config, configFile: "NuGet.config" },
      { id: "Microsoft.ML.OnnxRuntimeGenAI.Foundry", version: "0.17.1", outputDir: "pkgs" },
    );
    expect(args).toEqual([
      "install",
      "Microsoft.ML.OnnxRuntimeGenAI.Foundry",
      "-Version",
      "0.17.1",
      "-OutputDirectory",
      "pkgs",
      "-NonInteractive",
      "-DirectDownload",
      "-DependencyVersion",
      "Ignore",
      "-ConfigFile",
      "NuGet.config",
    ]);
  });

  it("a custom feed list (not the public defaults) fully replaces -Source values", () => {
    const args = buildNugetInstallArgs(
      { feeds: ["https://private.example/nuget/v3/index.json"], configFile: undefined },
      { id: "A", version: "1.0.0", outputDir: "pkgs" },
    );
    expect(args.filter((a: string) => a === "-Source")).toHaveLength(1);
    expect(args).toContain("https://private.example/nuget/v3/index.json");
    expect(args).not.toContain("https://api.nuget.org/v3/index.json");
  });

  it("keeps real feed URLs in spawn args but still redacts them when logged", () => {
    const feedWithSecret = "https://feed.example/index.json?pat=SECRET123";
    const args = buildNugetInstallArgs(
      { feeds: [feedWithSecret], configFile: undefined },
      {
        id: "A",
        version: "1.0.0",
        outputDir: "pkgs",
      },
    );
    expect(args.join(" ")).toContain("SECRET123");
    expect(redactUrlsInText(args.join(" "))).not.toContain("SECRET123");
  });
});

describe("findNugetPackageDir", () => {
  let outputDir: string;

  beforeEach(() => {
    outputDir = mkdtempSync(join(tmpdir(), "install-native-test-nuget-"));
  });

  afterEach(() => {
    rmSync(outputDir, { recursive: true, force: true });
  });

  it("finds the id.version directory using nuget's original casing", () => {
    const dir = join(outputDir, "Microsoft.ML.OnnxRuntime.1.30.0");
    mkdirSync(dir, { recursive: true });
    expect(findNugetPackageDir(outputDir, "Microsoft.ML.OnnxRuntime", "1.30.0")).toBe(dir);
  });

  it("matches case-insensitively regardless of the casing nuget.exe produced", () => {
    const dir = join(outputDir, "microsoft.ml.onnxruntimegenai.foundry.0.17.1");
    mkdirSync(dir, { recursive: true });
    expect(findNugetPackageDir(outputDir, "Microsoft.ML.OnnxRuntimeGenAI.Foundry", "0.17.1")).toBe(dir);
  });

  it("only looks at immediate children of outputDir, not nested dependency package folders", () => {
    // Simulates nuget.exe restoring a transitive dependency alongside the requested package —
    // only the exact id.version match at the root should be returned.
    mkdirSync(join(outputDir, "Some.Other.Dependency.2.0.0"), { recursive: true });
    const dir = join(outputDir, "Microsoft.ML.OnnxRuntime.1.30.0");
    mkdirSync(join(dir, "runtimes", "win-x64", "native"), { recursive: true });
    expect(findNugetPackageDir(outputDir, "Microsoft.ML.OnnxRuntime", "1.30.0")).toBe(dir);
  });

  it("throws when the expected package directory is missing", () => {
    expect(() => findNugetPackageDir(outputDir, "Nonexistent.Package", "9.9.9")).toThrow(/Restored package not found/);
  });
});

describe("collectNativeFilesFromPackageDir", () => {
  let packageDir: string;

  beforeEach(() => {
    packageDir = mkdtempSync(join(tmpdir(), "install-native-test-pkg-"));
  });

  afterEach(() => {
    rmSync(packageDir, { recursive: true, force: true });
  });

  it("collects files under runtimes/<rid>/native/", () => {
    const nativeDir = join(packageDir, "runtimes", "win-x64", "native");
    mkdirSync(nativeDir, { recursive: true });
    writeFileSync(join(nativeDir, "onnxruntime.dll"), "");
    writeFileSync(join(nativeDir, "readme.txt"), "");

    const files = collectNativeFilesFromPackageDir(packageDir, "win-x64", ".dll");
    expect(files).toEqual([join(nativeDir, "onnxruntime.dll")]);
  });

  it("collects loose files directly under runtimes/<rid>/ but not nested subfolders", () => {
    const runtimeDir = join(packageDir, "runtimes", "linux-x64");
    mkdirSync(runtimeDir, { recursive: true });
    writeFileSync(join(runtimeDir, "libonnxruntime.so.1"), "");
    mkdirSync(join(runtimeDir, "lib"), { recursive: true });
    writeFileSync(join(runtimeDir, "lib", "libonnxruntime.so.1"), "");

    const files = collectNativeFilesFromPackageDir(packageDir, "linux-x64", ".so");
    expect(files).toEqual([join(runtimeDir, "libonnxruntime.so.1")]);
  });

  it("returns an empty array when the RID has no matching directory", () => {
    expect(collectNativeFilesFromPackageDir(packageDir, "osx-arm64", ".dylib")).toEqual([]);
  });
});

describe("main() — FOUNDRY_LOCAL_SKIP_INSTALL", () => {
  it("returns 0 immediately without touching the filesystem or network", async () => {
    process.env.FOUNDRY_LOCAL_SKIP_INSTALL = "1";
    await expect(main()).resolves.toBe(0);
  });
});
