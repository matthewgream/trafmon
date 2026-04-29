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

static bool is_not_connected(const char *name) { return strncmp(name, "n/c", 3) == 0 || strstr(name, "no connect") != NULL; }

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

int main(int argc, char **argv) {

    const char *forced_bundle = NULL;
    int arg_start             = 1;
    while (arg_start < argc && strncmp(argv[arg_start], "--bundle=", 9) == 0) {
        forced_bundle = argv[arg_start] + 9;
        if (!*forced_bundle) {
            fprintf(stderr, "error: --bundle= requires a value\n");
            return EXIT_FAILURE;
        }
        arg_start++;
    }

    if (arg_start >= argc) {
        fprintf(stderr, "usage: %s [--bundle=NAME] <host[:community]> [host[:community]] ...\n", argv[0]);
        fprintf(stderr, "  e.g. %s 192.168.0.128\n", argv[0]);
        fprintf(stderr, "  e.g. %s 192.168.0.128:public 192.168.0.107:private\n", argv[0]);
        fprintf(stderr, "  e.g. %s --bundle=uplink router-a.lan router-b.lan\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "without --bundle, each host becomes its own bundle named after the host.\n");
        fprintf(stderr, "with --bundle=NAME, all discovered interfaces aggregate into one bundle.\n");
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
    printf("# discovered %d device(s) at %s\n", argc - arg_start, timestamp);
    printf("traffic-poll-period=60\n");

    int total_count = 0;
    int bundle_idx  = 0;
    for (int i = arg_start; i < argc; i++) {

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

        const char *bundle_name = forced_bundle ? forced_bundle : host;
        if (forced_bundle)
            printf("\n# %s (%s) - %d interfaces (bundle '%s')\n", host, community, state.count, bundle_name);
        else {
            printf("\n# %s (%s) - %d interfaces (rename bundle '%s' as desired)\n", host, community, state.count, bundle_name);
            bundle_idx = 0;
        }
        for (int j = 0; j < state.count; j++) {
            const char *prefix = is_not_connected(state.ifaces[j].name) ? "# " : "";
            printf("%straffic-iface[%s][%d]=%s:%s:%d:%s\n", prefix, bundle_name, ++bundle_idx, host, community, state.ifaces[j].index, state.ifaces[j].name);
            total_count++;
        }
    }

    snmp_end();
    return total_count > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
