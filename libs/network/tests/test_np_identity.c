/*
 * test_np_identity - the player name np_psnr_setup settles on.
 *
 *   clang-cl /I include /Fe:test_np_identity.exe libs/network/tests/test_np_identity.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_np_identity libs/network/tests/test_np_identity.c
 */
#include "../psnr/psnr.c"
#include "../np_psnr.c"

#include <assert.h>

const char* np_fake_username(void) { return "PS3Player"; }

#ifdef _WIN32
#  define OS_USER "USERNAME"
#else
#  define OS_USER "USER"
#endif

/* Resolve afresh, as a new process would. */
static const char* resolve(const char* username, const char* server)
{
    s_id_state = 0;
    np_psnr_setup(username, server);
    return np_psnr_identity();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    set_env("PS3_NP_ONLINE_ID", "");

    /* Offline with no name: no identity, so the old fixed PSID/MAC and
     * "PS3Player" stand and offline saves don't change. */
    assert(resolve(NULL, NULL) == NULL);
    assert(strcmp(np_psnr_online_id(), "PS3Player") == 0);

    /* A name applies offline too, and the flag beats the variable. */
    set_env("PS3_NP_ONLINE_ID", "marge");
    assert(strcmp(resolve(NULL, NULL), "marge") == 0);
    assert(strcmp(resolve("homer", NULL), "homer") == 0);
    assert(strcmp(np_psnr_online_id(), "homer") == 0);

    /* PSN's rules: disallowed characters dropped, 16 at most, and a name that
     * is still not an ID (too short, or not starting with a letter) is
     * ignored -- here falling back to the variable. */
    assert(strcmp(resolve("Bart Simpson!", NULL), "BartSimpson") == 0);
    assert(strcmp(resolve("abcdefghijklmnopqrstuvwxyz", NULL), "abcdefghijklmnop") == 0);
    assert(strcmp(resolve("x!", NULL), "marge") == 0);
    assert(strcmp(resolve("1abc", NULL), "marge") == 0);

    /* --psnr goes online; with no name, the OS login name is used. */
    set_env("PS3_NP_ONLINE_ID", "");
    set_env(OS_USER, "Lisa S.");
    assert(strcmp(resolve(NULL, "127.0.0.1:36100"), "LisaS") == 0);
    assert(np_psnr_enabled());
    assert(strcmp(getenv("PSNR_SERVER"), "127.0.0.1:36100") == 0);

    /* An OS name that can't be an online ID falls back to "PS3Player". */
    set_env(OS_USER, "42");
    assert(strcmp(resolve(NULL, NULL), "PS3Player") == 0);

    printf("test_np_identity: all passed\n");
    return 0;
}
