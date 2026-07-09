// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Post-install / CI step that fetches ORT + ORT-GenAI native binaries from
// NuGet and stages them into sdk_v2/js/prebuilds/<plat>-<arch>/ next to
// foundry_local.{dll,so,dylib} and the .node addons.
//
// Ported from sdk/js/script/install-{standard,utils}.cjs. Differences:
//   * Targets sdk_v2/js/prebuilds/<plat>-<arch>/ (v2's single addon dir)
//     instead of v1's per-platform foundry-local-core/ subpackage layout.
//   * Does NOT download Microsoft.AI.Foundry.Local.Core — the v2 build/pack
//     pipeline ships foundry_local itself inside the .tgz prebuilds dir.
//   * Reads sdk_v2/deps_versions.json with the same dual-path fallback v1
//     uses (next to the script when published, two levels up in the repo).

'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const https = require('node:https');
const AdmZip = require('adm-zip');

if (process.env.FOUNDRY_LOCAL_SKIP_INSTALL === '1') {
    console.log('[foundry-local] FOUNDRY_LOCAL_SKIP_INSTALL=1 set; skipping native runtime download.');
    process.exit(0);
}

const PLATFORM_MAP = {
    'win32-x64': 'win-x64',
    'win32-arm64': 'win-arm64',
    'linux-x64': 'linux-x64',
    'linux-arm64': 'linux-arm64',
    'darwin-arm64': 'osx-arm64',
};
const platformKey = `${os.platform()}-${os.arch()}`;
const RID = PLATFORM_MAP[platformKey];

if (!RID) {
    console.warn(`[foundry-local] Unsupported platform: ${platformKey}. Skipping native runtime install.`);
    process.exit(0);
}

const EXT = os.platform() === 'win32' ? '.dll' : os.platform() === 'darwin' ? '.dylib' : '.so';
const LIB_PREFIX = os.platform() === 'win32' ? '' : 'lib';

const BIN_DIR = path.join(__dirname, '..', 'prebuilds', platformKey);

const depsPath = fs.existsSync(path.resolve(__dirname, '..', 'deps_versions.json'))
    ? path.resolve(__dirname, '..', 'deps_versions.json')
    : path.resolve(__dirname, '..', '..', 'deps_versions.json');

if (!fs.existsSync(depsPath)) {
    console.error(`[foundry-local] deps_versions.json not found at ${depsPath}`);
    process.exit(1);
}
const deps = JSON.parse(fs.readFileSync(depsPath, 'utf8'));

const isLinuxX64 = os.platform() === 'linux' && os.arch() === 'x64';
const ortPackageName = isLinuxX64 ? 'Microsoft.ML.OnnxRuntime.Gpu.Linux' : 'Microsoft.ML.OnnxRuntime.Foundry';

const ortVersion = deps.onnxruntime.version;
const genaiVersion = deps['onnxruntime-genai'].version;

// ORT's dylib/so soname uses the major version only (e.g. libonnxruntime.1.dylib,
// libonnxruntime.so.1), which is the name libfoundry_local records as its dependency.
const ortMajor = ortVersion.split('.')[0];

// Expected post-install filenames per platform. On Linux/macOS we rename or
// symlink the unversioned ORT lib to a versioned name to match what
// libfoundry_local.{so,dylib} actually requests at load time.
function expectedOrt() {
    if (os.platform() === 'linux') return 'libonnxruntime.so.1';
    if (os.platform() === 'darwin') return `libonnxruntime.${ortMajor}.dylib`;
    return 'onnxruntime.dll';
}
function expectedGenai() {
    return `${LIB_PREFIX}onnxruntime-genai${EXT}`;
}

const ARTIFACTS = [
    { name: ortPackageName, version: ortVersion, expected: expectedOrt() },
    { name: 'Microsoft.ML.OnnxRuntimeGenAI.Foundry', version: genaiVersion, expected: expectedGenai() },
];

const FEEDS = [
    'https://api.nuget.org/v3/index.json',
    'https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/nuget/v3/index.json',
];

async function downloadWithRetryAndRedirects(url, destStream = null) {
    const maxRedirects = 5;
    let currentUrl = url;
    let redirects = 0;

    while (redirects < maxRedirects) {
        const response = await new Promise((resolve, reject) => {
            https.get(currentUrl, (res) => resolve(res)).on('error', reject);
        });

        if (response.statusCode >= 300 && response.statusCode < 400 && response.headers.location) {
            currentUrl = response.headers.location;
            response.resume();
            redirects++;
            console.log(`  Following redirect to ${new URL(currentUrl).host}...`);
            continue;
        }

        if (response.statusCode !== 200) {
            throw new Error(`Download failed with status ${response.statusCode}: ${currentUrl}`);
        }

        if (destStream) {
            response.pipe(destStream);
            return new Promise((resolve, reject) => {
                destStream.on('finish', resolve);
                destStream.on('error', reject);
                response.on('error', reject);
            });
        }

        let data = '';
        response.on('data', (chunk) => (data += chunk));
        return new Promise((resolve, reject) => {
            response.on('end', () => resolve(data));
            response.on('error', reject);
        });
    }
    throw new Error('Too many redirects');
}

async function downloadJson(url) {
    return JSON.parse(await downloadWithRetryAndRedirects(url));
}

async function downloadFile(url, dest) {
    const file = fs.createWriteStream(dest);
    try {
        await downloadWithRetryAndRedirects(url, file);
        file.close();
    } catch (e) {
        file.close();
        if (fs.existsSync(dest)) fs.unlinkSync(dest);
        throw e;
    }
}

const serviceIndexCache = new Map();

async function getBaseAddress(feedUrl) {
    if (!serviceIndexCache.has(feedUrl)) {
        serviceIndexCache.set(feedUrl, await downloadJson(feedUrl));
    }
    const resources = serviceIndexCache.get(feedUrl).resources || [];
    const res = resources.find((r) => r['@type'] && r['@type'].startsWith('PackageBaseAddress/3.0.0'));
    if (!res) throw new Error('Could not find PackageBaseAddress/3.0.0 in NuGet feed.');
    const baseAddress = res['@id'];
    return baseAddress.endsWith('/') ? baseAddress : `${baseAddress}/`;
}

function entryFileName(entry) {
    const normalized = entry.entryName.replace(/\\/g, '/');
    return normalized.slice(normalized.lastIndexOf('/') + 1);
}

function nativeEntriesForRid(zip) {
    const nativePrefix = `runtimes/${RID}/native/`.toLowerCase();
    const runtimePrefix = `runtimes/${RID}/`.toLowerCase();
    return zip.getEntries().filter((e) => {
        const p = e.entryName.toLowerCase();
        if (!p.endsWith(EXT) && !/\.so(\.\d+)+$/.test(p)) {
            return false;
        }
        if (p.startsWith(nativePrefix)) return true;
        if (p.startsWith(runtimePrefix)) {
            const relative = p.slice(runtimePrefix.length);
            return relative.length > 0 && !relative.includes('/');
        }
        return false;
    });
}

async function installPackage(artifact, tempDir, binDir) {
    if (artifact.expected && fs.existsSync(path.join(binDir, artifact.expected))) {
        console.log(`  ${artifact.name}: ${artifact.expected} already present, skipping download.`);
        return;
    }

    let lastError;
    for (let i = 0; i < FEEDS.length; i++) {
        const feedUrl = FEEDS[i];
        const feedHost = new URL(feedUrl).host;
        try {
            const baseAddress = await getBaseAddress(feedUrl);
            const nameLower = artifact.name.toLowerCase();
            const verLower = artifact.version.toLowerCase();
            const downloadUrl = `${baseAddress}${nameLower}/${verLower}/${nameLower}.${verLower}.nupkg`;

            const nupkgPath = path.join(tempDir, `${artifact.name}.${artifact.version}.nupkg`);
            console.log(`  Downloading ${artifact.name} ${artifact.version} from ${feedHost}...`);
            await downloadFile(downloadUrl, nupkgPath);

            console.log('  Extracting...');
            const zip = new AdmZip(nupkgPath);
            const entries = nativeEntriesForRid(zip);
            if (entries.length === 0) {
                console.warn(`    No files found for RID ${RID} in ${artifact.name}.`);
                return;
            }
            for (const entry of entries) {
                zip.extractEntryTo(entry, binDir, false, true);
                console.log(`    Extracted ${entryFileName(entry)}`);
            }
            return;
        } catch (err) {
            lastError = err;
            const reason = err instanceof Error ? err.message : String(err);
            if (i < FEEDS.length - 1) {
                console.warn(`  ${artifact.name} ${artifact.version}: download from ${feedHost} failed (${reason}); trying next feed...`);
            }
        }
    }
    throw new Error(
        `Failed to download ${artifact.name} ${artifact.version} from any configured feed (${FEEDS.map((f) => new URL(f).host).join(', ')}): ${lastError instanceof Error ? lastError.message : lastError}`,
    );
}

// libfoundry_local records a versioned SONAME/install_name dependency on ORT
// (libonnxruntime.so.1 / libonnxruntime.1.dylib), but the Foundry ORT nupkg extracts
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
    if (os.platform() === 'linux') {
        unversioned = path.join(binDir, 'libonnxruntime.so');
        versioned = path.join(binDir, 'libonnxruntime.so.1');
    } else if (os.platform() === 'darwin') {
        const major = ortVersion.split('.')[0];
        unversioned = path.join(binDir, 'libonnxruntime.dylib');
        versioned = path.join(binDir, `libonnxruntime.${major}.dylib`);
    } else {
        return;
    }
    if (!fs.existsSync(versioned) && fs.existsSync(unversioned)) {
        fs.renameSync(unversioned, versioned);
        console.log(`  Renamed ${path.basename(unversioned)} -> ${path.basename(versioned)}`);
    }
    if (os.platform() === 'darwin' && fs.existsSync(versioned) && !fs.existsSync(unversioned)) {
        fs.symlinkSync(path.basename(versioned), unversioned);
        console.log(`  Linked ${path.basename(unversioned)} -> ${path.basename(versioned)}`);
    }
}

(async () => {
    console.log(`[foundry-local] Installing native runtime libraries for ${RID} into ${BIN_DIR}...`);
    fs.mkdirSync(BIN_DIR, { recursive: true });

    const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'foundry-install-'));
    try {
        for (const artifact of ARTIFACTS) {
            await installPackage(artifact, tempDir, BIN_DIR);
        }
        normalizeOrtLibName(BIN_DIR, ortVersion);
        console.log('[foundry-local] Native runtime install complete.');
    } catch (err) {
        console.error('[foundry-local] Installation failed:', err instanceof Error ? err.message : err);
        process.exit(1);
    } finally {
        try {
            fs.rmSync(tempDir, { recursive: true, force: true });
        } catch {}
    }
})();
