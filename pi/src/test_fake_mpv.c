/**
 * test_fake_mpv.c — A stand-in for mpv, for test_player's real-process test
 * (docs/player-spec.md §13 T7). Test-only; never deployed.
 *
 * Takes mpv's argv, serves --input-ipc-server=<path> (one client at a
 * time, blocking I/O: it's its own process), answers every command with
 * "success", and emits start-file + file-loaded after a loadfile. It exits
 * on the "quit" command, or when killed.
 *
 * If FAKE_MPV_RECORD names a file, it appends to it:
 *   spawn [argv0] [argv1] ...
 *   xdg <XDG_RUNTIME_DIR or (unset)>
 *   cmd <each command line as received>
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cJSON.h"

static FILE *g_rec = NULL;

static void out(int fd, const char *s)
{
    size_t len = strlen(s), off = 0;
    while (off < len) {
        ssize_t n = write(fd, s + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static void handle(int fd, const char *line)
{
    if (g_rec) { fprintf(g_rec, "cmd %s\n", line); fflush(g_rec); }
    cJSON *obj = cJSON_Parse(line);
    if (!obj) return;
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(obj, "request_id");
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(obj, "command");
    const cJSON *name = cJSON_IsArray(cmd) ? cJSON_GetArrayItem(cmd, 0) : NULL;
    char buf[128];
    if (cJSON_IsNumber(rid)) {
        snprintf(buf, sizeof(buf), "{\"request_id\":%d,\"error\":\"success\",\"data\":null}\n",
                 (int)rid->valuedouble);
        out(fd, buf);
    }
    if (cJSON_IsString(name)) {
        if (strcmp(name->valuestring, "loadfile") == 0) {
            out(fd, "{\"event\":\"start-file\",\"playlist_entry_id\":1}\n");
            out(fd, "{\"event\":\"file-loaded\"}\n");
        } else if (strcmp(name->valuestring, "quit") == 0) {
            cJSON_Delete(obj);
            exit(0);
        }
    }
    cJSON_Delete(obj);
}

int main(int argc, char **argv)
{
    const char *sock = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--input-ipc-server=", 19) == 0) sock = argv[i] + 19;
    }
    const char *rec = getenv("FAKE_MPV_RECORD");
    if (rec) g_rec = fopen(rec, "a");
    if (g_rec) {
        fprintf(g_rec, "spawn");
        for (int i = 0; i < argc; i++) fprintf(g_rec, " [%s]", argv[i]);
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        fprintf(g_rec, "\nxdg %s\n", xdg ? xdg : "(unset)");
        fflush(g_rec);
    }
    if (!sock) return 2;
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
    unlink(sock);       /* like mpv */
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(lfd, 4) < 0)
        return 3;

    static char buf[65536];
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            return 4;
        }
        size_t len = 0;
        for (;;) {
            ssize_t n = read(cfd, buf + len, sizeof(buf) - 1 - len);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            len += (size_t)n;
            buf[len] = '\0';
            char *start = buf, *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                handle(cfd, start);
                start = nl + 1;
            }
            len = (size_t)(buf + len - start);
            memmove(buf, start, len);
            if (len == sizeof(buf) - 1) len = 0;    /* absurd line: drop */
        }
        close(cfd);
    }
}
