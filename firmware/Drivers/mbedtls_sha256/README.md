# mbed TLS SHA-256 subset

This directory contains unmodified upstream files from **mbed TLS v2.28.10** needed by the bootloader's image SHA-256 check (`Core/Src/boot_update.c`): `library/sha256.c`, `library/platform_util.c`, `library/common.h`, and their required headers under `include/mbedtls/`. The upstream license is retained in `LICENSE`.

Source: [mbed TLS v2.28.10](https://github.com/Mbed-TLS/mbedtls/tree/v2.28.10). Project configuration lives separately in `Core/Inc/mbedtls/config.h`. CMake and CubeIDE compile only the two library C files listed above.

This is not a complete cryptography or TLS distribution and does not implement the planned firmware signature verification. Add the selected algorithm's dependencies and tests when implementing signing; do not assume this subset supplies them.
