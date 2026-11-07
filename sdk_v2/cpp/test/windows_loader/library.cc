// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
extern "C" __declspec(dllimport) int FoundryLoaderDependency();
extern "C" __declspec(dllexport) int FoundryLoaderValue() { return FoundryLoaderDependency(); }
