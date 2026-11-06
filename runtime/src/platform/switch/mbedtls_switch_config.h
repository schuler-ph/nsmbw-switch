/* mbed TLS user configuration for the Switch build (MBEDTLS_USER_CONFIG_FILE, runtime/CMakeLists.txt).
 * mbed TLS only knows Unix and Windows platforms; on Horizon (devkitA64/libnx):
 *  - entropy comes from the kernel CSPRNG through mbedtls_hardware_poll(),
 *  - the millisecond clock is mbedtls_ms_time() over the ARM system tick,
 *  - the timing module (only used by DTLS) is left out.
 * Both functions are defined in src/platform/switch/switch_platform.cpp. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#undef MBEDTLS_TIMING_C
