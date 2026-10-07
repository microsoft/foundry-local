# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

function(foundry_local_configure_shared_library_exports target)
    if(APPLE)
        set(exports "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/exported-symbols.lst")
        target_link_options(${target} PRIVATE "LINKER:-exported_symbols_list,${exports}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
        set(exports "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/exports.map")
        # Public entry points come from OBJECT sources, not STATIC archives.
        target_link_options(${target} PRIVATE
            "LINKER:--version-script,${exports}"
            "LINKER:--exclude-libs,ALL")
    else()
        return()
    endif()
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${exports}")
endfunction()
