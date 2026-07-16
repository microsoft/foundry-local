# Building the Foundry Local C++ Core for Android — A Ground-Up Explanation

**Audience:** an experienced software engineer who has *never* worked with C/C++,
native build systems (CMake, vcpkg), or cross-compilation. If your background is
in managed/interpreted languages (C#, Java, Python, JavaScript, Go, Rust-via-cargo),
this document fills in the mental model you need to understand how
`libfoundry_local.so` is produced for Android.

The running example is the command:

```
python build.py --android --android_abi x86_64 --config Release
```

which cross-compiles the C++ SDK on a Windows machine into Android shared
libraries.

---

## Table of contents

1. [Why native builds feel alien](#1-why-native-builds-feel-alien)
2. [From source code to a running binary](#2-from-source-code-to-a-running-binary)
3. [Static vs. shared libraries](#3-static-vs-shared-libraries)
4. [Position-Independent Code (`-fPIC`)](#4-position-independent-code--fpic)
5. [Symbols and how the linker resolves them](#5-symbols-and-how-the-linker-resolves-them)
6. [Cross-compilation and the Android NDK](#6-cross-compilation-and-the-android-ndk)
7. [Android API levels](#7-android-api-levels)
8. [The `iconv` / Autotools story](#8-the-iconv--autotools-story)
9. [CMake: the build-system generator](#9-cmake-the-build-system-generator)
10. [vcpkg: the C++ package manager](#10-vcpkg-the-c-package-manager)
11. [Triplets and the overlay triplets in this repo](#11-triplets-and-the-overlay-triplets-in-this-repo)
12. [Toolchain files and "chain-loading"](#12-toolchain-files-and-chain-loading)
13. [Where ONNX Runtime and GenAI come from](#13-where-onnx-runtime-and-genai-come-from)
14. [End-to-end walkthrough of one build](#14-end-to-end-walkthrough-of-one-build)
15. [The output artifacts](#15-the-output-artifacts)
16. [Common failure modes](#16-common-failure-modes)
17. [Glossary](#17-glossary)

---

## 1. Why native builds feel alien

If you come from Java, C#, Python, or Node, a lot of machinery is *hidden* from
you:

- **Dependency management is automatic.** `npm install`, `pip install`, `dotnet
  restore`, and Maven download prebuilt, portable packages. They "just work" on
  any OS because they run on a virtual machine (JVM/CLR) or an interpreter.
- **There is one build target.** A `.jar`, a `.dll` assembly, or a Python wheel
  usually runs anywhere the runtime is installed. The bytecode is
  platform-neutral; the *runtime* absorbs the differences between Windows,
  Linux, macOS, x86, and ARM.

C and C++ have **none** of that safety net:

- There is **no virtual machine**. The compiler produces **raw machine code** for
  one specific CPU architecture and one specific operating system. Code compiled
  for Windows-x64 will not run on Android-arm64 — it is literally different CPU
  instructions calling different OS functions.
- There is **no built-in package manager**. For decades, getting a third-party
  library meant finding its source, compiling it yourself with the right flags,
  and manually telling your linker where it lives. vcpkg exists to automate this.
- The **build is a multi-tool pipeline**: a *generator* (CMake) produces build
  files, a *build tool* (Ninja/Make/MSBuild) runs the compiler and linker, a
  *package manager* (vcpkg) supplies dependencies, and a *toolchain* (the
  compiler + linker + system headers) does the actual work. Each is a separate
  program with its own configuration.

Everything below is really about making those four things agree on **one target**:
"Android, x86_64, API level 28, Release."

---

## 2. From source code to a running binary

A C++ program is built in two conceptual stages. Understanding this is the key to
everything else.

### Stage 1 — Compilation (one file at a time)

The compiler (here, `clang` from the Android NDK) takes **one** `.cc`/`.cpp`
source file and produces **one** *object file* (`.o`).

```
manager.cc   --clang-->   manager.o
model.cc     --clang-->   model.o
download.cc  --clang-->   download.o
```

An object file contains real machine code, but it is **incomplete**. Wherever
`manager.cc` calls a function that lives in another file (say `Download()` defined
in `download.cc`), the compiler cannot know that function's final address yet. So
it leaves a **placeholder** and records, in a table, "there is an unresolved
reference to a symbol named `Download` here."

Think of each `.o` as a puzzle piece: valid on its own, but with tabs and slots
that must connect to other pieces.

### Stage 2 — Linking (combine everything)

The **linker** takes all the `.o` files, plus any **libraries**, and:

1. Concatenates their machine code into one output.
2. **Resolves symbols**: for every "unresolved reference to `Download`," it finds
   the piece that *defines* `Download` and patches the placeholder with the real
   address.
3. Produces the final artifact — an executable or a library.

If a symbol is referenced but **never defined anywhere**, you get the infamous
`undefined reference to 'X'` **link error**. If a symbol is defined **twice**, you
get a `duplicate symbol` error. These two errors are the bread and butter of
native builds.

> **Mental model:** *Compiling* is translating each source file into machine code
> with holes. *Linking* is filling the holes by connecting all the pieces
> together.

---

## 3. Static vs. shared libraries

A **library** is just a bundle of already-compiled object files that you reuse
instead of rewriting (e.g. `curl` for HTTP, `openssl` for TLS, `libxml2` for XML
parsing). There are two fundamentally different ways to consume one.

### Static library — `.a` (Linux/Android) / `.lib` (Windows)

A static library is essentially an **archive of `.o` files** (the `.a` literally
stands for "archive"). When you link against it, the linker **copies the machine
code you use directly into your output binary**.

Consequences:
- The final binary is **self-contained** — it carries its dependencies inside it.
- The binary is **larger** (it physically contains the library code).
- At runtime there is **nothing external to find** — the code is already inside.

### Shared / dynamic library — `.so` (Linux/Android) / `.dll` (Windows) / `.dylib` (macOS)

A shared library stays a **separate file** on disk. Your binary does **not** copy
its code; instead it records a **dependency**: "at runtime, I will need
`libcurl.so`; please load it and connect my placeholders to it then."

- `.so` = "**shared object**."
- When the program starts, a part of the OS called the **dynamic linker/loader**
  finds each required `.so`, loads it into memory, and performs the
  symbol-resolution step that the static linker would otherwise have done at build
  time. This is called **dynamic linking**.
- Multiple programs can share **one** copy of the `.so` in memory — hence
  "shared."

### How this maps to Foundry Local

`libfoundry_local.so` **is** the C++ SDK, shipped as a **shared library** so that
the C#, Python, JavaScript, and Rust SDKs can all load the *same* native `.so` at
runtime (each language has a mechanism to call into a `.so`: P/Invoke in C#,
ctypes/cffi in Python, N-API in Node, etc.).

Now the important design decision in this repo:

> The SDK's **own dependencies** (curl, openssl, libxml2, azure-sdk, …) are built
> as **static** libraries and **bundled into** `libfoundry_local.so`.

So you ship **one** big `.so` (104 MB) instead of `libfoundry_local.so` **plus** a
dozen loose `libcurl.so`, `libssl.so`, `libxml2.so` files that would all have to
be located and loaded on the device. That bundling is exactly what the vcpkg
setting `VCPKG_LIBRARY_LINKAGE static` requests: "build my dependencies as static
libs so they get absorbed into my final shared library."

```
            +-------------------------------------------+
            |            libfoundry_local.so            |
            |  (a single shared library we ship)        |
            |                                           |
            |   [ SDK code ]                            |
            |   [ curl.a    ]  <-- statically bundled   |
            |   [ openssl.a ]  <-- statically bundled   |
            |   [ libxml2.a ]  <-- statically bundled   |
            |   [ azure-*.a ]  <-- statically bundled   |
            +-------------------------------------------+
                     ^
                     | loaded at runtime by C#, Python, JS, Rust...
```

(`libonnxruntime.so` and `libonnxruntime-genai.so` are the exception — they remain
separate `.so` files, because they are huge, prebuilt by Microsoft, and shipped
alongside. More on that in §13.)

---

## 4. Position-Independent Code (`-fPIC`)

This is the concept most people from managed languages have never had to think
about, because the JIT and OS handled it invisibly.

### The problem

When a program runs, its code and data live at memory **addresses**. Historically,
an executable was compiled assuming it would always be loaded at a **fixed** base
address — the machine code could contain hard-coded absolute addresses like "jump
to address 0x401000."

A **shared library** breaks that assumption. The **same** `.so` might be loaded:
- at a different address in every process that uses it, and
- at a different address on every run (modern OSes deliberately randomize load
  addresses for security — this is called **ASLR**, Address Space Layout
  Randomization).

If the `.so`'s code contained hard-coded absolute addresses, it would only work at
one specific load location and crash everywhere else.

### The solution: `-fPIC`

`-fPIC` is a **compiler flag** (`-f` = "flag"; `PIC` = **Position-Independent
Code**). It tells the compiler:

> Generate code that does **not** depend on being loaded at any particular
> address. Instead of hard-coded absolute addresses, use **relative** addressing
> ("jump 200 bytes forward from wherever I currently am") and an indirection table
> for global data.

Code compiled this way runs correctly **no matter where** the OS places it in
memory. That is a hard requirement for anything that ends up inside a `.so`.

### Why the repo forces `-fPIC` on the *static* dependencies

Normally, static libraries are compiled **without** PIC, because they were
designed to be linked into fixed-address executables. But recall the plan from §3:

> We take **static** dependency libraries and link them **into** the **shared**
> library `libfoundry_local.so`.

For that to be valid, the static libraries **must also be position-independent** —
otherwise you would be placing fixed-address code inside a relocatable `.so`, and
the linker will **refuse** (on x86_64 you typically get an error like *"relocation
R_X86_64_32 against '...' can not be used when making a shared object; recompile
with -fPIC"*).

So the triplet sets `CMAKE_POSITION_INDEPENDENT_CODE=ON` (which turns into `-fPIC`)
for every dependency vcpkg builds. That is the second half of the line:

```
VCPKG_LIBRARY_LINKAGE static  +  -fPIC
     |                              |
     |                              +-- compile the static libs as position-independent
     +-- build dependencies as static (.a) archives
     => so they can be legally and correctly bundled into libfoundry_local.so
```

---

## 5. Symbols and how the linker resolves them

A **symbol** is a named entry point or piece of data — a function name, a global
variable, etc. — recorded in an object file or library. The linker's core job is
**symbol resolution**: matching every *use* of a symbol to its single *definition*.

- **Defined (exported) symbol:** "I contain the code for `Download`."
- **Undefined (imported) symbol:** "I *call* `Download` but don't contain it —
  someone please provide it."

Linking succeeds only when **every** undefined symbol finds **exactly one**
definition among the object files and libraries provided.

For **shared** libraries there is an extra dimension: a `.so` publishes a list of
**exported symbols** — the public API other binaries may call at runtime. When C#
does a P/Invoke to `foundry_local`, it is asking the dynamic loader to find an
**exported symbol** (like `flManagerCreate`) inside `libfoundry_local.so` and call
it.

This is why the two classic link errors matter:
- **`undefined reference`** → you forgot to link a library that *defines* the
  symbol, or the dependency wasn't built.
- **`duplicate symbol`** → two libraries both define it; the linker can't choose.

---

## 6. Cross-compilation and the Android NDK

### Native compilation vs. cross-compilation

- **Native compilation:** you build **on** the same kind of machine you will
  **run** on. Compile on Windows-x64 → run on Windows-x64.
- **Cross-compilation:** you build **on** one platform but produce code **for a
  different** platform. Here: build **on Windows-x64**, produce code **for
  Android** (a different OS *and*, potentially, a different CPU).

Cross-compilation is normal for mobile and embedded work, because you don't
develop *on* the phone. But it introduces a crucial limitation:

> The build machine **cannot run** the code it produces. You can compile an
> Android binary on Windows, but you can't execute it there to test it.

Keep that limitation in mind — it is the root cause of the `iconv`/Autotools issue
in §8.

### The NDK (Native Development Kit)

To cross-compile for Android you need a **toolchain** that:
1. runs on your host OS (Windows), but
2. emits machine code + calls for **Android**.

That toolchain is the **Android NDK**. It bundles:
- **`clang`** — the C/C++ compiler, configured to target Android.
- **`lld`** — the linker.
- **Android system headers and stub libraries** — the declarations of the OS
  functions your code may call on Android (e.g. Android's C library, "Bionic").
- **`android.toolchain.cmake`** — a CMake **toolchain file** (see §12) that
  configures CMake to use all of the above with the right flags for a chosen ABI
  and API level.

In this repo the NDK is located via the environment variable `ANDROID_NDK_HOME`
(for example `.../Android/Sdk/ndk/29.0.14206865`). If that variable is unset, the
build cannot find the cross-compiler and fails immediately — which is exactly why
the first x86_64 attempt failed until `ANDROID_NDK_HOME` (and `VCPKG_ROOT`) were
set.

### ABI = Application Binary Interface

An **ABI** specifies the low-level contract for a target: the CPU instruction set,
how function arguments are passed in registers, struct layout, etc. Android's two
relevant ABIs here:

- **`arm64-v8a`** — 64-bit ARM. What real phones and tablets use.
- **`x86_64`** — 64-bit Intel/AMD. Used by the **Android emulator** running on a
  PC (which has an x86_64 CPU), so the emulator doesn't have to slowly emulate ARM
  instructions.

That's why x86_64 is described as "emulator-only" and arm64-v8a as "for devices."

---

## 7. Android API levels

Android ships in versions, and each is assigned an integer **API level**:

| API level | Android version |
|-----------|-----------------|
| 28        | 9.0 "Pie"       |
| 30        | 11              |
| 34        | 14              |

When you **target** an API level (here `ANDROID_PLATFORM=android-28`, and the
triplet's `VCPKG_CMAKE_SYSTEM_VERSION 28`), you are telling the compiler and
linker:

> Assume the device runs **at least** this Android version, so **these** system
> functions and libraries are guaranteed to be present.

- Choose a **low** API level → your `.so` runs on more (older) devices, but you can
  only rely on older system features.
- Choose a **high** API level → newer OS features are guaranteed available, but you
  drop support for older devices.

API level directly affects **which functions the NDK will let you call**, because
some Android system functions only appeared in certain versions. That is central
to the next section.

---

## 8. The `iconv` / Autotools story

This explains the second cryptic line:

> `VCPKG_CMAKE_SYSTEM_VERSION 28` — API 28 so the NDK supplies `iconv`, dodging
> autotools libiconv cross-compile pain.

### What is `iconv`?

`iconv` ("internationalization conversion") is a small, standard C library for
converting text between **character encodings** — e.g. UTF-8 ↔ UTF-16 ↔
Latin-1. One of the SDK's transitive dependencies, **`libxml2`** (an XML parser),
needs `iconv` to handle documents in various encodings.

So the build **must** provide an `iconv` implementation for Android. There are two
ways to do that:

### Option A — use the NDK's built-in `iconv` (what we do)

The Android C library (**Bionic**) gained a built-in `iconv` implementation
starting at **API level 28**. If you target API 28+, the NDK simply **hands you
`iconv`** — no extra library to build. Clean and reliable.

### Option B — compile the standalone `libiconv` from source (what we avoid)

If you targeted an API level **below 28**, Bionic wouldn't provide `iconv`, so
vcpkg would have to **download and compile the standalone `libiconv` package**.
And `libiconv` uses an old build system called **Autotools** — and Autotools does
not play well with cross-compilation. Here's why:

#### What is Autotools, and why does it break when cross-compiling?

**Autotools** (the `./configure && make` scripts you may have seen in old C
projects) predates CMake. To adapt to whatever machine it's building on, its
`configure` step **compiles tiny test programs, runs them, and inspects the
result** — e.g. "compile a program that prints `sizeof(long)`; run it; read the
number." This is called a **run test**.

That works fine for *native* builds. But in **cross-compilation** you are building
Android binaries on a Windows host — and, as established in §6, **the host cannot
run Android binaries**. So every one of Autotools' "compile-and-run" probes either:
- fails outright, or
- forces you to hand-feed the "right answers" via a pile of `cache variables`,
  which is fragile and version-specific.

The result is exactly the "cross-compile pain": confusing configure-time failures
that have nothing to do with your actual code.

### The decision

By pinning the target to **API 28**, the build takes **Option A**: it uses the
NDK's ready-made `iconv` and **never compiles `libiconv` at all**, sidestepping the
whole Autotools-cross-compile mess. That single number in the triplet
(`VCPKG_CMAKE_SYSTEM_VERSION 28`) is what makes that possible.

> **Takeaway:** the API level isn't just "which phones we support" — it also
> decides **which libraries the NDK gives us for free**, which in turn lets us
> avoid building fragile dependencies ourselves.

---

## 9. CMake: the build-system generator

C++ has no single standard build tool. Different platforms historically used
different ones: **MSBuild/Visual Studio** on Windows, **Make** or **Ninja** on
Linux, **Xcode** on macOS. Writing and maintaining a separate build script for
each is miserable.

**CMake** solves this by being a **build-system generator**. You describe your
project **once**, declaratively, in `CMakeLists.txt` files ("this target is a
shared library built from these sources; it depends on curl and openssl"). Then
CMake **generates** the native build files for whatever tool you choose:

```
CMakeLists.txt  --CMake-->  Visual Studio solution   (Windows default)
                --CMake-->  Ninja build files         (used for Android here)
                --CMake-->  Unix Makefiles
```

Two distinct phases:

1. **Configure/generate:** CMake reads your `CMakeLists.txt`, runs any `find_*`
   logic (e.g. locating ONNX Runtime — see §13), figures out compiler/flags, and
   emits build files for the chosen **generator**.
2. **Build:** the generator's tool (here **Ninja**) actually invokes the compiler
   and linker to produce the binaries.

### Why Ninja for Android?

**Ninja** is a very fast, minimalist build tool. The Android NDK's CMake
integration is designed and tested primarily with Ninja; the Visual Studio
generator can't drive the NDK cross-compiler cleanly. That's why `build.py`
**forces** the generator to Ninja whenever `--android` is passed.

### Key CMake terms you'll see

- **`CMAKE_TOOLCHAIN_FILE`** — a script that tells CMake *which compiler/linker to
  use and how* (see §12). For cross-compiling this is essential.
- **`CMAKE_BUILD_TYPE`** (`Debug` / `Release` / `RelWithDebInfo`) — optimization &
  debug-info profile. `Release` = optimized, no debug symbols; `Debug` =
  unoptimized, full symbols; `RelWithDebInfo` = optimized *with* symbols.
- **Target** — a thing CMake builds: an executable or a library.
- **`find_package` / `Find<X>.cmake`** — logic that locates a dependency and tells
  CMake how to link it.

---

## 10. vcpkg: the C++ package manager

Because C++ has no built-in package manager, **vcpkg** (from Microsoft) fills the
role that `npm`/`pip`/`nuget`/`cargo` play elsewhere — with one big twist.

### The twist: it builds from source, per-target

`npm` and `pip` mostly download **prebuilt, portable** packages. vcpkg instead
**downloads dependency source code and compiles it** for your **exact** target
(OS + CPU + linkage + compiler). It has to, because — as we've seen — native code
is not portable across platforms. There is no single "curl package" that works on
Windows-x64, Android-arm64, and Android-x86_64; each must be **compiled
separately**.

That's why the first Android build was slow: vcpkg was compiling curl, openssl,
libxml2, the Azure SDK, etc. **from source** for `x64-android`. It caches the
results, so later builds are fast.

### How vcpkg is wired into this project

- **`vcpkg.json`** (a *manifest*) declares the dependencies and optional
  **features**:
  ```jsonc
  {
    "dependencies": [ "azure-storage-blobs-cpp", "ms-gsl",
                      "nlohmann-json", "spdlog", ... ],
    "features": {
      "tests":   { "dependencies": [ "gtest", ... ] },
      "service": { "dependencies": [ "oatpp" ] }   // web server; excluded on Android
    }
  }
  ```
  This is the equivalent of `package.json` / `requirements.txt`.
- **`VCPKG_ROOT`** is an environment variable pointing at your vcpkg installation
  (e.g. `C:\projects\vcpkg`). If it's unset, the build can't find vcpkg and stops.
  (This was the *other* missing variable in the failed first attempt.)
- vcpkg integrates with CMake through a **toolchain file** (`vcpkg.cmake`) so that
  CMake's `find_package(curl)` automatically finds the vcpkg-built curl.

---

## 11. Triplets and the overlay triplets in this repo

A **triplet** is vcpkg's name for a **build target profile**. It answers: "for
*which* platform, *which* architecture, *which* linkage, and *how*, should I build
these packages?" Examples:

- `x64-windows`
- `arm64-android`
- `x64-android`

By default, `x64-android` (a built-in triplet) builds **shared** libraries. But
this project needs **static, position-independent** libraries so they can be
bundled into `libfoundry_local.so` (§3–§4). To override the defaults, the repo
ships **overlay triplets** — custom triplet files in `sdk_v2/cpp/triplets/` that
CMake is told to prefer via `VCPKG_OVERLAY_TRIPLETS`.

Here is the actual `x64-android` overlay triplet, annotated:

```cmake
# sdk_v2/cpp/triplets/x64-android.cmake

set(VCPKG_TARGET_ARCHITECTURE x64)          # target CPU: 64-bit x86
set(VCPKG_CRT_LINKAGE dynamic)              # link the C runtime dynamically
set(VCPKG_LIBRARY_LINKAGE static)           # >>> build deps as STATIC (.a) <<<
set(VCPKG_CMAKE_SYSTEM_NAME Android)        # target OS is Android
set(VCPKG_CMAKE_SYSTEM_VERSION 28)          # >>> API 28 -> NDK provides iconv <<<

set(VCPKG_HOST_TRIPLET x64-windows)         # we build ON Windows (cross-compile)

set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"  # >>> -fPIC on the static deps <<<
    "-DANDROID_ABI=x86_64"                  # the Android ABI
    "-DANDROID_PLATFORM=android-28")        # the Android API level

# Make vcpkg's own dependency builds use the SAME NDK compiler as the main project:
if(DEFINED ENV{ANDROID_NDK_HOME})
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
        "$ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake")
endif()
```

Every cryptic phrase from earlier now maps to a concrete line:

| Earlier phrase | Triplet line |
|----------------|--------------|
| "deps build as static … bundled into `libfoundry_local.so`" | `VCPKG_LIBRARY_LINKAGE static` |
| "PIC libs" (`-fPIC`) | `-DCMAKE_POSITION_INDEPENDENT_CODE=ON` |
| "API 28 so the NDK supplies `iconv`" | `VCPKG_CMAKE_SYSTEM_VERSION 28` |
| "cross-compile on Windows" | `VCPKG_HOST_TRIPLET x64-windows` |
| "use the same NDK" | `VCPKG_CHAINLOAD_TOOLCHAIN_FILE …` |

The `arm64-android.cmake` triplet is identical except the architecture is `arm64`
and the ABI is `arm64-v8a`.

---

## 12. Toolchain files and "chain-loading"

A **CMake toolchain file** is a script that tells CMake **which compiler and
linker to use and how to invoke them**. For a *native* build CMake auto-detects
this. For *cross*-compilation you must supply it explicitly, because CMake has no
way to guess "use the Android NDK's clang targeting x86_64 at API 28."

In this build there are **two** toolchain files that must cooperate:

1. **vcpkg's toolchain** (`vcpkg.cmake`) — makes `find_package` locate
   vcpkg-built dependencies.
2. **the NDK's toolchain** (`android.toolchain.cmake`) — makes the compiler target
   Android with the right ABI/API.

You can only pass CMake **one** primary `CMAKE_TOOLCHAIN_FILE`. So the pattern is
**chain-loading**: you give CMake vcpkg's toolchain as the primary, and tell vcpkg
to **chain-load** (delegate to) the NDK toolchain underneath. The variable
`VCPKG_CHAINLOAD_TOOLCHAIN_FILE` is exactly that instruction.

The subtlety the code comments call out: this chain-loading must happen in **two
places**, because there are **two separate compilations** going on:

- **The vcpkg dependency builds** (curl, openssl, …) — chain-loading is set
  **inside the triplet** (last block of §11).
- **The top-level Foundry Local project** — chain-loading is passed on the CMake
  **command line** by `build.py` (`-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=...`).

If only one of the two were configured, half the build would use the NDK
cross-compiler and half would try to use the host compiler — and nothing would
link. Getting **both** to point at the **same** NDK is the whole reason
`build.py` also exports `ANDROID_NDK_HOME` into the child process's environment.

---

## 13. Where ONNX Runtime and GenAI come from

Two dependencies are **not** built by vcpkg and **not** bundled statically:

- **ONNX Runtime** (`libonnxruntime.so`) — the ML inference engine.
- **ONNX Runtime GenAI** (`libonnxruntime-genai.so`) — the generative-AI layer on
  top of it.

These are enormous, and Microsoft already publishes **prebuilt** Android binaries.
So instead of compiling them, the CMake `Find*.cmake` modules **download prebuilt
packages and extract the `.so` for the target ABI**:

- **ONNX Runtime** ships inside a NuGet package that contains an **AAR**. (An
  **AAR** is an "Android Archive" — a ZIP-format bundle of native `.so` files under
  `jni/<abi>/`, used by the Android ecosystem.) The build extracts
  `jni/x86_64/libonnxruntime.so` (or `jni/arm64-v8a/...` for ARM).
- **GenAI** publishes a standalone `.aar` on its GitHub Releases page; the build
  downloads it and extracts `jni/<abi>/libonnxruntime-genai.so`.

The ABI selection is explicit in the CMake logic:

```cmake
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(_ORT_PLATFORM "android-arm64")
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(_ORT_PLATFORM "android-x64")
else()
    message(FATAL_ERROR "Unsupported Android ABI: ${ANDROID_ABI}")
endif()
```

These two `.so` files are shipped **next to** `libfoundry_local.so` (they stay
separate rather than being bundled in, because they're prebuilt and huge).

---

## 14. End-to-end walkthrough of one build

Command:

```
python build.py --android --android_abi x86_64 --config Release
```

Step by step:

1. **`build.py` prepares the configuration.**
   - Forces the generator to **Ninja** (NDK requirement).
   - Maps ABI → triplet: `x86_64` → `x64-android`.
   - Resolves the NDK from `ANDROID_NDK_HOME`; verifies `android.toolchain.cmake`
     exists.
   - Locates `ninja` on `PATH`; requires `VCPKG_ROOT` to be set.

2. **CMake configure phase.**
   - Primary toolchain = vcpkg's `vcpkg.cmake`; it chain-loads the NDK toolchain.
   - vcpkg reads `vcpkg.json` and, using the **overlay triplet** `x64-android`,
     **compiles every dependency from source** as **static + PIC** libraries for
     Android-x86_64-API28. (Slow the first time; cached afterward.)
   - The `Find*.cmake` modules **download and extract** the ONNX Runtime and GenAI
     Android `.so` files for `x86_64`.
   - CMake emits Ninja build files under `build\Android-x86_64\Release\`.

3. **Ninja build phase.**
   - Compiles each SDK `.cc` into a `.o` using the NDK clang.
   - **Links** `libfoundry_local.so`, absorbing all the static+PIC dependency
     archives into it.
   - Builds the test executables (`foundry_local_tests`, `sdk_integration_tests`,
     …). `153/153` steps = complete.

4. **Output** lands in `build\Android-x86_64\Release\bin\`.

---

## 15. The output artifacts

Full paths for the x86_64 build:

```
C:\projects\github\Foundry-Local\sdk_v2\cpp\build\Android-x86_64\Release\bin\libfoundry_local.so
C:\projects\github\Foundry-Local\sdk_v2\cpp\build\Android-x86_64\Release\bin\libonnxruntime-genai.so
C:\projects\github\Foundry-Local\sdk_v2\cpp\build\Android-x86_64\Release\bin\libonnxruntime.so
```

And for arm64 (real devices):

```
C:\projects\github\Foundry-Local\sdk_v2\cpp\build\Android-arm64-v8a\Release\bin\libfoundry_local.so
C:\projects\github\Foundry-Local\sdk_v2\cpp\build\Android-arm64-v8a\Release\bin\libonnxruntime-genai.so
C:\projects\github\Foundry-Local\sdk_v2\cpp\build\Android-arm64-v8a\Release\bin\libonnxruntime.so
```

- `libfoundry_local.so` — the SDK, with curl/openssl/libxml2/azure statically
  bundled inside (that's why it's ~104 MB).
- `libonnxruntime.so`, `libonnxruntime-genai.so` — prebuilt runtime libraries,
  shipped alongside.

The build directory is **platform-segmented** (`Android-x86_64` vs.
`Android-arm64-v8a`) so the two ABIs never overwrite each other.

---

## 16. Common failure modes

- **`vcpkg root not found`** → `VCPKG_ROOT` env var not set. Point it at your
  vcpkg checkout (e.g. `C:\projects\vcpkg`). *This was the first failure you hit.*
- **NDK not found / toolchain file missing** → `ANDROID_NDK_HOME` unset or pointing
  at the wrong folder. It must be the NDK **root** (contains
  `build/cmake/android.toolchain.cmake`). *This was the second missing piece.*
- **`ninja not found on PATH`** → install Ninja (`pip install ninja`) or add it to
  `PATH`; Android builds require the Ninja generator.
- **`relocation … can not be used when making a shared object; recompile with
  -fPIC`** → a static dependency was built without PIC. In this repo the overlay
  triplet's `CMAKE_POSITION_INDEPENDENT_CODE=ON` prevents this.
- **`undefined reference to '…'`** → a library that *defines* the symbol wasn't
  linked, or a dependency failed to build.
- **`Unsupported Android ABI`** → you passed an ABI other than `arm64-v8a` or
  `x86_64`; only those two are wired up for the ORT/GenAI downloads.
- **Trying to run tests on the host for an Android build** → you can't; Android
  binaries don't run on Windows. Use `--android_run_emulator --android_abi x86_64`
  to run them on an emulator.

---

## 17. Glossary

- **ABI (Application Binary Interface)** — the low-level contract (CPU
  instructions, calling convention, struct layout) for a target. e.g. `arm64-v8a`,
  `x86_64`.
- **AAR (Android Archive)** — a ZIP bundle of Android native libraries (`.so`
  under `jni/<abi>/`) and metadata.
- **ASLR (Address Space Layout Randomization)** — OS security feature that loads
  code at randomized addresses; requires position-independent code in shared libs.
- **Autotools** — an older `./configure && make` build system that probes the host
  by compiling and **running** test programs — which breaks under
  cross-compilation.
- **Bionic** — Android's implementation of the C standard library.
- **CMake** — a build-system *generator*: describe the project once, generate
  native build files (Ninja/Make/VS) from it.
- **Compile** — translate one source file into an object file (`.o`) of machine
  code with unresolved placeholders.
- **Cross-compilation** — build on one platform for a different platform; the build
  host cannot run the produced binaries.
- **Dynamic linker/loader** — the OS component that loads `.so`/`.dll` files at
  runtime and resolves symbols.
- **`-fPIC` / Position-Independent Code** — code that works regardless of its load
  address; mandatory for anything placed in a shared library.
- **Link** — combine object files + libraries, resolve symbols, produce the final
  executable/library.
- **NDK (Native Development Kit)** — Android's C/C++ cross-compiler toolchain
  (clang, linker, headers, CMake toolchain file).
- **Ninja** — a fast, minimal build tool; the generator used for Android builds
  here.
- **Object file (`.o`)** — compiled machine code from one source file, not yet
  linked.
- **Shared library (`.so`/`.dll`/`.dylib`)** — a library kept as a separate file
  and loaded at runtime (dynamic linking).
- **Static library (`.a`/`.lib`)** — an archive of object files copied into your
  binary at link time.
- **Symbol** — a named function or global variable tracked by the compiler/linker
  for resolution.
- **Toolchain file** — a CMake script specifying which compiler/linker to use and
  how; essential for cross-compilation.
- **Triplet (vcpkg)** — a build-target profile (arch + OS + linkage + options),
  e.g. `x64-android`.
- **vcpkg** — Microsoft's C/C++ package manager; builds dependencies from source
  per target.
- **`VCPKG_ROOT` / `ANDROID_NDK_HOME`** — environment variables locating vcpkg and
  the NDK, respectively; both must be set for this build.
```
