{
    'targets': [
        {
            'target_name': 'node-hactool',
            'sources': ['src/node-hactool.cc'],
            'cflags!': ['-fno-exceptions'],
            'cflags_cc!': ['-fno-exceptions'],
            'include_dirs': ["<!(node -p \"require('node-addon-api').include_dir\")"],
            'libraries': [],
            'dependencies': [
                "<!(node -p \"require('node-addon-api').gyp\")"
            ],
            'defines': [ 'NAPI_DISABLE_CPP_EXCEPTIONS' ],
            'conditions': [
                [
                    "OS=='win'",
                    {
                        "defines": [
                            "_HAS_EXCEPTIONS=1"
                        ],
                        "msvs_settings": {
                            "VCCLCompilerTool": {
                                "ExceptionHandling": 1
                            },
                        },
                    }
                ],
                [
                    "OS=='mac'",
                    {
                        'cflags+': ['-fvisibility=hidden'],
                        'xcode_settings': {
                            'GCC_SYMBOLS_PRIVATE_EXTERN': 'YES', # -fvisibility=hidden
                            'GCC_ENABLE_CPP_EXCEPTIONS': 'YES',
                            'CLANG_CXX_LIBRARY': 'libc++',
                            'MACOSX_DEPLOYMENT_TARGET': '10.9',
                            "OTHER_CFLAGS": [
                                "-arch x86_64",
                                "-arch arm64",
                            ],
                            "OTHER_LDFLAGS": [
                                "-arch x86_64",
                                "-arch arm64",
                            ],
                        },
                    }
                ],
            ],
        }
    ]
}