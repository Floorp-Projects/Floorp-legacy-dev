# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
{
  'includes': [
    '../../coreconf/config.gypi'
  ],
  'targets': [
    {
      'target_name': 'intel-gcm-s_lib',
      'type': 'static_library',
      'sources': [
        'intel-aes.s',
        'intel-gcm.s',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'conditions': [
        [ 'cc_is_clang==1 and force_integrated_as!=1', {
          'cflags': [
            '-no-integrated-as',
          ],
          'cflags_mozilla': [
            '-no-integrated-as',
          ],
          'asflags_mozilla': [
            '-no-integrated-as',
          ],
        }],
      ],
    },
    {
      'target_name': 'intel-gcm-wrap_c_lib',
      'type': 'static_library',
      'sources': [
        'intel-gcm-wrap.c',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'conditions': [
        [ '(OS=="linux" or OS=="android") and target_arch=="x64"', {
          'dependencies': [
            'intel-gcm-s_lib',
          ],
        }],
      ],
      'cflags': [
        '-mssse3',
      ],
      'cflags_mozilla': [
        '-mssse3'
      ],
    },
    {
      'target_name': 'hw-acc-crypto-avx',
      'type': 'static_library',
      # 'sources': [
      #   All AVX hardware accelerated crypto currently requires x64
      # ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'conditions': [
        [ 'target_arch=="x64"', {
          'cflags': [
            '-mssse3',
            '-msse4.1',
            '-msse4.2'
          ],
          'cflags_mozilla': [
            '-mssse3',
            '-msse4.1',
            '-msse4.2',
            '-mpclmul',
            '-maes',
            '-mavx',
          ],
          # GCC doesn't define this.
          'defines': [
            '__SSSE3__',
          ],
        }],
        [ 'OS=="linux" or OS=="android" or OS=="dragonfly" or OS=="freebsd" or \
           OS=="netbsd" or OS=="openbsd"', {
          'cflags': [
            '-mpclmul',
            '-maes',
            '-mavx',
          ],
        }],
        # macOS build doesn't use cflags.
        [ 'OS=="mac" or OS=="ios"', {
          'xcode_settings': {
            'OTHER_CFLAGS': [
              '-mssse3',
              '-msse4.1',
              '-msse4.2',
              '-mpclmul',
              '-maes',
              '-mavx',
            ],
          },
        }],
        [ 'target_arch=="arm"', {
          # Gecko doesn't support non-NEON platform on Android, but tier-3
          # platform such as Linux/arm will need it
          'cflags_mozilla': [
            '-mfpu=neon'
          ],
        }],
        [ 'target_arch=="x64"', {
          'sources': [
            'verified/Hacl_Poly1305_128.c',
            'verified/Hacl_Chacha20_Vec128.c',
            'verified/Hacl_Chacha20Poly1305_128.c',
          ],
        }],
      ],
    },
    {
      'target_name': 'hw-acc-crypto-avx2',
      'type': 'static_library',
      # 'sources': [
      #   All AVX2 hardware accelerated crypto currently requires x64
      # ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'conditions': [
        [ 'target_arch=="x64"', {
          'cflags': [
            '-mssse3',
            '-msse4.1',
            '-msse4.2'
          ],
          'cflags_mozilla': [
            '-mssse3',
            '-msse4.1',
            '-msse4.2',
            '-mpclmul',
            '-maes',
            '-mavx',
            '-mavx2',
          ],
          # GCC doesn't define this.
          'defines': [
            '__SSSE3__',
          ],
        }],
        [ 'OS=="linux" or OS=="android" or OS=="dragonfly" or OS=="freebsd" or \
           OS=="netbsd" or OS=="openbsd"', {
          'cflags': [
            '-mpclmul',
            '-maes',
            '-mavx',
            '-mavx2',
          ],
        }],
        # macOS build doesn't use cflags.
        [ 'OS=="mac" or OS=="ios"', {
          'xcode_settings': {
            'OTHER_CFLAGS': [
              '-mssse3',
              '-msse4.1',
              '-msse4.2',
              '-mpclmul',
              '-maes',
              '-mavx',
              '-mavx2',
            ],
          },
        }],
        [ 'target_arch=="arm"', {
          # Gecko doesn't support non-NEON platform on Android, but tier-3
          # platform such as Linux/arm will need it
          'cflags_mozilla': [
            '-mfpu=neon'
          ],
        }],
        [ 'target_arch=="x64"', {
          'sources': [
            'verified/Hacl_Poly1305_256.c',
            'verified/Hacl_Chacha20_Vec256.c',
            'verified/Hacl_Chacha20Poly1305_256.c',
          ],
        }],
      ],
    },
    {
      'target_name': 'gcm-aes-x86_c_lib',
      'type': 'static_library',
      'sources': [
        'gcm-x86.c', 'aes-x86.c'
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      # Enable isa option for pclmul and aes-ni; supported since gcc 4.4.
      # This is only supported by x84/x64. It's not needed for Windows,
      # unless clang-cl is used.
      'cflags_mozilla': [
        '-mpclmul', '-maes'
      ],
      'conditions': [
        [ 'OS=="linux" or OS=="android" or OS=="dragonfly" or OS=="freebsd" or OS=="netbsd" or OS=="openbsd"', {
          'cflags': [
            '-mpclmul', '-maes'
          ],
        }],
        # macOS build doesn't use cflags.
        [ 'OS=="mac" or OS=="ios"', {
          'xcode_settings': {
            'OTHER_CFLAGS': [
              '-mpclmul', '-maes'
            ],
          },
        }]
      ]
    },
    {
      'target_name': 'sha-x86_c_lib',
      'type': 'static_library',
      'sources': [
        'sha256-x86.c'
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'cflags': [
        '-msha',
        '-mssse3',
        '-msse4.1'
      ],
      'cflags_mozilla': [
        '-msha',
        '-mssse3',
        '-msse4.1'
      ],
      'conditions': [
        # macOS build doesn't use cflags.
        [ 'OS=="mac" or OS=="ios"', {
          'xcode_settings': {
            'OTHER_CFLAGS': [
              '-msha',
              '-mssse3',
              '-msse4.1'
            ],
          },
        }]
      ]
    },
    {
      'target_name': 'gcm-aes-arm32-neon_c_lib',
      'type': 'static_library',
      'sources': [
        'gcm-arm32-neon.c'
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'cflags': [
        '-march=armv7',
        '-mfpu=neon',
        '<@(softfp_cflags)',
      ],
      'cflags_mozilla': [
        '-mfpu=neon',
        '<@(softfp_cflags)',
      ]
    },
    {
      'target_name': 'gcm-aes-aarch64_c_lib',
      'type': 'static_library',
      'sources': [
        'gcm-aarch64.c'
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'cflags': [
        '-march=armv8-a+crypto'
      ],
      'cflags_mozilla': [
        '-march=armv8-a+crypto'
      ]
    },
    {
      'target_name': 'gcm-aes-ppc_c_lib',
      'type': 'static_library',
      'sources': [
        'gcm-ppc.c',
        'sha512-p8.s',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'conditions': [
        [ 'disable_crypto_vsx==0', {
          'cflags': [
            '-mcrypto',
            '-maltivec'
           ],
           'cflags_mozilla': [
             '-mcrypto',
             '-maltivec'
           ],
        }, 'disable_crypto_vsx==1', {
          'cflags': [
            '-maltivec'
          ],
          'cflags_mozilla': [
            '-maltivec'
          ],
        }]
      ]
    },
    {
      'target_name': 'gcm-aes-ppc_lib',
      'type': 'static_library',
      'sources': [
        'ppc-gcm.s',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'conditions': [
        [ 'cc_is_clang==1 and force_integrated_as!=1', {
          'cflags': [
            '-no-integrated-as',
          ],
          'cflags_mozilla': [
            '-no-integrated-as',
          ],
          'asflags_mozilla': [
            '-no-integrated-as',
          ],
        }],
      ],
    },
    {
      'target_name': 'ppc-gcm-wrap-nodepend_c_lib',
      'type': 'static_library',
      'sources': [
        'ppc-gcm-wrap.c',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports',
        'gcm-aes-ppc_lib',
      ],
    },
    {
      'target_name': 'ppc-gcm-wrap_c_lib',
      'type': 'static_library',
      'sources': [
        'ppc-gcm-wrap.c',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports',
        'gcm-aes-ppc_lib',
      ],
      'defines!': [
        'FREEBL_NO_DEPEND',
      ],
    },
    {
      'target_name': 'gcm-sha512-nodepend-ppc_c_lib',
      'type': 'static_library',
      'sources': [
        'sha512.c',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'conditions': [
        [ 'disable_crypto_vsx==0', {
          'cflags': [
            '-mcrypto',
            '-maltivec',
            '-mvsx',
            '-funroll-loops',
            '-fpeel-loops'
           ],
           'cflags_mozilla': [
            '-mcrypto',
            '-maltivec',
            '-mvsx',
            '-funroll-loops',
            '-fpeel-loops'
           ],
        }, 'disable_crypto_vsx==1', {
          'cflags': [
            '-maltivec',
            '-funroll-loops',
            '-fpeel-loops'
          ],
          'cflags_mozilla': [
            '-maltivec',
            '-funroll-loops',
            '-fpeel-loops'
          ],
        }]
      ]
    },
    {
      'target_name': 'gcm-sha512-ppc_c_lib',
      'type': 'static_library',
      'sources': [
        'sha512.c',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'conditions': [
        [ 'disable_crypto_vsx==0', {
          'cflags': [
            '-mcrypto',
            '-maltivec',
            '-mvsx',
            '-funroll-loops',
            '-fpeel-loops'
           ],
           'cflags_mozilla': [
            '-mcrypto',
            '-maltivec',
            '-mvsx',
            '-funroll-loops',
            '-fpeel-loops'
           ],
        }, 'disable_crypto_vsx==1', {
          'cflags': [
            '-maltivec',
            '-funroll-loops',
            '-fpeel-loops'
          ],
          'cflags_mozilla': [
            '-maltivec',
            '-funroll-loops',
            '-fpeel-loops'
          ],
        }]
      ],
      'defines!': [
        'FREEBL_NO_DEPEND',
      ],
    },
    {
      'target_name': 'chacha20-ppc_lib',
      'type': 'static_library',
      'sources': [
        'chacha20poly1305-ppc.c',
        'chacha20-ppc64le.S',
      ]
    },
    {
      'target_name': 'armv8_c_lib',
      'type': 'static_library',
      'sources': [
        'aes-armv8.c',
        'sha1-armv8.c',
        'sha256-armv8.c',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ],
      'conditions': [
        [ 'target_arch=="arm"', {
          'cflags': [
            '-march=armv8-a',
            '-mfpu=crypto-neon-fp-armv8',
            '<@(softfp_cflags)',
          ],
          'cflags_mozilla': [
            '-march=armv8-a',
            '-mfpu=crypto-neon-fp-armv8',
            '<@(softfp_cflags)',
          ],
        }, 'target_arch=="arm64" or target_arch=="aarch64"', {
          'cflags': [
            '-march=armv8-a+crypto'
          ],
          'cflags_mozilla': [
            '-march=armv8-a+crypto'
          ],
        }]
      ]
    },
    {
      'target_name': 'freebl',
      'type': 'static_library',
      'sources': [
        'loader.c'
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports'
      ]
    },
    # Build a static freebl library so we can statically link it into
    # the binary. This way we don't have to dlopen() the shared lib
    # but can directly call freebl functions.
    {
      'target_name': 'freebl_static',
      'type': 'static_library',
      'includes': [
        'freebl_base.gypi',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports',
        'hw-acc-crypto-avx',
        'hw-acc-crypto-avx2',
      ],
      'conditions': [
        [ 'target_arch=="ia32" or target_arch=="x64"', {
          'dependencies': [
            'gcm-aes-x86_c_lib',
          ],
        }, '(disable_arm_hw_aes==0 or disable_arm_hw_sha1==0 or disable_arm_hw_sha2==0) and (target_arch=="arm" or target_arch=="arm64" or target_arch=="aarch64")', {
          'dependencies': [
            'armv8_c_lib'
          ],
        }],
        [ '(target_arch=="ia32" or target_arch=="x64") and disable_intel_hw_sha==0', {
          'dependencies': [
            'sha-x86_c_lib',
          ],
        }],
        [ 'disable_arm32_neon==0 and target_arch=="arm"', {
          'dependencies': [
            'gcm-aes-arm32-neon_c_lib',
          ],
        }],
        [ 'disable_arm32_neon==1 and target_arch=="arm"', {
          'defines!': [
            'NSS_DISABLE_ARM32_NEON',
          ],
        }],
        [ 'target_arch=="arm64" or target_arch=="aarch64"', {
          'dependencies': [
            'gcm-aes-aarch64_c_lib',
          ],
        }],
        [ 'disable_altivec==0 and target_arch=="ppc64"', {
          'dependencies': [
            'gcm-aes-ppc_c_lib',
            'gcm-sha512-ppc_c_lib',
          ],
        }],
        [ 'disable_altivec==0 and target_arch=="ppc64le"', {
          'dependencies': [
            'gcm-aes-ppc_c_lib',
            'gcm-sha512-ppc_c_lib',
            'chacha20-ppc_lib',
            'ppc-gcm-wrap_c_lib',
          ],
        }],
        [ 'disable_altivec==1 and (target_arch=="ppc64" or target_arch=="ppc64le")', {
          'defines!': [
            'NSS_DISABLE_ALTIVEC',
          ],
        }],
        [ 'disable_crypto_vsx==1 and (target_arch=="ppc" or target_arch=="ppc64" or target_arch=="ppc64le")', {
          'defines!': [
            'NSS_DISABLE_CRYPTO_VSX',
          ],
        }],
        [ 'OS=="linux"', {
          'defines!': [
            'FREEBL_NO_DEPEND',
            'FREEBL_LOWHASH',
            'USE_HW_AES',
            'INTEL_GCM',
            'PPC_GCM',
          ],
          'conditions': [
            [ 'target_arch=="x64"', {
              # The AES assembler code doesn't work in static builds.
              # The linker complains about non-relocatable code, and I
              # currently don't know how to fix this properly.
              'sources!': [
                'intel-aes.s',
                'intel-gcm.s',
              ],
            }],
          ],
        }],
      ],
    },
    {
      'target_name': '<(freebl_name)',
      'type': 'shared_library',
      'includes': [
        'freebl_base.gypi',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports',
        'hw-acc-crypto-avx',
        'hw-acc-crypto-avx2',
      ],
      'conditions': [
        [ 'target_arch=="ia32" or target_arch=="x64"', {
          'dependencies': [
            'gcm-aes-x86_c_lib',
          ]
        }, 'target_arch=="arm" or target_arch=="arm64" or target_arch=="aarch64"', {
          'dependencies': [
            'armv8_c_lib',
          ],
        }],
        [ '(target_arch=="ia32" or target_arch=="x64") and disable_intel_hw_sha==0', {
          'dependencies': [
            'sha-x86_c_lib',
          ],
        }],
        [ 'disable_arm32_neon==0 and target_arch=="arm"', {
          'dependencies': [
            'gcm-aes-arm32-neon_c_lib',
          ],
        }],
        [ 'disable_arm32_neon==1 and target_arch=="arm"', {
          'defines!': [
            'NSS_DISABLE_ARM32_NEON',
          ],
        }],
        [ 'target_arch=="arm64" or target_arch=="aarch64"', {
          'dependencies': [
            'gcm-aes-aarch64_c_lib',
          ],
        }],
        [ 'disable_altivec==0', {
          'conditions': [
            [ 'target_arch=="ppc64"', {
              'dependencies': [
                'gcm-aes-ppc_c_lib',
                'gcm-sha512-nodepend-ppc_c_lib',
              ],
            }, 'target_arch=="ppc64le"', {
               'dependencies': [
                 'gcm-aes-ppc_c_lib',
                 'gcm-sha512-nodepend-ppc_c_lib',
                 'ppc-gcm-wrap-nodepend_c_lib',
               ],
            }],
          ],
        }],
        [ 'disable_altivec==1 and (target_arch=="ppc64" or target_arch=="ppc64le")', {
          'defines!': [
            'NSS_DISABLE_ALTIVEC',
          ],
        }],
        [ 'disable_crypto_vsx==1 and (target_arch=="ppc" or target_arch=="ppc64" or target_arch=="ppc64le")', {
          'defines!': [
            'NSS_DISABLE_CRYPTO_VSX',
          ],
        }],
        [ 'OS!="linux"', {
          'conditions': [
            [ 'moz_fold_libs==0', {
              'dependencies': [
                '<(DEPTH)/lib/util/util.gyp:nssutil3',
              ],
            }, {
              'libraries': [
                '<(moz_folded_library_name)',
              ],
            }],
          ],
        }],
        [ '(OS=="linux" or OS=="android") and target_arch=="x64"', {
          'dependencies': [
            'intel-gcm-wrap_c_lib',
          ],
        }],
        [ 'OS=="win" and (target_arch=="ia32" or target_arch=="x64") and cc_is_clang==1', {
          'dependencies': [
            'intel-gcm-wrap_c_lib',
          ],
        }],
        [ 'OS=="linux"', {
          'sources': [
            'nsslowhash.c',
            'stubs.c',
          ],
        }],
      ],
      'variables': {
       'conditions': [
         [ 'OS=="linux"', {
           'mapfile': 'freebl_hash_vector.def',
         }, {
           'mapfile': 'freebl.def',
         }],
       ]
      },
    },
    {
      'target_name': 'freebl_64int_3',
      'includes': [
        'freebl_base.gypi',
      ],
      'type': 'shared_library',
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports',
        'hw-acc-crypto-avx',
        'hw-acc-crypto-avx2',
      ],
    },
    {
      'target_name': 'freebl_64fpu_3',
      'includes': [
        'freebl_base.gypi',
      ],
      'type': 'shared_library',
      'sources': [
        'mpi/mpi_sparc.c',
        'mpi/mpv_sparcv9.s',
        'mpi/montmulfv9.s',
      ],
      'dependencies': [
        '<(DEPTH)/exports.gyp:nss_exports',
        'hw-acc-crypto-avx',
        'hw-acc-crypto-avx2',
      ],
      'asflags_mozilla': [
        '-mcpu=v9', '-Wa,-xarch=v9a'
      ],
      'defines': [
        'MP_NO_MP_WORD',
        'MP_USE_UINT_DIGIT',
        'MP_ASSEMBLY_MULTIPLY',
        'MP_USING_MONT_MULF',
        'MP_MONT_USE_MP_MUL',
      ],
    },
  ],
  'conditions': [
    [ 'OS=="linux"', {
      # stub build
      'targets': [
        {
          'target_name': 'freebl3',
          'type': 'shared_library',
          'defines': [
            'FREEBL_NO_DEPEND',
          ],
          'sources': [
            'lowhash_vector.c'
          ],
          'dependencies': [
            '<(DEPTH)/exports.gyp:nss_exports'
          ],
          'variables': {
            'mapfile': 'freebl_hash.def'
          }
        },
      ],
    }],
  ],
  'target_defaults': {
    'include_dirs': [
      'mpi',
      'ecl',
      'verified',
      'verified/internal',
      'verified/karamel/include',
      'verified/karamel/krmllib/dist/minimal',
      'deprecated',
    ],
    'defines': [
      'SHLIB_SUFFIX=\"<(dll_suffix)\"',
      'SHLIB_PREFIX=\"<(dll_prefix)\"',
      'SHLIB_VERSION=\"3\"',
      'SOFTOKEN_SHLIB_VERSION=\"3\"',
      'RIJNDAEL_INCLUDE_TABLES',
      'MP_API_COMPATIBLE'
    ],
    'conditions': [
      [ 'OS=="win" and target_arch=="ia32"', {
        'msvs_settings': {
          'VCCLCompilerTool': {
            #TODO: -Ox optimize flags
            'PreprocessorDefinitions': [
              'MP_ASSEMBLY_MULTIPLY',
              'MP_ASSEMBLY_SQUARE',
              'MP_ASSEMBLY_DIV_2DX1D',
              'MP_USE_UINT_DIGIT',
              'MP_NO_MP_WORD',
              'USE_HW_AES',
              'INTEL_GCM',
            ],
          },
        },
      }],
      [ 'OS=="win" and target_arch=="x64"', {
        'msvs_settings': {
          'VCCLCompilerTool': {
            #TODO: -Ox optimize flags
            'PreprocessorDefinitions': [
              # Should be copied to mingw defines below
              'MP_IS_LITTLE_ENDIAN',
              'NSS_BEVAND_ARCFOUR',
              'MPI_AMD64',
              'MP_ASSEMBLY_MULTIPLY',
              'NSS_USE_COMBA',
              'USE_HW_AES',
              'INTEL_GCM',
            ],
          },
        },
      }],
      [ '(OS=="win" or OS=="mac" or OS=="ios") and (target_arch=="ia32" or target_arch=="x64") and disable_intel_hw_sha==0', {
        'defines': [
          'USE_HW_SHA2',
        ],
      }],
      [ '(OS=="win" or OS=="mac" or OS=="ios") and (target_arch=="arm64" or target_arch=="aarch64") and disable_arm_hw_aes==0', {
        'defines': [
          'USE_HW_AES',
        ],
      }],
      [ '(OS=="win" or OS=="mac" or OS=="ios") and (target_arch=="arm64" or target_arch=="aarch64") and disable_arm_hw_sha1==0', {
        'defines': [
          'USE_HW_SHA1',
        ],
      }],
      [ '(OS=="win" or OS=="mac" or OS=="ios") and (target_arch=="arm64" or target_arch=="aarch64") and disable_arm_hw_sha2==0', {
        'defines': [
          'USE_HW_SHA2',
        ],
      }],
      [ 'cc_use_gnu_ld==1 and OS=="win" and target_arch=="x64"', {
        # mingw x64
        'defines': [
          'MP_IS_LITTLE_ENDIAN',
         ],
      }],
      # Poly1305_256 requires the flag to run
      ['target_arch=="x64"', {
        'defines':[
          'HACL_CAN_COMPILE_VEC128',
          'HACL_CAN_COMPILE_VEC256',
        ],
      }],
      # MSVC has no __int128 type. Use emulated int128 and leave
      # have_int128_support as-is for Curve25519 impl. selection.
      [ 'have_int128_support==1 and (OS!="win" or cc_is_clang==1 or cc_is_gcc==1)', {
        'defines': [
          # The Makefile does version-tests on GCC, but we're not doing that here.
          'HAVE_INT128_SUPPORT',
        ],
      }, {
        'defines': [
          'KRML_VERIFIED_UINT128',
        ],
      }],
      [ 'OS=="linux"', {
        'defines': [
          'FREEBL_LOWHASH',
          'FREEBL_NO_DEPEND',
        ],
        'conditions': [
          [ 'disable_altivec==0 and target_arch=="ppc64le"', {
            'defines': [
              'PPC_GCM',
            ],
          }],
        ],
      }],
      [ 'OS=="linux" or OS=="android"', {
        'conditions': [
          [ 'target_arch=="x64"', {
            'defines': [
              'MP_IS_LITTLE_ENDIAN',
              'NSS_BEVAND_ARCFOUR',
              'MPI_AMD64',
              'MP_ASSEMBLY_MULTIPLY',
              'NSS_USE_COMBA',
            ],
          }],
          [ 'target_arch=="x64"', {
            'defines': [
              'USE_HW_AES',
              'INTEL_GCM',
            ],
          }],
          [ 'target_arch=="ia32"', {
            'defines': [
              'MP_IS_LITTLE_ENDIAN',
              'MP_ASSEMBLY_MULTIPLY',
              'MP_ASSEMBLY_SQUARE',
              'MP_ASSEMBLY_DIV_2DX1D',
              'MP_USE_UINT_DIGIT',
            ],
          }],
          [ 'target_arch=="arm"', {
            'defines': [
              'MP_ASSEMBLY_MULTIPLY',
              'MP_ASSEMBLY_SQUARE',
              'MP_USE_UINT_DIGIT',
              'SHA_NO_LONG_LONG',
              'ARMHF',
            ],
          }],
          [ 'disable_intel_hw_sha==0 and (target_arch=="ia32" or target_arch=="x64")', {
            'defines': [
              'USE_HW_SHA2',
            ],
          }],
          [ 'disable_arm_hw_aes==0 and (target_arch=="arm" or target_arch=="arm64" or target_arch=="aarch64")', {
            'defines': [
              'USE_HW_AES',
            ],
          }],
          [ 'disable_arm_hw_sha1==0 and (target_arch=="arm" or target_arch=="arm64" or target_arch=="aarch64")', {
            'defines': [
              'USE_HW_SHA1',
            ],
          }],
          [ 'disable_arm_hw_sha2==0 and (target_arch=="arm" or target_arch=="arm64" or target_arch=="aarch64")', {
            'defines': [
              'USE_HW_SHA2',
            ],
          }],
        ],
      }],
    ],
  },
  'variables': {
    'module': 'nss',
    'conditions': [
      [ 'target_arch=="x64" or target_arch=="arm64" or target_arch=="aarch64"', {
        'have_int128_support%': 1,
      }, {
        'have_int128_support%': 0,
      }],
      [ 'target_arch=="arm"', {
        # When the compiler uses the softfloat ABI, we want to use the compatible softfp ABI when enabling NEON for these objects.
        # Confusingly, __SOFTFP__ is the name of the define for the softfloat ABI, not for the softfp ABI.
        'softfp_cflags': '<!(${CC:-cc} -o - -E -dM - ${CFLAGS} < /dev/null | grep __SOFTFP__ > /dev/null && echo -mfloat-abi=softfp || true)',
      }],
    ],
  }
}
