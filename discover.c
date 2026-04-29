// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "include/snmp_linux.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define OID_IF_DESCR "1.3.6.1.2.1.2.2.1.2"
#define MAX_INTERFACES 256

typedef struct {
    int index;
    char name[SNMP_STR_MAX];
} discovered_iface_t;

typedef struct {
    discovered_iface_t ifaces[MAX_INTERFACES];
    int count;
} discover_state_t;

static int compare_iface(const void *a, const void *b) {
    const discovered_iface_t *ia = (const discovered_iface_t *)a;
    const discovered_iface_t *ib = (const discovered_iface_t *)b;
    return ia->index - ib->index;
}

static bool walk_callback(const char *oid_str, const snmp_value_t *value, void *userdata) {
    discover_state_t *state = (discover_state_t *)userdata;
    if (state->count >= MAX_INTERFACES)
        return false;
    const char *last = strrchr(oid_str, '.');
    if (!last)
        return true;
    int index = atoi(last + 1);
    if (index <= 0)
        return true;
    state->ifaces[state->count].index = index;
    strncpy(state->ifaces[state->count].name, value->str, SNMP_STR_MAX - 1);
    state->ifaces[state->count].name[SNMP_STR_MAX - 1] = '\0';
    state->count++;
    return true;
}

static bool is_not_connected(const char *name) {
    return strncmp(name, "n/c", 3) == 0 || strstr(name, "no connect") != NULL;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

int main(int argc, char **argv) {

    if (argc < 2) {
        fprintf(stderr, "usage: %s <host[:community]> [host[:community]] ...\n", argv[0]);
        fprintf(stderr, "  e.g. %s 192.168.0.128\n", argv[0]);
        fprintf(stderr, "  e.g. %s 192.168.0.128:public 192.168.0.107:private\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "output is traffmon.cfg format. Redirect to save:\n");
        fprintf(stderr, "  %s 192.168.0.128 192.168.0.107 >> /etc/default/traffmon\n", argv[0]);
        return EXIT_FAILURE;
    }

    snmp_begin();

    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    printf("# discovered %d device(s) at %s\n", argc - 1, timestamp);
    printf("traffic-poll-period=60\n");

    int total_count = 0;
    for (int i = 1; i < argc; i++) {

        char target[256];
        strncpy(target, argv[i], sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';

        const char *host      = target;
        const char *community = "public";
        char *colon           = strchr(target, ':');
        if (colon) {
            *colon    = '\0';
            community = colon + 1;
        }

        fprintf(stderr, "discovering %s (community=%s)...\n", host, community);

        discover_state_t state = { .count = 0 };
        char err[256];
        if (!snmp_walk(host, community, OID_IF_DESCR, walk_callback, &state, err, sizeof(err))) {
            fprintf(stderr, "  error: %s\n", err);
            continue;
        }

        qsort(state.ifaces, (size_t)state.count, sizeof(discovered_iface_t), compare_iface);
        fprintf(stderr, "  found %d interfaces\n", state.count);

        printf("\n# %s (%s) - %d interfaces\n", host, community, state.count);
        for (int j = 0; j < state.count; j++) {
            const char *prefix = is_not_connected(state.ifaces[j].name) ? "# " : "";
            printf("%straffic-iface[%d]=%s:%s:%d:%s\n", prefix, ++total_count, host, community, state.ifaces[j].index, state.ifaces[j].name);
        }
    }

    snmp_end();
    return total_count > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
