{
    'targets': [
        {
            'target_name': 'node-hactool',
            'sources': [
                'src/node-hactool.cpp',
                'src/library/getopt.c',
                "<!@(node -p \"require('fs').readdirSync('./src/hactool').filter(f=>f.endsWith('.c') || f.endsWith('.cpp')).map(f=>'src/hactool/'+f).join(' ')\")",
                "<!@(node -p \"require('fs').readdirSync('./src/hactool/mbedtls/library').filter(f=>f.endsWith('.c')).map(f=>'src/hactool/mbedtls/library/'+f).join(' ')\")"
            ],
            'cflags!': ['-fno-exceptions'],
            'cflags_cc!': ['-fno-exceptions'],
            'cflags_cc': ['-std:c++20'],
            'include_dirs': [
                "<!(node -p \"require('node-addon-api').include_dir\")",
                './src/hactool',
                './src/hactool/mbedtls/include',
                './src/hactool/fmt/include',
                './src/library'
            ],
            'libraries': [],
            'dependencies': [
                "<!(node -p \"require('node-addon-api').gyp\")"
            ],
            'defines': ['NAPI_DISABLE_CPP_EXCEPTIONS'],
            'conditions': [
                [
                    "OS=='win'",
                    {
                        'defines': [
                            '_HAS_EXCEPTIONS=1'
                        ],
                        'msvs_settings': {
                            'VCCLCompilerTool': {
                                'AdditionalOptions': ['-std:c++20', '-permissive'],
                                'ExceptionHandling': 1
                            },
                        }
                    }
                ],
                [
                    "OS=='mac'",
                    {
                        'cflags+': ['-fvisibility=hidden'],
                        'xcode_settings': {
                            'GCC_SYMBOLS_PRIVATE_EXTERN': 'YES',
                            'GCC_ENABLE_CPP_EXCEPTIONS': 'YES',
                            'CLANG_CXX_LIBRARY': 'libc++',
                            'CLANG_CXX_LANGUAGE_STANDARD': 'c++20',
                            'MACOSX_DEPLOYMENT_TARGET': '10.9',
                            'OTHER_CFLAGS': [
                                '-arch x86_64',
                                '-arch arm64',
                            ],
                            'OTHER_LDFLAGS': [
                                '-arch x86_64',
                                '-arch arm64',
                            ],
                        },
                    }
                ],
            ],
        }
    ]
}
