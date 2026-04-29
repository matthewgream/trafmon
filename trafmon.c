// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <json-c/json.h>

#include "include/util_linux.h"

#define MQTT_CONNECT_TIMEOUT 60
#define MQTT_PUBLISH_QOS 0
#define MQTT_PUBLISH_RETAIN false
#include "include/mqtt_linux.h"

#define CONFIG_MAX_ENTRIES 256
#include "include/config_linux.h"

#include "include/snmp_linux.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define CONFIG_FILE_DEFAULT "trafmon.cfg"

#define HEARTBEAT_PERIOD_DEFAULT 60
#define TRAFFIC_POLL_PERIOD_DEFAULT 60

#define MQTT_SERVER_DEFAULT ""
#define MQTT_CLIENT_DEFAULT "trafmon"
#define MQTT_TOPIC_PREFIX_DEFAULT "system/traffic"

#define MAX_TRAFFIC_TARGETS 64
#define MAX_TRAFFIC_BUNDLES 32
#define MAX_BUNDLE_NAME 64

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define PRINTF_ERROR printf_stderr
#define PRINTF_INFO printf_stdout

void printf_stdout(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

void printf_stderr(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

const struct option config_options[] = {
    { "config", required_argument, 0, 0 },
    { "name", required_argument, 0, 0 },
    { "heartbeat-period", required_argument, 0, 0 },
    { "heartbeat-verbose", required_argument, 0, 0 },
    { "traffic-poll-period", required_argument, 0, 0 },
    { "traffic-verbose", required_argument, 0, 0 },
    { "mqtt-server", required_argument, 0, 0 },
    { "mqtt-client", required_argument, 0, 0 },
    { "mqtt-topic-prefix", required_argument, 0, 0 },
    { 0, 0, 0, 0 },
};

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    bool enabled;
    bool verbose;
} heartbeat_config_t;

typedef struct {
    char host[64];
    char community[64];
    int index;
    char name[128];
    bool has_prev;
    time_t prev_time;
    uint64_t prev_in_octets;
    uint64_t prev_out_octets;
    uint64_t prev_in_packets;
    uint64_t prev_out_packets;
    uint64_t prev_in_errors;
    uint64_t prev_out_errors;
} traffic_target_t;

typedef struct {
    char name[MAX_BUNDLE_NAME];
    int target_indices[MAX_TRAFFIC_TARGETS];
    int target_count;
} traffic_bundle_t;

typedef struct {
    traffic_target_t targets[MAX_TRAFFIC_TARGETS];
    traffic_bundle_t bundles[MAX_TRAFFIC_BUNDLES];
    int target_count;
    int bundle_count;
    bool enabled;
    bool verbose;
} traffic_config_t;

typedef struct {
    int heartbeat_period;
    int traffic_poll_period;
} timing_config_t;

heartbeat_config_t heartbeat_config;
traffic_config_t traffic_config;
timing_config_t timing_config;
MqttConfig mqtt_config;
const char *mqtt_topic_prefix;
bool mqtt_enabled;
const char *instance_name;
char hostname[256];

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static bool do_heartbeat(char *status_message, size_t status_size) {
    static unsigned long counter = 0;
    snprintf(status_message, status_size, "daemon active (%lu)", ++counter);
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static void publish_daemon_event(const char *event_type, bool success, const char *message, bool verbose) {

    if (verbose)
        printf("%s: %s\n", event_type, message);

    if (mqtt_enabled) {

        char topic[256 + 64];
        snprintf(topic, sizeof(topic), "%s/%s", mqtt_topic_prefix, instance_name);

        json_object *json = json_object_new_object();
        json_object_object_add(json, "timestamp", json_object_new_int64(time(NULL)));
        json_object_object_add(json, "hostname", json_object_new_string(hostname));
        json_object_object_add(json, "name", json_object_new_string(instance_name));
        json_object_object_add(json, "event", json_object_new_string(event_type));
        json_object_object_add(json, "success", json_object_new_boolean(success));
        json_object_object_add(json, "message", json_object_new_string(message));

        if (strcmp(event_type, "heartbeat") == 0) {
            json_object_object_add(json, "traffic_enabled", json_object_new_boolean(traffic_config.enabled));
            json_object_object_add(json, "traffic_bundles", json_object_new_int(traffic_config.bundle_count));
            json_object_object_add(json, "traffic_targets", json_object_new_int(traffic_config.target_count));
        }

        const char *json_str = json_object_to_json_string(json);
        mqtt_send(topic, json_str, (int)strlen(json_str));

        json_object_put(json);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define TRAFFIC_OID_COUNT 8
static const char *traffic_oid_bases[TRAFFIC_OID_COUNT] = {
    "1.3.6.1.2.1.31.1.1.1.6",  // ifHCInOctets
    "1.3.6.1.2.1.31.1.1.1.10", // ifHCOutOctets
    "1.3.6.1.2.1.31.1.1.1.7",  // ifHCInUcastPkts
    "1.3.6.1.2.1.31.1.1.1.11", // ifHCOutUcastPkts
    "1.3.6.1.2.1.2.2.1.14",    // ifInErrors
    "1.3.6.1.2.1.2.2.1.20",    // ifOutErrors
    "1.3.6.1.2.1.2.2.1.8",     // ifOperStatus
    "1.3.6.1.2.1.31.1.1.1.15", // ifHighSpeed
};

static void traffic_format_bytes(uint64_t bytes, char *out, size_t out_size) {
    static const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double v                   = (double)bytes;
    size_t u                   = 0;
    while (v >= 1024.0 && u < (sizeof(units) / sizeof(units[0])) - 1) {
        v /= 1024.0;
        u++;
    }
    snprintf(out, out_size, "%.1f%s", v, units[u]);
}

static uint64_t traffic_delta_u64(uint64_t curr, uint64_t prev) { return (curr >= prev) ? curr - prev : 0; }

static void traffic_collect_host(const char *bundle_name, const char *host, const char *community, const int *target_indices, int target_count, time_t now, json_object *interfaces,
                                 int *success_count, int *fail_count) {

    const int item_count   = target_count * TRAFFIC_OID_COUNT;
    snmp_get_item_t *items = calloc((size_t)item_count, sizeof(snmp_get_item_t));
    if (!items)
        return;

    for (int t = 0; t < target_count; t++)
        for (int o = 0; o < TRAFFIC_OID_COUNT; o++)
            snprintf(items[t * TRAFFIC_OID_COUNT + o].oid, SNMP_OID_MAX, "%s.%d", traffic_oid_bases[o], traffic_config.targets[target_indices[t]].index);

    char err[256];
    if (!snmp_get(host, community, items, item_count, err, sizeof(err))) {
        if (traffic_config.verbose)
            printf("traffic[%s]: snmp get %s failed: %s\n", bundle_name, host, err);
        *fail_count += target_count;
        free(items);
        return;
    }

    for (int t = 0; t < target_count; t++) {
        traffic_target_t *target = &traffic_config.targets[target_indices[t]];
        const snmp_get_item_t *r = &items[t * TRAFFIC_OID_COUNT];

        if (!r[0].value.valid || !r[1].value.valid) {
            if (traffic_config.verbose)
                printf("traffic[%s]: %s/%s no counter data\n", bundle_name, target->host, target->name);
            (*fail_count)++;
            continue;
        }

        uint64_t in_octets   = r[0].value.u64;
        uint64_t out_octets  = r[1].value.u64;
        uint64_t in_packets  = r[2].value.u64;
        uint64_t out_packets = r[3].value.u64;
        uint64_t in_errors   = r[4].value.u64;
        uint64_t out_errors  = r[5].value.u64;
        uint64_t oper_status = r[6].value.u64;
        uint64_t speed_mbps  = r[7].value.u64;

        if (!target->has_prev) {
            target->prev_time        = now;
            target->prev_in_octets   = in_octets;
            target->prev_out_octets  = out_octets;
            target->prev_in_packets  = in_packets;
            target->prev_out_packets = out_packets;
            target->prev_in_errors   = in_errors;
            target->prev_out_errors  = out_errors;
            target->has_prev         = true;
            continue;
        }

        time_t duration = now - target->prev_time;
        if (duration <= 0)
            continue;

        uint64_t in_bytes  = traffic_delta_u64(in_octets, target->prev_in_octets);
        uint64_t out_bytes = traffic_delta_u64(out_octets, target->prev_out_octets);
        uint64_t in_pkts   = traffic_delta_u64(in_packets, target->prev_in_packets);
        uint64_t out_pkts  = traffic_delta_u64(out_packets, target->prev_out_packets);
        uint64_t in_errs   = traffic_delta_u64(in_errors, target->prev_in_errors);
        uint64_t out_errs  = traffic_delta_u64(out_errors, target->prev_out_errors);

        uint64_t in_bps  = (in_bytes * 8) / (uint64_t)duration;
        uint64_t out_bps = (out_bytes * 8) / (uint64_t)duration;

        if (traffic_config.verbose) {
            char in_str[16], out_str[16], errors_str[64] = "";
            traffic_format_bytes(in_bytes, in_str, sizeof(in_str));
            traffic_format_bytes(out_bytes, out_str, sizeof(out_str));
            if (in_errs > 0 || out_errs > 0)
                snprintf(errors_str, sizeof(errors_str), " ERRORS:%lu/%lu", (unsigned long)in_errs, (unsigned long)out_errs);
            printf("traffic[%s]: %s/%s %lds rx:%s(%.2fMbps) tx:%s(%.2fMbps)%s\n", bundle_name, target->host, target->name, (long)duration, in_str, (double)in_bps / 1e6, out_str,
                   (double)out_bps / 1e6, errors_str);
        }

        json_object *iface = json_object_new_object();
        json_object_object_add(iface, "device", json_object_new_string(target->host));
        json_object_object_add(iface, "interface", json_object_new_string(target->name));
        json_object_object_add(iface, "duration", json_object_new_int64((int64_t)duration));
        json_object_object_add(iface, "in_octets", json_object_new_int64((int64_t)in_bytes));
        json_object_object_add(iface, "out_octets", json_object_new_int64((int64_t)out_bytes));
        json_object_object_add(iface, "in_packets", json_object_new_int64((int64_t)in_pkts));
        json_object_object_add(iface, "out_packets", json_object_new_int64((int64_t)out_pkts));
        json_object_object_add(iface, "in_errors", json_object_new_int64((int64_t)in_errs));
        json_object_object_add(iface, "out_errors", json_object_new_int64((int64_t)out_errs));
        json_object_object_add(iface, "in_bps", json_object_new_int64((int64_t)in_bps));
        json_object_object_add(iface, "out_bps", json_object_new_int64((int64_t)out_bps));
        json_object_object_add(iface, "oper_status", json_object_new_int((int32_t)oper_status));
        json_object_object_add(iface, "speed_mbps", json_object_new_int64((int64_t)speed_mbps));
        json_object_array_add(interfaces, iface);
        (*success_count)++;

        target->prev_time        = now;
        target->prev_in_octets   = in_octets;
        target->prev_out_octets  = out_octets;
        target->prev_in_packets  = in_packets;
        target->prev_out_packets = out_packets;
        target->prev_in_errors   = in_errors;
        target->prev_out_errors  = out_errors;
    }

    free(items);
}

static void traffic_poll_bundle(traffic_bundle_t *bundle) {

    time_t now              = time(NULL);
    json_object *interfaces = json_object_new_array();
    int success_count       = 0;
    int fail_count          = 0;

    bool processed[MAX_TRAFFIC_TARGETS] = { false };
    for (int i = 0; i < bundle->target_count; i++) {
        if (processed[i])
            continue;
        int batch[MAX_TRAFFIC_TARGETS];
        int batch_count      = 0;
        batch[batch_count++] = bundle->target_indices[i];
        processed[i]         = true;
        traffic_target_t *t0 = &traffic_config.targets[bundle->target_indices[i]];
        for (int j = i + 1; j < bundle->target_count; j++)
            if (!processed[j])
                if (strcmp(t0->host, traffic_config.targets[bundle->target_indices[j]].host) == 0 &&
                    strcmp(t0->community, traffic_config.targets[bundle->target_indices[j]].community) == 0) {
                    batch[batch_count++] = bundle->target_indices[j];
                    processed[j]         = true;
                }
        traffic_collect_host(bundle->name, t0->host, t0->community, batch, batch_count, now, interfaces, &success_count, &fail_count);
    }

    if (json_object_array_length(interfaces) == 0) {
        json_object_put(interfaces);
        return;
    }

    bool all_ok = (fail_count == 0);
    char message[256];
    snprintf(message, sizeof(message), "%d/%d interfaces ok", success_count, bundle->target_count);

    if (mqtt_enabled) {
        char topic[256 + MAX_BUNDLE_NAME];
        snprintf(topic, sizeof(topic), "%s/%s", mqtt_topic_prefix, bundle->name);
        json_object *json = json_object_new_object();
        json_object_object_add(json, "timestamp", json_object_new_int64(now));
        json_object_object_add(json, "hostname", json_object_new_string(hostname));
        json_object_object_add(json, "name", json_object_new_string(bundle->name));
        json_object_object_add(json, "event", json_object_new_string("traffic"));
        json_object_object_add(json, "success", json_object_new_boolean(all_ok));
        json_object_object_add(json, "message", json_object_new_string(message));
        json_object_object_add(json, "interfaces", interfaces);
        const char *json_str = json_object_to_json_string(json);
        mqtt_send(topic, json_str, (int)strlen(json_str));
        json_object_put(json);
    } else {
        json_object_put(interfaces);
    }
}

static void traffic_poll_all(void) {
    for (int b = 0; b < traffic_config.bundle_count; b++)
        traffic_poll_bundle(&traffic_config.bundles[b]);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

volatile bool running = true;

void signal_handler(int sig __attribute__((unused))) {
    if (running)
        running = false;
}

void process_loop(void) {

    time_t last_heartbeat = 0, last_traffic = 0;

    char status_message[512];

    while (running) {

        if (running && traffic_config.enabled && intervalable(timing_config.traffic_poll_period, &last_traffic, true))
            traffic_poll_all();

        if (running && intervalable(timing_config.heartbeat_period, &last_heartbeat, false))
            publish_daemon_event("heartbeat", do_heartbeat(status_message, sizeof(status_message)), status_message, heartbeat_config.verbose);

        sleep(1);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static bool config_parse_traffic_iface(const char *value, traffic_target_t *target) {
    char buf[512];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *p              = buf;
    char *host_str       = strsep(&p, ":");
    char *community_str  = strsep(&p, ":");
    char *index_str      = strsep(&p, ":");
    char *name_str       = p;
    if (!host_str || !community_str || !index_str || !name_str || !*host_str || !*community_str || !*index_str || !*name_str)
        return false;
    strncpy(target->host, host_str, sizeof(target->host) - 1);
    target->host[sizeof(target->host) - 1] = '\0';
    strncpy(target->community, community_str, sizeof(target->community) - 1);
    target->community[sizeof(target->community) - 1] = '\0';
    target->index                                    = atoi(index_str);
    strncpy(target->name, name_str, sizeof(target->name) - 1);
    target->name[sizeof(target->name) - 1] = '\0';
    target->has_prev                       = false;
    return target->index > 0;
}

static bool config_parse_iface_key(const char *key, char *bundle, size_t bundle_size, int *index) {
    static const char prefix[] = "traffic-iface[";
    size_t prefix_len          = sizeof(prefix) - 1;
    if (strncmp(key, prefix, prefix_len) != 0)
        return false;
    const char *p      = key + prefix_len;
    const char *close1 = strchr(p, ']');
    if (!close1)
        return false;
    size_t name_len = (size_t)(close1 - p);
    if (name_len == 0 || name_len >= bundle_size)
        return false;
    memcpy(bundle, p, name_len);
    bundle[name_len] = '\0';
    if (close1[1] != '[')
        return false;
    p                  = close1 + 2;
    const char *close2 = strchr(p, ']');
    if (!close2 || close2[1] != '\0')
        return false;
    char num[16];
    size_t num_len = (size_t)(close2 - p);
    if (num_len == 0 || num_len >= sizeof(num))
        return false;
    memcpy(num, p, num_len);
    num[num_len] = '\0';
    *index       = atoi(num);
    return *index > 0;
}

static traffic_bundle_t *config_get_or_create_bundle(const char *name) {
    for (int b = 0; b < traffic_config.bundle_count; b++)
        if (strcmp(traffic_config.bundles[b].name, name) == 0)
            return &traffic_config.bundles[b];
    if (traffic_config.bundle_count >= MAX_TRAFFIC_BUNDLES)
        return NULL;
    traffic_bundle_t *bundle = &traffic_config.bundles[traffic_config.bundle_count++];
    strncpy(bundle->name, name, sizeof(bundle->name) - 1);
    bundle->name[sizeof(bundle->name) - 1] = '\0';
    bundle->target_count                   = 0;
    return bundle;
}

bool config_load_traffic_targets(void) {

    traffic_config.target_count = 0;
    traffic_config.bundle_count = 0;

    for (int e = 0; e < config_entry_count; e++) {
        const char *key = config_entries[e].key;
        char bundle_name[MAX_BUNDLE_NAME];
        int index;
        if (!config_parse_iface_key(key, bundle_name, sizeof(bundle_name), &index))
            continue;
        if (traffic_config.target_count >= MAX_TRAFFIC_TARGETS) {
            fprintf(stderr, "config: too many traffic interfaces, ignoring %s\n", key);
            continue;
        }
        traffic_target_t *target = &traffic_config.targets[traffic_config.target_count];
        if (!config_parse_traffic_iface(config_entries[e].value, target)) {
            fprintf(stderr, "config: invalid traffic-iface value '%s' for %s\n", config_entries[e].value, key);
            continue;
        }
        traffic_bundle_t *bundle = config_get_or_create_bundle(bundle_name);
        if (!bundle) {
            fprintf(stderr, "config: too many traffic bundles, ignoring %s\n", key);
            continue;
        }
        if (bundle->target_count >= MAX_TRAFFIC_TARGETS) {
            fprintf(stderr, "config: bundle '%s' has too many interfaces, ignoring %s\n", bundle_name, key);
            continue;
        }
        bundle->target_indices[bundle->target_count++] = traffic_config.target_count;
        traffic_config.target_count++;
    }
    return traffic_config.target_count > 0;
}

bool config_load_settings(const int argc, const char *argv[]) {

    if (!config_load(CONFIG_FILE_DEFAULT, argc, argv, config_options))
        return false;

    timing_config.heartbeat_period    = config_get_integer("heartbeat-period", HEARTBEAT_PERIOD_DEFAULT);
    timing_config.traffic_poll_period = config_get_integer("traffic-poll-period", TRAFFIC_POLL_PERIOD_DEFAULT);

    heartbeat_config.enabled = (timing_config.heartbeat_period > 0);
    heartbeat_config.verbose = config_get_bool("heartbeat-verbose", false);
    if (heartbeat_config.enabled)
        printf("heartbeat: (%d, %s)\n", timing_config.heartbeat_period, heartbeat_config.verbose ? "verbose" : "quiet");

    config_load_traffic_targets();
    traffic_config.enabled = timing_config.traffic_poll_period > 0 && (traffic_config.target_count > 0);
    traffic_config.verbose = config_get_bool("traffic-verbose", true);
    if (traffic_config.enabled)
        for (int b = 0; b < traffic_config.bundle_count; b++) {
            traffic_bundle_t *bundle = &traffic_config.bundles[b];
            printf("traffic: bundle '%s' (%d, %s)\n", bundle->name, timing_config.traffic_poll_period, traffic_config.verbose ? "verbose" : "quiet");
            for (int i = 0; i < bundle->target_count; i++) {
                traffic_target_t *target = &traffic_config.targets[bundle->target_indices[i]];
                printf("traffic:   [%d] '%s/%s' index=%d\n", i + 1, target->host, target->name, target->index);
            }
        }

    if (gethostname(hostname, sizeof(hostname)) != 0)
        strcpy(hostname, "unknown");

    instance_name = config_get_string("name", hostname);

    mqtt_config.server = config_get_string("mqtt-server", MQTT_SERVER_DEFAULT);
    mqtt_config.client = config_get_string("mqtt-client", MQTT_CLIENT_DEFAULT);
    mqtt_config.debug  = false;
    mqtt_topic_prefix  = config_get_string("mqtt-topic-prefix", MQTT_TOPIC_PREFIX_DEFAULT);
    mqtt_enabled       = mqtt_config.server != NULL && (strlen(mqtt_config.server) > 0);

    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

int main(int argc, const char **argv) {

    setbuf(stdout, NULL);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (!config_load_settings(argc, argv)) {
        fprintf(stderr, "failed to load configuration\n");
        return EXIT_FAILURE;
    }
    if (!traffic_config.enabled) {
        fprintf(stderr, "traffic monitoring is disabled, nothing to do\n");
        return EXIT_FAILURE;
    }

    if (mqtt_enabled && !mqtt_begin(&mqtt_config)) {
        fprintf(stderr, "failed to initialize MQTT\n");
        return EXIT_FAILURE;
    }

    snmp_begin();
    publish_daemon_event("startup", true, "daemon started", true);
    process_loop();
    publish_daemon_event("shutdown", true, "daemon stopped", true);
    snmp_end();

    if (mqtt_enabled)
        mqtt_end();

    return EXIT_SUCCESS;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
