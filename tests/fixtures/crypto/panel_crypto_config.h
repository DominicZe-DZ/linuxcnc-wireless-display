/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * Minimal config for the SDK's own mbedTLS, used to test HMAC natively.
 * This is not the configuration used by the ESP32 firmware. */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
