// Multithreaded stress test for sanitizer builds (links the sources directly).
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const char *context_say_hello(void);
void initialize_context_helper(void);
void cleanup_context_helper(void);
void recover_context_helper_after_resume(void);
void free_string(const char *);
char *getFocusedAppInfo(void);
void getFocusedAppInfoAsync(void (*)(const char *));
char *getFocusedElementInfo(void);
void getFocusedElementInfoAsync(void (*)(const char *));
char *getFocusedElementRelatedContent(int, int, int);
void getFocusedElementRelatedContentAsync(int, int, int, void (*)(const char *));
char *getFocusedVisibleText(int, int);
void getFocusedVisibleTextAsync(int, int, void (*)(const char *));
int setFocusedWindowEnhancedUserInterface(void);

static atomic_int n_cb, n_bad;
static void cb(const char *s) {
    if (!s) atomic_fetch_add(&n_bad, 1);
    else (void)strlen(s);
    atomic_fetch_add(&n_cb, 1);
}

static volatile int stop;
static void *hammer(void *arg) {
    long id = (long)arg;
    unsigned seed = (unsigned)id * 7919u;
    while (!stop) {
        switch (rand_r(&seed) % 9) {
        case 0: getFocusedAppInfoAsync(cb); break;
        case 1: getFocusedElementInfoAsync(cb); break;
        case 2: getFocusedElementRelatedContentAsync(500, 500, 400, cb); break;
        case 3: getFocusedVisibleTextAsync(2000, 800, cb); break;
        case 4: free_string(getFocusedAppInfo()); break;
        case 5: free_string(getFocusedElementInfo()); break;
        case 6: free_string(getFocusedElementRelatedContent(50, 50, 300)); break;
        case 7: free_string(getFocusedVisibleText(500, 500)); break;
        case 8: setFocusedWindowEnhancedUserInterface(); break;
        }
        usleep(1000 * (rand_r(&seed) % 20));
    }
    return NULL;
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    printf("%s\n", context_say_hello());
    for (int round = 0; round < 2; round++) {
        initialize_context_helper();
        stop = 0;
        pthread_t th[6];
        for (long i = 0; i < 6; i++) pthread_create(&th[i], NULL, hammer, (void *)i);
        for (int s = 0; s < secs; s++) {
            sleep(1);
            if (s % 3 == 1) recover_context_helper_after_resume();
        }
        stop = 1;
        for (int i = 0; i < 6; i++) pthread_join(th[i], NULL);
        cleanup_context_helper();
        printf("round %d: callbacks=%d bad=%d\n", round, atomic_load(&n_cb), atomic_load(&n_bad));
    }
    return atomic_load(&n_bad) ? 1 : 0;
}
