// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Post-install / CI step that fetches ORT + ORT-GenAI native binaries from
// NuGet and stages them into sdk_v2/js/prebuilds/<plat>-<arch>/ next to
// foundry_local.{dll,so,dylib} and the .node addons.
//
// Uses the NuGet v3 HTTP API by default, or an explicit dotnet-restore or
// nuget.exe-install mode for feeds configured through NuGet credential providers.
//
// Ported from sdk/js/script/install-{standard,utils}.cjs. Differences:
//   * Targets sdk_v2/js/prebuilds/<plat>-<arch>/ (v2's single addon dir)
//     instead of v1's per-platform foundry-local-core/ subpackage layout.
//   * Does NOT download Microsoft.AI.Foundry.Local.Core — the v2 build/pack
//     pipeline ships foundry_local itself inside the .tgz prebuilds dir.
//   * Reads sdk_v2/deps_versions.json with the same dual-path fallback v1
//     uses (next to the script when published, two levels up in the repo).

"use strict";

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const https = require("node:https");
const { spawnSync } = require("node:child_process");
const AdmZip = require("adm-zip");

const PLATFORM_MAP = {
  "win32-x64": "win-x64",
  "win32-arm64": "win-arm64",
  "linux-x64": "linux-x64",
  "linux-arm64": "linux-arm64",
  "darwin-arm64": "osx-arm64",
};

const DEFAULT_FEEDS = [
  "https://api.nuget.org/v3/index.json",
  "https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/nuget/v3/index.json",
];

const VALID_MODES = new Set(["http", "dotnet", "nuget"]);

function readConfig(env) {
  const modeRaw = env.FOUNDRY_LOCAL_NUGET_MODE;
  const mode = modeRaw === undefined || modeRaw === "" ? "http" : modeRaw;
  if (!VALID_MODES.has(mode)) {
    throw new Error(`Invalid FOUNDRY_LOCAL_NUGET_MODE '${modeRaw}'. Expected 'http', 'dotnet', or 'nuget'.`);
  }

  const feedsRaw = env.FOUNDRY_LOCAL_NUGET_FEEDS;
  const feeds =
    feedsRaw === undefined
      ? DEFAULT_FEEDS.slice()
      : feedsRaw
          .split(";")
          .map((f) => f.trim())
          .filter(Boolean);
  if (feedsRaw !== undefined && feeds.length === 0) {
    throw new Error("FOUNDRY_LOCAL_NUGET_FEEDS is set but contains no feed URLs.");
  }
  for (const feed of feeds) {
    try {
      const url = new URL(feed);
      if (url.username || url.password) {
        throw new Error("embedded credentials are not supported");
      }
    } catch {
      throw new Error(`FOUNDRY_LOCAL_NUGET_FEEDS contains an invalid URL: ${redactUrl(feed)}`);
    }
  }

  const configFile = env.FOUNDRY_LOCAL_NUGET_CONFIG || undefined;
  const dotnetCommandRaw = env.FOUNDRY_LOCAL_DOTNET_COMMAND || undefined;
  const nugetCommandRaw = env.FOUNDRY_LOCAL_NUGET_COMMAND || undefined;

  if (mode === "http") {
    for (const feed of feeds) {
      if (new URL(feed).protocol !== "https:") {
        throw new Error(`FOUNDRY_LOCAL_NUGET_FEEDS must use HTTPS in http mode: ${redactUrl(feed)}`);
      }
    }
    if (configFile) {
      throw new Error("FOUNDRY_LOCAL_NUGET_CONFIG is only valid when FOUNDRY_LOCAL_NUGET_MODE is 'dotnet' or 'nuget'.");
    }
    if (dotnetCommandRaw) {
      throw new Error("FOUNDRY_LOCAL_DOTNET_COMMAND is only valid when FOUNDRY_LOCAL_NUGET_MODE=dotnet.");
    }
    if (nugetCommandRaw) {
      throw new Error("FOUNDRY_LOCAL_NUGET_COMMAND is only valid when FOUNDRY_LOCAL_NUGET_MODE=nuget.");
    }
  } else {
    if (mode === "dotnet" && nugetCommandRaw) {
      throw new Error("FOUNDRY_LOCAL_NUGET_COMMAND is only valid when FOUNDRY_LOCAL_NUGET_MODE=nuget.");
    }
    if (mode === "nuget" && dotnetCommandRaw) {
      throw new Error("FOUNDRY_LOCAL_DOTNET_COMMAND is only valid when FOUNDRY_LOCAL_NUGET_MODE=dotnet.");
    }
  }

  // nuget.exe is the canonical distribution name on Windows; POSIX users typically shim a
  // `nuget` script (e.g. `mono nuget.exe`) onto PATH. FOUNDRY_LOCAL_NUGET_COMMAND overrides either way.
  const defaultNugetCommand = os.platform() === "win32" ? "nuget.exe" : "nuget";

  return {
    mode,
    feeds,
    configFile,
    dotnetCommand: dotnetCommandRaw || "dotnet",
    nugetCommand: nugetCommandRaw || defaultNugetCommand,
  };
}

function redactUrl(urlString) {
  try {
    const u = new URL(urlString);
    u.username = "";
    u.password = "";
    u.search = "";
    u.hash = "";
    return u.toString();
  } catch {
    return urlString;
  }
}

function redactUrlsInText(text) {
  return String(text).replace(/https?:\/\/[^\s"'<>]+/gi, (url) => redactUrl(url));
}

function safeErrorMessage(error) {
  return redactUrlsInText(error instanceof Error ? error.message : String(error));
}

function detectPlatform() {
  const platformKey = `${os.platform()}-${os.arch()}`;
  const rid = PLATFORM_MAP[platformKey];
  if (!rid) return null;
  const ext = os.platform() === "win32" ? ".dll" : os.platform() === "darwin" ? ".dylib" : ".so";
  const libPrefix = os.platform() === "win32" ? "" : "lib";
  return { platformKey, rid, ext, libPrefix };
}

function loadDeps(pkgRoot) {
  const depsPath = fs.existsSync(path.join(pkgRoot, "deps_versions.json"))
    ? path.join(pkgRoot, "deps_versions.json")
    : path.resolve(pkgRoot, "..", "deps_versions.json");
  if (!fs.existsSync(depsPath)) {
    throw new Error(`deps_versions.json not found at ${depsPath}`);
  }
  return JSON.parse(fs.readFileSync(depsPath, "utf8"));
}

function buildArtifacts(deps, platform) {
  const ortVersion = deps.onnxruntime.version;
  const genaiVersion = deps["onnxruntime-genai"].version;
  const ortMajor = ortVersion.split(".")[0];

  // libfoundry_local's SONAME/install_name dependency on ORT is versioned (see normalizeOrtLibName below);
  // Windows has no soname concept so onnxruntime.dll stays unversioned.
  const expectedOrt = () => {
    if (platform.rid.startsWith("linux")) return "libonnxruntime.so.1";
    if (platform.rid.startsWith("osx")) return `libonnxruntime.${ortMajor}.dylib`;
    return "onnxruntime.dll";
  };
  const expectedGenai = () => `${platform.libPrefix}onnxruntime-genai${platform.ext}`;

  return [
    { name: "Microsoft.ML.OnnxRuntime", version: ortVersion, expected: expectedOrt() },
    { name: "Microsoft.ML.OnnxRuntimeGenAI.Foundry", version: genaiVersion, expected: expectedGenai() },
  ];
}

async function downloadWithRetryAndRedirects(url, { destStream = null, request = https.get } = {}) {
  const maxRedirects = 5;
  let currentUrl = url;
  let redirects = 0;

  while (redirects < maxRedirects) {
    const response = await new Promise((resolve, reject) => {
      request(currentUrl, {}, (res) => resolve(res)).on("error", reject);
    });

    if (response.statusCode >= 300 && response.statusCode < 400 && response.headers.location) {
      const nextUrl = new URL(response.headers.location, currentUrl).toString();
      response.resume();
      redirects++;
      console.log(`  Following redirect to ${new URL(nextUrl).host}...`);
      currentUrl = nextUrl;
      continue;
    }

    if (response.statusCode !== 200) {
      throw new Error(`Download failed with status ${response.statusCode}: ${redactUrl(currentUrl)}`);
    }

    if (destStream) {
      response.pipe(destStream);
      return new Promise((resolve, reject) => {
        destStream.on("finish", resolve);
        destStream.on("error", reject);
        response.on("error", reject);
      });
    }

    let data = "";
    response.on("data", (chunk) => {
      data += chunk;
    });
    return new Promise((resolve, reject) => {
      response.on("end", () => resolve(data));
      response.on("error", reject);
    });
  }
  throw new Error(`Too many redirects: ${redactUrl(url)}`);
}

async function downloadJson(url, opts) {
  return JSON.parse(await downloadWithRetryAndRedirects(url, opts));
}

async function downloadFile(url, dest, opts) {
  const file = fs.createWriteStream(dest);
  try {
    await downloadWithRetryAndRedirects(url, { ...opts, destStream: file });
    file.close();
  } catch (e) {
    file.close();
    if (fs.existsSync(dest)) fs.unlinkSync(dest);
    throw e;
  }
}

async function getBaseAddress(feedUrl, cache) {
  if (!cache.has(feedUrl)) {
    cache.set(feedUrl, await downloadJson(feedUrl));
  }
  const resources = cache.get(feedUrl).resources || [];
  const res = resources.find((r) => r["@type"]?.startsWith("PackageBaseAddress/3.0.0"));
  if (!res) throw new Error(`Could not find PackageBaseAddress/3.0.0 in NuGet feed: ${redactUrl(feedUrl)}`);
  const baseAddress = res["@id"];
  const normalized = baseAddress.endsWith("/") ? baseAddress : `${baseAddress}/`;
  if (new URL(normalized).protocol !== "https:") {
    throw new Error(`PackageBaseAddress must use HTTPS in http mode: ${redactUrl(normalized)}`);
  }
  return normalized;
}

function entryFileName(entry) {
  const normalized = entry.entryName.replace(/\\/g, "/");
  return normalized.slice(normalized.lastIndexOf("/") + 1);
}

function isNativeFileName(name, ext) {
  return name.toLowerCase().endsWith(ext) || /\.so(\.\d+)+$/i.test(name);
}

function nativeEntriesForRid(zip, rid, ext) {
  const nativePrefix = `runtimes/${rid}/native/`.toLowerCase();
  const runtimePrefix = `runtimes/${rid}/`.toLowerCase();
  return zip.getEntries().filter((e) => {
    const p = e.entryName.toLowerCase();
    if (!isNativeFileName(p, ext)) return false;
    if (p.startsWith(nativePrefix)) return true;
    if (p.startsWith(runtimePrefix)) {
      const relative = p.slice(runtimePrefix.length);
      return relative.length > 0 && !relative.includes("/");
    }
    return false;
  });
}

async function installPackageHttp(artifact, tempDir, binDir, config, platform, cache) {
  if (artifact.expected && fs.existsSync(path.join(binDir, artifact.expected))) {
    console.log(`  ${artifact.name}: ${artifact.expected} already present, skipping download.`);
    return;
  }

  let lastError;
  const { feeds } = config;
  for (let i = 0; i < feeds.length; i++) {
    const feedUrl = feeds[i];
    const feedHost = new URL(feedUrl).host;
    try {
      const baseAddress = await getBaseAddress(feedUrl, cache);
      const nameLower = artifact.name.toLowerCase();
      const verLower = artifact.version.toLowerCase();
      const downloadUrl = `${baseAddress}${nameLower}/${verLower}/${nameLower}.${verLower}.nupkg`;

      const nupkgPath = path.join(tempDir, `${artifact.name}.${artifact.version}.nupkg`);
      console.log(`  Downloading ${artifact.name} ${artifact.version} from ${feedHost}...`);
      await downloadFile(downloadUrl, nupkgPath);

      console.log("  Extracting...");
      const zip = new AdmZip(nupkgPath);
      const entries = nativeEntriesForRid(zip, platform.rid, platform.ext);
      if (entries.length === 0) {
        throw new Error(
          [
            `No native files found for RID '${platform.rid}' in ${artifact.name} ${artifact.version}.`,
            "The package may not yet support this platform.",
            "Set FOUNDRY_LOCAL_SKIP_INSTALL=1 to bypass if you are building from source.",
          ].join(" "),
        );
      }
      for (const entry of entries) {
        zip.extractEntryTo(entry, binDir, false, true);
        console.log(`    Extracted ${entryFileName(entry)}`);
      }
      return;
    } catch (err) {
      lastError = err;
      const reason = safeErrorMessage(err);
      if (i < feeds.length - 1) {
        console.warn(
          `  ${artifact.name} ${artifact.version}: download from ${feedHost} failed (${reason}); trying next feed...`,
        );
      }
    }
  }
  const feedHosts = feeds.map((f) => new URL(f).host).join(", ");
  const reason = safeErrorMessage(lastError);
  throw new Error(
    `Failed to download ${artifact.name} ${artifact.version} from any configured feed (${feedHosts}): ${reason}`,
  );
}

async function runHttpMode(config, artifacts, binDir, platform) {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "foundry-install-"));
  const serviceIndexCache = new Map();
  try {
    for (const artifact of artifacts) {
      await installPackageHttp(artifact, tempDir, binDir, config, platform, serviceIndexCache);
    }
  } finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
}

function escapeXml(value) {
  return String(value).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function generateRestoreProjectXml(artifacts) {
  const refs = artifacts
    .map((a) => `    <PackageReference Include="${escapeXml(a.name)}" Version="[${escapeXml(a.version)}]" />`)
    .join("\n");
  return `<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <IsPackable>false</IsPackable>
  </PropertyGroup>
  <ItemGroup>
${refs}
  </ItemGroup>
</Project>
`;
}

function buildDotnetRestoreArgs(config, { projectPath, packagesDir }) {
  const args = ["restore", projectPath, "--packages", packagesDir, "--no-cache"];
  if (config.configFile) {
    args.push("--configfile", config.configFile);
  } else {
    for (const feed of config.feeds) {
      args.push("--source", feed);
    }
  }
  return args;
}

function findRestoredPackageDir(packagesDir, id, version) {
  const dir = path.join(packagesDir, id.toLowerCase(), version.toLowerCase());
  if (!fs.existsSync(dir)) {
    throw new Error(`Restored package not found at expected path: ${dir}`);
  }
  return dir;
}

function collectNativeFilesFromPackageDir(packageDir, rid, ext) {
  const results = [];
  const nativeDir = path.join(packageDir, "runtimes", rid, "native");
  if (fs.existsSync(nativeDir)) {
    for (const name of fs.readdirSync(nativeDir)) {
      if (isNativeFileName(name, ext)) results.push(path.join(nativeDir, name));
    }
  }
  const runtimeDir = path.join(packageDir, "runtimes", rid);
  if (fs.existsSync(runtimeDir)) {
    for (const name of fs.readdirSync(runtimeDir)) {
      const full = path.join(runtimeDir, name);
      if (isNativeFileName(name, ext) && fs.statSync(full).isFile()) results.push(full);
    }
  }
  return results;
}

function runDotnetMode(config, artifacts, binDir, platform) {
  const missing = artifacts.filter((a) => !(a.expected && fs.existsSync(path.join(binDir, a.expected))));
  if (missing.length === 0) {
    console.log("  All expected native files already present, skipping dotnet restore.");
    return;
  }

  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "foundry-install-dotnet-"));
  try {
    const projectPath = path.join(tempDir, "restore.csproj");
    fs.writeFileSync(projectPath, generateRestoreProjectXml(missing));
    const packagesDir = path.join(tempDir, "packages");
    fs.mkdirSync(packagesDir, { recursive: true });

    const args = buildDotnetRestoreArgs(config, { projectPath, packagesDir });
    console.log("  Running dotnet restore...");
    const result = spawnSync(config.dotnetCommand, args, { encoding: "utf8", shell: false });

    if (result.error) {
      if (result.error.code === "ENOENT") {
        const cmd = config.dotnetCommand;
        throw new Error(
          `dotnet command not found: '${cmd}'. Install the .NET SDK or set FOUNDRY_LOCAL_DOTNET_COMMAND.`,
        );
      }
      throw result.error;
    }
    if (result.status !== 0) {
      const output = `${result.stdout ?? ""}\n${result.stderr ?? ""}`.trim();
      throw new Error(`dotnet restore failed (exit ${result.status}).\n${redactUrlsInText(output)}`.trim());
    }

    for (const artifact of missing) {
      const pkgDir = findRestoredPackageDir(packagesDir, artifact.name, artifact.version);
      const files = collectNativeFilesFromPackageDir(pkgDir, platform.rid, platform.ext);
      if (files.length === 0) {
        throw new Error(
          `No native files found for RID '${platform.rid}' in ${artifact.name} ${artifact.version} (dotnet restore).`,
        );
      }
      for (const file of files) {
        fs.copyFileSync(file, path.join(binDir, path.basename(file)));
        console.log(`    Staged ${path.basename(file)}`);
      }
    }
  } finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
}

function buildNugetInstallArgs(config, { id, version, outputDir }) {
  const args = [
    "install",
    id,
    "-Version",
    version,
    "-OutputDirectory",
    outputDir,
    "-NonInteractive",
    "-DirectDownload",
    "-DependencyVersion",
    "Ignore",
  ];
  if (config.configFile) {
    args.push("-ConfigFile", config.configFile);
  } else {
    for (const feed of config.feeds) {
      args.push("-Source", feed);
    }
  }
  return args;
}

// nuget.exe install writes `<id>.<version>` under outputDir, but the casing follows the
// package's nuspec id rather than what was passed on the command line. Scan only the
// immediate children of outputDir (no recursion) and match case-insensitively.
function findNugetPackageDir(outputDir, id, version) {
  const expected = `${id}.${version}`.toLowerCase();
  const match = fs
    .readdirSync(outputDir, { withFileTypes: true })
    .find((e) => e.isDirectory() && e.name.toLowerCase() === expected);
  if (!match) {
    throw new Error(`Restored package not found under ${outputDir} (expected ${id}.${version}).`);
  }
  return path.join(outputDir, match.name);
}

function runNugetMode(config, artifacts, binDir, platform) {
  const missing = artifacts.filter((a) => !(a.expected && fs.existsSync(path.join(binDir, a.expected))));
  if (missing.length === 0) {
    console.log("  All expected native files already present, skipping nuget install.");
    return;
  }

  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "foundry-install-nuget-"));
  try {
    const packagesDir = path.join(tempDir, "packages");
    fs.mkdirSync(packagesDir, { recursive: true });

    for (const artifact of missing) {
      const args = buildNugetInstallArgs(config, {
        id: artifact.name,
        version: artifact.version,
        outputDir: packagesDir,
      });
      console.log(`  Running nuget install for ${artifact.name} ${artifact.version}...`);
      const result = spawnSync(config.nugetCommand, args, { encoding: "utf8", shell: false });

      if (result.error) {
        if (result.error.code === "ENOENT") {
          const cmd = config.nugetCommand;
          throw new Error(
            `nuget command not found: '${cmd}'. Install the NuGet CLI or set FOUNDRY_LOCAL_NUGET_COMMAND.`,
          );
        }
        throw result.error;
      }
      if (result.status !== 0) {
        const output = `${result.stdout ?? ""}\n${result.stderr ?? ""}`.trim();
        const detail = redactUrlsInText(output);
        throw new Error(
          `nuget install failed for ${artifact.name} ${artifact.version} (exit ${result.status}).\n${detail}`.trim(),
        );
      }

      const pkgDir = findNugetPackageDir(packagesDir, artifact.name, artifact.version);
      const files = collectNativeFilesFromPackageDir(pkgDir, platform.rid, platform.ext);
      if (files.length === 0) {
        throw new Error(
          `No native files found for RID '${platform.rid}' in ${artifact.name} ${artifact.version} (nuget install).`,
        );
      }
      for (const file of files) {
        fs.copyFileSync(file, path.join(binDir, path.basename(file)));
        console.log(`    Staged ${path.basename(file)}`);
      }
    }
  } finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
}

// libfoundry_local records a versioned SONAME/install_name dependency on ORT
// (libonnxruntime.so.1 / libonnxruntime.1.dylib), but the vanilla ORT nupkg extracts
// the unversioned libonnxruntime.{so,dylib}. Rename the extracted file to the versioned
// soname so foundry_local resolves it via rpath. Windows uses onnxruntime.dll, which has
// no soname.
//
// On macOS also expose the unversioned libonnxruntime.dylib as a symlink to the versioned
// file. onnxruntime-genai references ORT under the unversioned name, and macOS dyld keys
// loaded images by path: with only the versioned file present, the unversioned reference
// resolves to a separate ORT image, and the two runtimes abort in ORT's global teardown
// on process exit. One physical file under both names keeps the process to a single ORT
// image. Linux's loader dedups by soname, so it needs only the versioned name.
function normalizeOrtLibName(binDir, ortVersion) {
  let unversioned;
  let versioned;
  if (os.platform() === "linux") {
    unversioned = path.join(binDir, "libonnxruntime.so");
    versioned = path.join(binDir, "libonnxruntime.so.1");
  } else if (os.platform() === "darwin") {
    const major = ortVersion.split(".")[0];
    unversioned = path.join(binDir, "libonnxruntime.dylib");
    versioned = path.join(binDir, `libonnxruntime.${major}.dylib`);
  } else {
    return;
  }
  if (!fs.existsSync(versioned) && fs.existsSync(unversioned)) {
    fs.renameSync(unversioned, versioned);
    console.log(`  Renamed ${path.basename(unversioned)} -> ${path.basename(versioned)}`);
  }
  if (os.platform() === "darwin" && fs.existsSync(versioned) && !fs.existsSync(unversioned)) {
    fs.symlinkSync(path.basename(versioned), unversioned);
    console.log(`  Linked ${path.basename(unversioned)} -> ${path.basename(versioned)}`);
  }
}

async function main() {
  if (process.env.FOUNDRY_LOCAL_SKIP_INSTALL === "1") {
    console.log("[foundry-local] FOUNDRY_LOCAL_SKIP_INSTALL=1 set; skipping native runtime download.");
    return 0;
  }

  const platform = detectPlatform();
  if (!platform) {
    console.warn(
      `[foundry-local] Unsupported platform: ${os.platform()}-${os.arch()}. Skipping native runtime install.`,
    );
    return 0;
  }

  const pkgRoot = path.resolve(__dirname, "..");
  const deps = loadDeps(pkgRoot);
  const artifacts = buildArtifacts(deps, platform);
  const binDir = path.join(pkgRoot, "prebuilds", platform.platformKey);
  const config = readConfig(process.env);

  console.log(
    `[foundry-local] Installing native runtime libraries for ${platform.rid} into ${binDir} (mode: ${config.mode})...`,
  );
  fs.mkdirSync(binDir, { recursive: true });

  if (config.mode === "http") {
    await runHttpMode(config, artifacts, binDir, platform);
  } else if (config.mode === "dotnet") {
    runDotnetMode(config, artifacts, binDir, platform);
  } else {
    runNugetMode(config, artifacts, binDir, platform);
  }

  normalizeOrtLibName(binDir, deps.onnxruntime.version);
  console.log("[foundry-local] Native runtime install complete.");
  return 0;
}

module.exports = {
  // config
  readConfig,
  DEFAULT_FEEDS,
  // url helpers
  redactUrl,
  redactUrlsInText,
  safeErrorMessage,
  // platform / artifacts
  detectPlatform,
  loadDeps,
  buildArtifacts,
  // http mode
  downloadWithRetryAndRedirects,
  nativeEntriesForRid,
  // dotnet mode
  generateRestoreProjectXml,
  buildDotnetRestoreArgs,
  findRestoredPackageDir,
  collectNativeFilesFromPackageDir,
  // nuget mode
  buildNugetInstallArgs,
  findNugetPackageDir,
  // shared
  normalizeOrtLibName,
  main,
};

if (require.main === module) {
  main()
    .then((code) => {
      process.exitCode = code ?? 0;
    })
    .catch((err) => {
      console.error("[foundry-local] Installation failed:", safeErrorMessage(err));
      process.exitCode = 1;
    });
}
