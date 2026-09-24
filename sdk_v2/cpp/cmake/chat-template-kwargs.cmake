# Copyright (c) Microsoft. All rights reserved.

set(FOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS "AUTO" CACHE STRING
    "Tokenizer kwargs capability: AUTO detects packaged GenAI; ON/OFF explicitly selects support")
set_property(CACHE FOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${FOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS}" _kwargs_mode)

set(FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS OFF)
if(_kwargs_mode STREQUAL "ON")
    set(FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS ON)
elseif(_kwargs_mode STREQUAL "AUTO")
    # UpdateOptions exists in older headers too; the bundled tokenizer determines support, not a C++ symbol.
    # A cached package version cannot describe a caller-provided local build.
    if(ORT_GENAI_HOME)
        message(STATUS
            "Local ORT GenAI chat template kwargs capability is unknown. "
            "Set FOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS=ON if the local tokenizer supports it.")
    elseif(ORT_GENAI_VERSION MATCHES "^0\\.16\\.0-dev([0-9]+)$")
        if(CMAKE_MATCH_1 GREATER_EQUAL 1001407373)
            set(FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS ON)
        endif()
    elseif(ORT_GENAI_VERSION AND NOT ORT_GENAI_VERSION VERSION_LESS "0.16.0")
        set(FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS ON)
    endif()
elseif(NOT _kwargs_mode STREQUAL "OFF")
    message(FATAL_ERROR "FOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS must be AUTO, ON or OFF")
endif()

if(FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS)
    message(STATUS "ORT GenAI chat template kwargs: enabled")
else()
    message(STATUS "ORT GenAI chat template kwargs: disabled")
endif()
if(FOUNDRY_LOCAL_REQUIRE_CHAT_TEMPLATE_KWARGS AND NOT FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS)
    message(FATAL_ERROR
        "Chat template kwargs support is required but has not been enabled. "
        "Select a supported GenAI package, or set FOUNDRY_LOCAL_CHAT_TEMPLATE_KWARGS=ON "
        "for a local build whose tokenizer supports chat_template_kwargs.")
endif()
