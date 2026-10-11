/**
 * @file fuzz_runner.c
 * @brief Standalone corpus runner shared by all fuzz harnesses.
 *
 * Built only when FUZZ_STANDALONE_RUNNER is defined (FUZZ_MODE=asan/none):
 * libFuzzer mode links its own main() instead, so the Makefile defines
 * FUZZ_STANDALONE_RUNNER exclusively for the standalone flavors. The runner
 * replays corpus files/directories through LLVMFuzzerTestOneInput exactly
 * once — usable with or without sanitizers (this is the local-Windows path
 * where libFuzzer cannot link).
 *
 * libFuzzer-style flags (-dict=..., -max_total_time=..., -print_final_stats)
 * are accepted and ignored, so the Makefile's run_fuzz recipe works
 * unchanged in both flavors.
 */
typedef int fuzz_runner_tu_guard_t; /* keep the TU non-empty under libFuzzer */

#ifdef FUZZ_STANDALONE_RUNNER

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#define PATH_ISDIR(st) (((st).st_mode & _S_IFDIR) != 0)
#else
#include <dirent.h>
#define PATH_ISDIR(st) S_ISDIR((st).st_mode)
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static unsigned long g_files;
static unsigned long g_bytes;

static int run_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *buf;

    if (f == NULL) {
        fprintf(stderr, "fuzz_runner: cannot open %s\n", path);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "fuzz_runner: cannot size %s\n", path);
        fclose(f);
        return 1;
    }
    buf = (uint8_t *)malloc((len > 0) ? (size_t)len : 1u);
    if (buf == NULL) {
        fclose(f);
        return 1;
    }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "fuzz_runner: cannot read %s\n", path);
        free(buf);
        fclose(f);
        return 1;
    }
    fclose(f);
    LLVMFuzzerTestOneInput(buf, (size_t)len);
    free(buf);
    g_files++;
    g_bytes += (unsigned long)len;
    return 0;
}

static int run_dir(const char *path)
{
#ifdef _WIN32
    char pattern[1024];
    struct _finddata_t ent;
    intptr_t h;
    int rc = 0;

    snprintf(pattern, sizeof(pattern), "%s\\*.*", path);
    h = _findfirst(pattern, &ent);
    if (h == -1) {
        fprintf(stderr, "fuzz_runner: cannot list %s\n", path);
        return 1;
    }
    do {
        char child[1024];
        if (ent.name[0] == '.') {
            continue;
        }
        if ((ent.attrib & _A_SUBDIR) != 0) {
            continue;
        }
        snprintf(child, sizeof(child), "%s\\%s", path, ent.name);
        rc |= run_file(child);
    } while (_findnext(h, &ent) == 0);
    _findclose(h);
    return rc;
#else
    DIR *d = opendir(path);
    struct dirent *ent;
    int rc = 0;

    if (d == NULL) {
        fprintf(stderr, "fuzz_runner: cannot list %s\n", path);
        return 1;
    }
    while ((ent = readdir(d)) != NULL) {
        char child[1024];
        struct stat st;
        if (ent->d_name[0] == '.') {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (stat(child, &st) == 0 && PATH_ISDIR(st)) {
            continue;
        }
        rc |= run_file(child);
    }
    closedir(d);
    return rc;
#endif
}

static int run_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && PATH_ISDIR(st)) {
        return run_dir(path);
    }
    return run_file(path);
}

int main(int argc, char **argv)
{
    int i;
    int paths = 0;
    int bad = 0;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            continue; /* libFuzzer flag: accepted and ignored */
        }
        paths++;
        bad |= run_path(argv[i]);
    }
    if (paths == 0) {
        fprintf(stderr,
                "usage: %s <corpus-file-or-dir>... (libFuzzer flags ignored)\n",
                argv[0]);
        return 2;
    }
    if (bad != 0) {
        return 1;
    }
    printf("fuzz_runner: %lu files, %lu bytes replayed\n", g_files, g_bytes);
    return 0;
}

#endif /* FUZZ_STANDALONE_RUNNER */
