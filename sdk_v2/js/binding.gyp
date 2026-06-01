{
    "targets": [
        {
            "target_name": "foundry_local_node",
            "sources": [
                "native/src/addon.cc",
                "native/src/catalog.cc",
                "native/src/errors.cc",
                "native/src/item_queue.cc",
                "native/src/items.cc",
                "native/src/manager.cc",
                "native/src/model.cc",
                "native/src/request.cc",
                "native/src/request_options.cc",
                "native/src/session.cc"
            ],
            "include_dirs": [
                "<!@(node -p \"require('node-addon-api').include\")",
                "../cpp/include",
                "<!(node script/gyp/print-vcpkg-include.mjs)"
            ],
            "defines": [
                "NAPI_VERSION=8",
                "NAPI_DISABLE_CPP_EXCEPTIONS=0",
                "NAPI_CPP_EXCEPTIONS"
            ],
            "cflags!": ["-fno-exceptions"],
            "cflags_cc!": ["-fno-exceptions"],
            "cflags_cc": ["-std=c++20", "-fexceptions"],
            "conditions": [
                ["OS=='win'", {
                    "msvs_settings": {
                        "VCCLCompilerTool": {
                            "ExceptionHandling": 1,
                            "LanguageStandard": "stdcpp20",
                            "AdditionalOptions": ["/EHsc", "/Zc:__cplusplus"]
                        },
                        "VCLinkerTool": {
                            "AdditionalLibraryDirectories": [
                                "<!(node script/gyp/print-import-lib-dir.mjs)"
                            ],
                            "AdditionalDependencies": ["foundry_local.lib"]
                        }
                    }
                }],
                ["OS=='linux'", {
                    "libraries": [
                        "-L<!(node script/gyp/print-import-lib-dir.mjs)",
                        "-lfoundry_local",
                        "-Wl,-rpath,'$$ORIGIN'"
                    ]
                }],
                ["OS=='mac'", {
                    "xcode_settings": {
                        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                        "CLANG_CXX_LANGUAGE_STANDARD": "c++20",
                        "MACOSX_DEPLOYMENT_TARGET": "11.0",
                        "OTHER_LDFLAGS": [
                            "-Wl,-rpath,@loader_path"
                        ]
                    },
                    "libraries": [
                        "-L<!(node script/gyp/print-import-lib-dir.mjs)",
                        "-lfoundry_local"
                    ]
                }]
            ]
        },
        {
            "target_name": "foundry_local_preload",
            "sources": [
                "native/src/preload_addon.cc"
            ],
            "include_dirs": [
                "<!@(node -p \"require('node-addon-api').include\")"
            ],
            "defines": [
                "NAPI_VERSION=8",
                "NAPI_DISABLE_CPP_EXCEPTIONS=0",
                "NAPI_CPP_EXCEPTIONS"
            ],
            "cflags!": ["-fno-exceptions"],
            "cflags_cc!": ["-fno-exceptions"],
            "cflags_cc": ["-std=c++20", "-fexceptions"],
            "conditions": [
                ["OS=='win'", {
                    "msvs_settings": {
                        "VCCLCompilerTool": {
                            "ExceptionHandling": 1,
                            "LanguageStandard": "stdcpp20",
                            "AdditionalOptions": ["/EHsc", "/Zc:__cplusplus"]
                        }
                    }
                }],
                ["OS=='linux'", {
                    "libraries": ["-ldl"]
                }],
                ["OS=='mac'", {
                    "xcode_settings": {
                        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                        "CLANG_CXX_LANGUAGE_STANDARD": "c++20",
                        "MACOSX_DEPLOYMENT_TARGET": "11.0"
                    }
                }]
            ]
        },
        {
            "target_name": "copy_addon_to_prebuilds",
            "type": "none",
            "dependencies": ["foundry_local_node", "foundry_local_preload"],
            "copies": [
                {
                    "files": [
                        "<(PRODUCT_DIR)/foundry_local_node.node",
                        "<(PRODUCT_DIR)/foundry_local_preload.node"
                    ],
                    "destination": "<!(node script/gyp/print-prebuild-dir.mjs)"
                }
            ]
        }
    ]
}
