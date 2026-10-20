/*
 * Host tests for the update and remote-help modules ported from the
 * Velocity9x updater (crypto, inflate, zip, update protocol, signed release
 * file) and for the relay framing.
 */
#include <stdio.h>

unsigned int v9x_run_crypto_tests(void);
unsigned int v9x_run_inflate_tests(void);
unsigned int v9x_run_update_proto_tests(void);
unsigned int v9x_run_update_release_tests(void);
unsigned int v9x_run_relay_tests(void);

int main(void)
{
    unsigned int failures = 0u;

    failures += v9x_run_crypto_tests();
    failures += v9x_run_inflate_tests();
    failures += v9x_run_update_proto_tests();
    failures += v9x_run_update_release_tests();
    failures += v9x_run_relay_tests();
    if (failures != 0u) {
        printf("%u update/relay test failure(s)\n", failures);
        return 1;
    }
    printf("PASS: crypto, inflate, zip, update protocol, release file, relay\n");
    return 0;
}
