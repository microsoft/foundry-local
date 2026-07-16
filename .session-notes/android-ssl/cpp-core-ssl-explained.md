# How the Foundry Local C++ Core Does SSL/TLS (Especially on Android)

**Audience:** A software engineer who is comfortable with code but has *never* worked with
SSL/TLS, HTTPS internals, certificates, or how any of this behaves on Android. Everything is
built up from zero. By the end you should understand exactly what the C++ Core expects, why it
sometimes fails on Android with a confusing "self-signed certificate" error, and how the whole
trust machinery fits together.

---

## Table of Contents

1. [The one-sentence version](#1-the-one-sentence-version)
2. [What problem is SSL/TLS even solving?](#2-what-problem-is-ssltls-even-solving)
3. [Certificates, signing, and the chain of trust](#3-certificates-signing-and-the-chain-of-trust)
4. [The "trust store": your book of trusted stamps](#4-the-trust-store-your-book-of-trusted-stamps)
5. [Who actually does the work: libcurl vs OpenSSL](#5-who-actually-does-the-work-libcurl-vs-openssl)
6. [How the C++ Core makes an HTTPS request](#6-how-the-c-core-makes-an-https-request)
7. [Where the trust store comes from on a normal computer](#7-where-the-trust-store-comes-from-on-a-normal-computer)
8. [Why Android is different (and harder)](#8-why-android-is-different-and-harder)
9. [The full Android SSL flow, end to end](#9-the-full-android-ssl-flow-end-to-end)
10. [What can go wrong: the "self-signed certificate in chain" trap](#10-what-can-go-wrong-the-self-signed-certificate-in-chain-trap)
11. [How the Core reports SSL failures (and how we made it useful)](#11-how-the-core-reports-ssl-failures-and-how-we-made-it-useful)
12. [The mental model in one page](#12-the-mental-model-in-one-page)
13. [Glossary](#13-glossary)

---

## 1. The one-sentence version

The C++ Core makes HTTPS calls with **libcurl**, which uses a **statically bundled copy of OpenSSL**
to verify that the server is genuine; on Android there is **no file on disk** that OpenSSL can read
its list of trusted authorities from, so the app must **export Android's trust list into a `.pem`
file** and point the Core at it via the **`SSL_CERT_FILE`** environment variable — and if that file
is missing even a single required authority, every HTTPS call fails.

Everything below explains each noun in that sentence.

---

## 2. What problem is SSL/TLS even solving?

When your app talks to `https://ai.azure.com`, three things must be true:

1. **Secrecy** — nobody on the network (coffee-shop Wi-Fi, your ISP, a malicious router) can read
   the data.
2. **Integrity** — nobody can *modify* the data in transit without being detected.
3. **Authenticity** — you are *actually* talking to Azure, not an impostor who intercepted your
   connection.

The **"S" in HTTPS** is a protocol called **TLS** (older versions were called **SSL**; people still
say "SSL" out of habit). TLS handles all three. Secrecy and integrity are the "easy" parts (math:
encryption). **The hard, subtle part — and the one that breaks on Android — is #3, authenticity.**

The core question of authenticity is:

> A stranger on the internet claims to be `ai.azure.com`. **How do you know they're telling the
> truth?**

The rest of this document is really just the answer to that one question.

---

## 3. Certificates, signing, and the chain of trust

### The analogy: sealed letters and official stamps

Imagine you receive a sealed letter that says *"I am the government."* Anyone can print that. So the
letter also carries a **wax stamp** from an authority you already trust. You keep a **book of
trusted stamps**. If the letter's stamp matches one in your book → you believe it. If not → you
throw it away.

In TLS:

- The letter = a **certificate** (a small file the server sends you).
- The stamp = a **digital signature** on that certificate.
- The authority who stamped it = a **Certificate Authority (CA)**.
- Your book of trusted stamps = the **trust store** (a list of CA certificates you trust).

### A certificate is just a signed statement

A certificate says, roughly:

> "The website `ai.azure.com` owns this public key `<big number>`. — Signed, *some Certificate
> Authority*."

The **signature** is unforgeable: only the CA (who holds a secret private key) could have produced
it, and anyone can *verify* it using the CA's public key. So if you trust that CA, and the signature
checks out, you believe the statement.

### Why there's a *chain*, not a single stamp

Here's the wrinkle that causes 90% of real-world SSL pain, including ours.

CAs don't sign website certificates directly with their most precious key. That top-level key (the
**root**) is kept offline in a vault because if it were ever stolen, the entire internet's trust
collapses. Instead:

```
Root CA  (kept in a vault, rarely used)
   │  signs
   ▼
Intermediate CA  (the "working" CA, used day-to-day)
   │  signs
   ▼
Your website's certificate  (ai.azure.com)  ← this is called the "leaf"
```

So the server actually sends you a **chain** of certificates:

```
leaf (ai.azure.com)  ← signed by →  Intermediate  ← signed by →  Root
```

To trust the leaf, you verify each link:

1. Is the leaf validly signed by the Intermediate? (check signature)
2. Is the Intermediate validly signed by the Root? (check signature)
3. **Is the Root one that I already trust?** (look it up in my book of trusted stamps)

**Only step 3 requires pre-existing trust.** The leaf and intermediates are proven by math; the
root is proven by *"I already have a copy of this root in my trust store and I decided to trust
it."* This is the anchor of the whole system — hence the term **trust anchor**.

> **Key takeaway:** You don't need to pre-trust the website. You need to pre-trust the **root** at
> the top of its chain. If that root isn't in your book, verification fails — *even if everything
> else is perfect.*

### Real example: what `ai.azure.com` actually sends

This is the exact chain the Foundry catalog server presents (we captured it live):

```
depth 0:  eastus2.studio.ai.azure.com          ← leaf (the website)
depth 1:  Microsoft TLS G2 RSA CA OCSP 16       ← intermediate
depth 2:  Microsoft TLS RSA Root G2             ← (see the "cross-signed" note below)
depth 3:  DigiCert Global Root G2               ← self-signed root
```

"depth 0" is the website; each higher depth is who signed the one below. The top (highest depth) is
**self-signed** — it signs itself, which is what makes it a *root*. That self-signed cert is the one
your trust store must contain.

---

## 4. The "trust store": your book of trusted stamps

A **trust store** is simply a collection of root (and sometimes intermediate) CA certificates that
you've decided to trust in advance. Every OS ships with one, curated by the OS vendor:

- **Windows** keeps it in a system registry-backed store ("Certificate Manager").
- **macOS** keeps it in the "Keychain".
- **Linux** keeps it as a **file** (or folder of files), typically `/etc/ssl/certs/ca-certificates.crt`.
- **Android** keeps it in a special system store (more on this below) — **not** as a plain file the
  way desktop Linux does.

A trust store commonly has **~130–150 roots**. When your browser connects to any HTTPS site, it
walks the chain and checks the top root against this store. You never notice because it just works.

**The entire Android SSL problem in this project is: our Core cannot see Android's trust store the
normal way, so we have to hand it one as a file.** Sections 8–10 are all about that.

---

## 5. Who actually does the work: libcurl vs OpenSSL

Two libraries cooperate to make one HTTPS request. Keeping them straight is essential, because the
Android bug lives exactly at the seam between them.

### libcurl — the "make an HTTP request" library

**libcurl** is the workhorse for talking HTTP(S). You hand it a URL; it does DNS lookup, opens the
TCP connection, sends the request, and reads the response. libcurl itself does **not** implement the
cryptography — it delegates the TLS part to a crypto library.

> ⚠️ **Critical distinction that bit us:** there is the **`curl` command-line tool** (the thing you
> type in a terminal) and there is the **libcurl library** (linked into a program). They are *not*
> the same and they **read different settings**. Some environment variables (like
> `CURL_CA_BUNDLE`) are honored *only by the command-line tool*, and are silently ignored by the
> library. Our Core uses the **library**, so `CURL_CA_BUNDLE` does nothing for us.

### OpenSSL — the "cryptography + certificate verification" library

**OpenSSL** is the library that actually performs the TLS handshake math *and* does the certificate
chain verification from Section 3. When libcurl needs "verify this server's chain," it calls into
OpenSSL. OpenSSL is the component that ultimately answers *"is the root in the trust store?"*

### How they split the "where is the trust store?" decision

This split is the whole ballgame:

- libcurl decides **which file/folder to tell OpenSSL to load roots from**. It has a setting called
  **`CURLOPT_CAINFO`** ("the CA book is *this* file").
- If libcurl does **not** set an explicit file, OpenSSL falls back to **its own built-in default
  location**, and *that* default is influenced by the **`SSL_CERT_FILE`** environment variable.

So there are two different "who decides the trust file" paths:

| Path | Who sets it | How |
|------|-------------|-----|
| Explicit | libcurl | `CURLOPT_CAINFO = /path/to/bundle.pem` |
| Fallback | OpenSSL | `SSL_CERT_FILE=/path/to/bundle.pem` env var (used only if libcurl set no explicit file) |

**The Core does NOT set `CURLOPT_CAINFO`.** It relies on the fallback path — i.e., it expects
**`SSL_CERT_FILE`** to be set. Remember this; it's the contract.

---

## 6. How the C++ Core makes an HTTPS request

All of the Core's networking funnels through two files:

- `sdk_v2/cpp/src/http/http_client.cc` — general requests (GET/POST to the catalog, etc.)
- `sdk_v2/cpp/src/http/http_download.cc` — large file downloads (model blobs)

Both pick a **transport** based on the platform:

```cpp
#if defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)
  WinHttpTransport transport;   // desktop Windows only
#else
  CurlTransport transport;      // Android, Linux, macOS, UWP
#endif
```

- On **desktop Windows**, the Core uses **WinHTTP**, the OS's own HTTP stack. WinHTTP automatically
  uses the Windows system trust store. **No certificate configuration needed — it just works.** This
  is why the SSL problem is Android-specific: Windows never takes the libcurl/OpenSSL path.

- Everywhere else (**including Android**), the Core uses **`CurlTransport`** — that's libcurl +
  OpenSSL from Section 5. The transport is constructed with **default options**, meaning **no
  `CURLOPT_CAINFO` is set**, meaning we're on the **OpenSSL fallback path** that reads
  `SSL_CERT_FILE`.

`CurlTransport` here comes from the **Azure SDK for C++** (`azure-core`), which the Core uses as its
HTTP layer. Azure-core wraps libcurl; libcurl wraps OpenSSL. Three layers, one request.

> So the Core's SSL "expectation" is simply: **"Someone will set `SSL_CERT_FILE` to a valid PEM
> bundle that contains the roots for the servers I talk to."** On desktop that someone is the OS; on
> Android that someone has to be *us*.

---

## 7. Where the trust store comes from on a normal computer

On a Linux desktop or server, none of this is a problem, because:

- OpenSSL is compiled with a **baked-in default directory** (called `OPENSSLDIR`) such as
  `/etc/ssl`, and it looks for a bundle file like `/etc/ssl/cert.pem` there.
- That file exists, is maintained by the OS, and contains all ~140 roots.
- So even with no configuration, OpenSSL finds a full trust store and HTTPS "just works."

**Two subtle points that matter later:**

1. That baked-in `OPENSSLDIR` is decided **when OpenSSL is compiled**. Our Core's OpenSSL was
   compiled on a **Windows build machine** (via vcpkg), so its baked default points at a **Windows
   build-machine path** (something like `C:/.../msys2/.../etc/ssl/cert.pem`). That path is total
   nonsense on an Android phone. This is harmless *as long as* `SSL_CERT_FILE` is set — because
   `SSL_CERT_FILE`, when present, **overrides** the baked default. But if `SSL_CERT_FILE` is *not*
   set, OpenSSL tries the nonexistent Windows path and fails.

2. `OPENSSLDIR` (an **OpenSSL** compile default) and `CURLOPT_CAINFO` (a **libcurl** setting) are
   **different knobs**. Confusing the two is easy and led us down a wrong path early in debugging.
   For our build, what matters is the **`SSL_CERT_FILE`** override, and whether the file it points
   at actually contains the needed roots.

---

## 8. Why Android is different (and harder)

Android breaks the comfortable desktop assumptions in three ways.

### 8.1 Android's trust store is not a file OpenSSL can read

Android *does* have a trust store (~140 roots), but it's exposed to apps through a **Java/Android
API** called `AndroidCAStore` (accessed via `KeyStore.getInstance("AndroidCAStore")`), **not** as a
`/etc/ssl/cert.pem` file. Our statically-bundled OpenSSL knows nothing about Java or
`AndroidCAStore`; it only knows how to read a **file**. So out of the box, OpenSSL on Android has
**no trust store it can see** → every HTTPS verification fails.

### 8.2 OpenSSL is statically bundled into our `.so`

To ship a single self-contained native library, the Core links OpenSSL **statically** — the OpenSSL
code is copied *inside* `libfoundry_local.so` (this is configured by the vcpkg Android *triplets*,
which set static linkage + position-independent code). Consequences:

- There is **no separate `libssl.so`/`libcrypto.so`** on the device to swap or configure — the
  crypto is baked in. (We verified with the NDK's `llvm-readelf`: our `.so` has no dependency on
  system libssl/libcrypto.)
- The baked-in `OPENSSLDIR` is the useless Windows path from Section 7. So on Android we **must**
  provide a real trust file via `SSL_CERT_FILE`.

### 8.3 Only `SSL_CERT_FILE` works — not `SSL_CERT_DIR`

OpenSSL supports two ways to point at trusted roots:

- `SSL_CERT_FILE` → **one file** containing many concatenated certificates. ✅ Works on Android.
- `SSL_CERT_DIR` → a **folder** of individually-named cert files (using a special hashed-filename
  scheme). ❌ Does **not** work reliably with our statically-linked Android OpenSSL.

So the rule on Android is: **build one big `.pem` file and set `SSL_CERT_FILE` to it.** This is
documented in the repo (`sdk_v2/cpp/docs/AndroidBuildPlan.md`, "SSL/Certificate Handling") and
implemented in the Core's test harness (`sdk_v2/cpp/tools/android.py`), which builds a bundle by
concatenating the device's system certs before running tests.

### 8.4 (For debugging only) a cert sanity-checker

There's a small helper, `sdk_v2/cpp/src/platform/android/ssl_cert_checker.cc`, invoked from
`manager.cc` **only in debug builds on Android** (`#if defined(__ANDROID__) && !defined(NDEBUG)`).
It logs whether `SSL_CERT_FILE` is set, exists, and is non-empty. **Caution:** it inspects OpenSSL
directly and is a *coarse* sanity check — it can pass ("file looks fine") while real HTTPS still
fails because the file is missing a *specific required root* (exactly our bug). Don't treat a green
checkmark from it as proof that TLS will succeed.

---

## 9. The full Android SSL flow, end to end

Putting it together, here's the complete journey of trust from Android's OS into a verified HTTPS
connection inside the Core. (Steps 1–4 happen in the **Android app**, in Kotlin/Java; steps 5–7 are
inside the **C++ Core**.)

```
┌─────────────────────────── Android app (Kotlin) ───────────────────────────┐
│ 1. Read Android's trust store via KeyStore("AndroidCAStore")               │
│      → ~140 root CA certificates (Java objects)                            │
│                                                                            │
│ 2. Export each cert to text (PEM) and concatenate into one file:          │
│      /data/user/0/<app>/files/foundry_local_ca_bundle.pem                 │
│      (SslCertBundleHelper.kt does this; caches the file)                   │
│                                                                            │
│ 3. Tell the Core where the file is, by setting an environment variable    │
│    BEFORE loading/initializing the native library:                        │
│      Os.setenv("SSL_CERT_FILE", "<path to that .pem>", true)              │
│      (also set again from the JNI layer at Manager-create time)           │
└────────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼   (native library loads; makes a request)
┌─────────────────────────────── C++ Core ───────────────────────────────────┐
│ 4. http_client.cc builds a CurlTransport (default options, no CAINFO)     │
│ 5. libcurl connects to ai.azure.com and starts the TLS handshake          │
│ 6. Server sends its certificate CHAIN (leaf → intermediates → root)       │
│ 7. OpenSSL verifies the chain:                                            │
│      - checks each signature (math)                                       │
│      - loads trusted roots from the file named by SSL_CERT_FILE           │
│      - checks: is the chain's root in that file?                          │
│         YES → connection trusted ✅                                        │
│         NO  → "self-signed certificate in certificate chain" ❌            │
└────────────────────────────────────────────────────────────────────────────┘
```

**What the Core "expects" in one line:** by the time it makes its first HTTPS call, `SSL_CERT_FILE`
points to a readable PEM file that contains **every root needed** for the servers it will contact.
The Core does not fetch, build, or repair this file — that's the app's job.

---

## 10. What can go wrong: the "self-signed certificate in chain" trap

This is the exact failure we hit, and it's the most confusing one, so let's decode it fully.

### The error

libcurl reports `CURLE_PEER_FAILED_VERIFICATION`, and OpenSSL's underlying reason is
`X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN`, surfaced in our logs as:

```
SSL peer certificate ... was not OK. Underlying error: self-signed certificate in certificate chain
```

### What it does *not* mean

It does **not** mean the server is using a fake/self-signed certificate, and it does **not** mean
TLS is broken or misconfigured at the protocol level. The handshake actually *succeeded*; the
connection got all the way to **chain verification**.

### What it actually means

> OpenSSL walked up the server's chain, reached the top (a self-signed root), and **could not find a
> matching trusted copy of that root in the file named by `SSL_CERT_FILE`.** Lacking a trusted
> anchor, it treats the topmost cert as an untrusted self-signed certificate and rejects the whole
> chain.

In plain terms: **a required root (or a required cross-signed intermediate) is missing from your
bundle.**

### Our specific case: the cross-signed root gap

Recall `ai.azure.com`'s chain:

```
leaf → Microsoft TLS G2 RSA CA OCSP 16 → Microsoft TLS RSA Root G2 → DigiCert Global Root G2
```

We inspected the exported bundle and found:

- `DigiCert Global Root G2` — **present** ✅
- `Microsoft TLS RSA Root G2` — **missing** ❌

You'd think having `DigiCert Global Root G2` (the ultimate root) would be enough. It isn't, and this
is the subtle part worth understanding:

- `Microsoft TLS RSA Root G2` is a Microsoft-operated authority. To be trusted on devices that only
  ship DigiCert's root, it is **cross-signed** — i.e., DigiCert also signs a version of it. This
  creates two links from the same authority: one where it stands as its own root, and one where it
  chains down into DigiCert.
- For OpenSSL to succeed *via DigiCert*, it must have the **`Microsoft TLS RSA Root G2` certificate
  in hand** to form the link `... → Microsoft TLS RSA Root G2 → DigiCert Global Root G2`. That
  intermediate/root cert is exactly what was **missing** from the bundle.
- Android's own browser/TrustManager has `Microsoft TLS RSA Root G2` in its live store (newer root
  program), so the site loads fine in Chrome — but our **exported (and cached) `.pem` didn't include
  it**, so OpenSSL couldn't complete the path and failed.

### Why the bundle was missing it

Two contributing factors:

1. **Cross-signed anchors are easy to omit.** A naive "export the system roots" can miss a
   cross-signing intermediate/root that the platform treats specially.
2. **The bundle is cached.** `SslCertBundleHelper` only regenerates the `.pem` if it's absent
   (`if (certFile.exists() && length>0) return`). A bundle generated earlier (e.g., on an older
   image/emulator that predated the Microsoft root) sticks around until explicitly invalidated. The
   device log literally said `Using existing cert bundle`, i.e., a stale file.

### The fix

Add the missing `Microsoft TLS RSA Root G2` certificate to the bundle. Two ways:

- **Quick:** delete the cached `.pem` (or call `invalidateCache`) and regenerate on the real device,
  *if* the live device store contains the root.
- **Robust (recommended):** ship the required root(s) as an **app asset** and always **append** them
  to the exported bundle, de-duplicating by **SHA-256 fingerprint**. Then trust never depends on the
  device store being complete or the cache being fresh. For our endpoint that means bundling at least
  `Microsoft TLS RSA Root G2` (and keeping `DigiCert Global Root G2` for good measure).

> **General principle:** an app that talks to a *fixed, known* backend should **pin/ship the roots it
> needs** rather than trust that every device's store is complete and current. Device stores vary by
> OS version, OEM, and update state.

---

## 11. How the Core reports SSL failures (and how we made it useful)

Early on, this bug was nearly impossible to diagnose because the Core swallowed the real reason. It's
worth understanding the reporting path, because good diagnostics are half the fix.

### Where the real message is born

When libcurl/OpenSSL fail, the Azure transport throws a C++ exception. The Core catches it in
`http_client.cc` and captures the message:

```cpp
catch (const std::exception& e) {
  return HttpRawResult{0, std::string("transport error: ") + e.what(), {}};
}
```

- `status == 0` is the Core's convention for **"no HTTP response at all"** (a transport/TLS failure,
  as opposed to an HTTP 404/500 which *are* responses). The real curl/OpenSSL text lives in the
  `body` field.

### Where it used to get thrown away

The catalog talks to Azure through a **region-fallback** layer
(`sdk_v2/cpp/src/util/region_fallback.cc`) that tries multiple Azure regions in turn. When a region
failed, it summarized the failure with a helper that printed **only the status**, discarding the
`body`:

```cpp
// old behavior
status == 0 ? "transport failure" : "HTTP " + std::to_string(status);
```

So no matter the true cause — bad CA file, missing root, DNS failure, timeout — the logs and the
final exception all said the same opaque thing: **"transport failure."** The actual
`self-signed certificate in certificate chain` message never surfaced.

### The diagnostics fix we applied

We switched the region-fallback layer to use the existing body-preserving helper
`http::DescribeFailure(response)` (which appends the captured `body`, truncated) in **both** the
per-region warning log and the final aggregated error. After the change, the device logcat finally
showed the real reason:

```
RegionFallback: region 'centralus' unhealthy
  (transport failure: transport error: Fail to get a new connection for: https://ai.azure.com.
   SSL peer certificate ... was not OK. Underlying error: self-signed certificate in certificate chain);
  trying next candidate.
```

That single line is what turned "it just says transport failure" into a precise, fixable diagnosis.
(Note: these are **Warning**-level logs, so the app's log level must include warnings to see them.)

> **Lesson:** when an error crosses several abstraction layers (OpenSSL → libcurl → Azure transport →
> Core HTTP → region fallback), each layer must **carry the original message forward**, not replace
> it with a category label. The root cause is usually already in hand at the bottom; the bug is that
> upper layers throw it away.

---

## 12. The mental model in one page

- HTTPS needs to prove the server is genuine. It does this with a **certificate chain**:
  `leaf → intermediate(s) → root`. You must already trust the **root**.
- Your list of trusted roots is the **trust store**. Desktops get it from the OS automatically.
- The C++ Core uses **libcurl + statically-bundled OpenSSL** for HTTPS on everything except desktop
  Windows (which uses WinHTTP and needs no setup).
- The Core sets **no explicit CA file**; it relies on OpenSSL's fallback, which reads the
  **`SSL_CERT_FILE`** environment variable. **That is the Core's contract: "give me a PEM file of
  trusted roots via `SSL_CERT_FILE`."**
- On **Android**, OpenSSL can't read Android's Java-based trust store and its compiled-in default
  path is a useless Windows path. So the **app must export Android's roots into one `.pem` file and
  set `SSL_CERT_FILE`** before the Core runs. Use `SSL_CERT_FILE` (a single file), **not**
  `SSL_CERT_DIR`.
- If that file is **missing any required root or cross-signed anchor**, OpenSSL fails with
  **`self-signed certificate in certificate chain`** — which really means *"I couldn't find a trusted
  anchor for this chain,"* not *"the server is fake."*
- **Robust practice:** ship the roots your fixed backend needs as **app assets** and append them to
  the bundle (dedup by SHA-256), instead of trusting every device's store to be complete and fresh.
- **Diagnostics matter:** the Core now forwards the real curl/OpenSSL message through the
  region-fallback layer (`DescribeFailure`) instead of collapsing everything to "transport failure."

---

## 13. Glossary

- **TLS / SSL** — the protocol that secures HTTPS (encryption + integrity + server authentication).
  "SSL" is the old name; people use the terms interchangeably.
- **HTTPS** — HTTP carried inside a TLS-secured connection.
- **Certificate (cert)** — a signed file stating "this entity owns this public key."
- **Leaf certificate** — the server's own certificate (e.g., for `ai.azure.com`).
- **Intermediate CA** — a certificate authority that signs leaf certs, itself signed by a root.
- **Root CA** — the top, **self-signed** authority; the anchor of trust. Must be pre-trusted.
- **Certificate chain** — the ordered list `leaf → intermediate(s) → root` the server sends.
- **Trust anchor** — the root (or trusted cert) at the top of a chain that you already trust.
- **Trust store** — your collection of pre-trusted root CA certificates.
- **CA (Certificate Authority)** — an organization that issues/signs certificates (DigiCert,
  Microsoft, etc.).
- **Cross-signing** — when one CA signs another CA's certificate so it's trusted on devices that only
  have the first CA's root. Creates multiple valid paths for the same authority.
- **PEM** — a text encoding for certificates (Base64 between `-----BEGIN CERTIFICATE-----` /
  `-----END CERTIFICATE-----` lines). A "bundle" is many PEM certs concatenated in one file.
- **DER** — the raw binary form of a certificate (PEM is Base64-wrapped DER).
- **OpenSSL** — the library that does TLS crypto and certificate-chain verification.
- **libcurl** — the library that performs HTTP(S) requests; delegates TLS to OpenSSL. *Different from
  the `curl` command-line tool, which reads different settings.*
- **WinHTTP** — Windows' built-in HTTP stack; uses the Windows system trust store automatically. The
  Core uses it on desktop Windows instead of libcurl.
- **`SSL_CERT_FILE`** — environment variable naming a single PEM file of trusted roots for OpenSSL.
  **The Core's trust contract on Android.**
- **`SSL_CERT_DIR`** — environment variable naming a *folder* of hashed cert files. Does **not** work
  reliably with the statically-linked Android OpenSSL; avoid it.
- **`CURLOPT_CAINFO`** — a libcurl setting to explicitly name the CA file. The Core does **not** set
  it (it uses the `SSL_CERT_FILE` fallback instead).
- **`CURL_CA_BUNDLE`** — an environment variable honored **only by the `curl` command-line tool**,
  **not** by the libcurl library. Setting it has no effect on the Core.
- **`OPENSSLDIR`** — OpenSSL's compiled-in default directory for finding certs. In our build it points
  at a Windows build-machine path (useless on-device), which is why `SSL_CERT_FILE` is required.
- **`AndroidCAStore`** — the Java-side handle to Android's system trust store. Apps read it to export
  roots; native OpenSSL cannot read it directly.
- **Static linking** — copying a library's code *into* your binary (here, OpenSSL is baked into
  `libfoundry_local.so`), so there's no separate `.so` to configure on the device.
- **`CURLE_PEER_FAILED_VERIFICATION`** — libcurl's error code for "I couldn't verify the server's
  certificate."
- **`X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN`** — OpenSSL's specific reason: it reached a self-signed
  root it doesn't trust (usually: **a required root/anchor is missing from your bundle**).
- **Region fallback** — the Core's strategy of retrying the catalog request across multiple Azure
  regions; the layer that used to hide the real TLS error behind "transport failure."
- **NDK** — Android's Native Development Kit; provides the compiler/tools used to build the C++ Core
  for Android and to inspect the resulting `.so`.
