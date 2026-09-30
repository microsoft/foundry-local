# Copyright (c) Microsoft. All rights reserved.

set(FOUNDRY_LOCAL_PACKAGE_VERSION "" CACHE STRING
    "Explicit package version returned by FoundryLocalGetVersionString()")

# Remove the former cache variable so an existing build directory cannot keep
# reporting an SDK version from an earlier configure.
unset(FOUNDRY_LOCAL_VERSION_STRING CACHE)

if(FOUNDRY_LOCAL_PACKAGE_VERSION)
    set(FOUNDRY_LOCAL_VERSION_STRING "${FOUNDRY_LOCAL_PACKAGE_VERSION}")
else()
    set(FOUNDRY_LOCAL_VERSION_STRING "${PROJECT_VERSION}")
endif()