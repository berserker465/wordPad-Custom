# WordPad(Custom)

WordPad(Custom) is a customized Windows rich-text editor derived from the open-source RWordPad / ReactOS WordPad implementation. This directory contains the application source code, resources, translations, and build configuration.

## Features

The customized editor includes additional text-editing commands, extended find-and-replace tools, bookmark support, and Google Drive cloud backup functionality, among other enhancements. Specific features and interfaces may change as development continues.

## Building

This is Windows-oriented C code. The repository contains `CMakeLists.txt` and a `Build_Cloud_Beta.cmd` helper, which expects CMake and Ninja to be available in a suitable Windows build environment (such as a configured ReactOS Build Environment / RosBE command prompt).

Example, from this directory in a properly configured build environment:

```cmd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Build requirements and compatibility depend on the selected Windows toolchain. The commands above have not been re-tested as part of this source-publication preparation. The `cloud_beta/` directory contains an earlier PowerShell-based Google Drive upload proof of concept; the native cloud implementation is in `cloud_native.c` and `cloud_native.h`.

## Licensing and upstream attribution

WordPad(Custom) includes code derived from RWordPad / ReactOS WordPad. The source files retain the original contributors' copyright notices and LGPL licensing notices. The accompanying `LICENSE.txt` contains the **GNU Lesser General Public License, version 2.1**; the source headers specify **LGPL version 2.1 or (at your option) any later version**.

Retain copyright notices and comply with applicable upstream licensing requirements when redistributing or modifying the code. The source is distributed without warranty, as described in its license.

## Repository layout

The application's source code is deliberately kept under `Source Code/` in the GitHub repository so the existing top-level OAuth policy files (`Overview`, `privacy`, and `terms`) remain untouched.

## Privacy and credentials

OAuth client configuration and Google Drive credentials are intended to be supplied by the user at runtime. Do **not** commit local credential exports, access tokens, refresh tokens, personal backup scripts, or generated build directories.
