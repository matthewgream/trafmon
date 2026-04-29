
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <stdint.h>

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#ifndef SNMP_TIMEOUT_US
#define SNMP_TIMEOUT_US 3000000L
#endif
#ifndef SNMP_RETRIES
#define SNMP_RETRIES 1
#endif
#ifndef SNMP_OID_MAX
#define SNMP_OID_MAX 128
#endif
#ifndef SNMP_STR_MAX
#define SNMP_STR_MAX 256
#endif

typedef struct {
    bool valid;
    uint64_t u64;
    char str[SNMP_STR_MAX];
} snmp_value_t;

typedef struct {
    char oid[SNMP_OID_MAX];
    snmp_value_t value;
} snmp_get_item_t;

typedef bool (*snmp_walk_cb_t)(const char *oid_str, const snmp_value_t *value, void *userdata);

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool snmp_begin(void) {
    init_snmp("traffmon");
    return true;
}

void snmp_end(void) { snmp_shutdown("traffmon"); }

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
static struct snmp_session *__snmp_session_open(const char *host, const char *community, long timeout_us) {
    struct snmp_session session;
    snmp_sess_init(&session);
    session.peername      = (char *)host;
    session.community     = (u_char *)community;
    session.community_len = strlen(community);
    session.version       = SNMP_VERSION_2c;
    session.timeout       = timeout_us;
    session.retries       = SNMP_RETRIES;
    return snmp_open(&session);
}
#pragma GCC diagnostic pop

static void __snmp_extract(const struct variable_list *vars, snmp_value_t *value) {
    value->valid  = true;
    value->u64    = 0;
    value->str[0] = '\0';
    switch (vars->type) {
    case ASN_COUNTER64: {
        const struct counter64 *c = vars->val.counter64;
        value->u64                = ((uint64_t)c->high << 32) | (uint64_t)c->low;
        break;
    }
    case ASN_COUNTER:
    case ASN_GAUGE:
    case ASN_TIMETICKS:
    case ASN_UINTEGER:
        value->u64 = (uint64_t)*vars->val.integer;
        break;
    case ASN_INTEGER:
        value->u64 = (uint64_t)(int64_t)*vars->val.integer;
        break;
    case ASN_OCTET_STR: {
        size_t length = vars->val_len < SNMP_STR_MAX - 1 ? vars->val_len : SNMP_STR_MAX - 1;
        memcpy(value->str, vars->val.string, length);
        value->str[length] = '\0';
        break;
    }
    default:
        value->valid = false;
        break;
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool snmp_get(const char *host, const char *community, snmp_get_item_t *items, int count, char *error_message, size_t error_size) {

    for (int i = 0; i < count; i++) {
        items[i].value.valid  = false;
        items[i].value.u64    = 0;
        items[i].value.str[0] = '\0';
    }

    struct snmp_session *ss = __snmp_session_open(host, community, SNMP_TIMEOUT_US);
    if (!ss) {
        snprintf(error_message, error_size, "session open failed");
        return false;
    }

    struct snmp_pdu *pdu = snmp_pdu_create(SNMP_MSG_GET);
    if (!pdu) {
        snprintf(error_message, error_size, "pdu create failed");
        snmp_close(ss);
        return false;
    }
    for (int i = 0; i < count; i++) {
        oid name[MAX_OID_LEN];
        size_t name_len = MAX_OID_LEN;
        if (!read_objid(items[i].oid, name, &name_len))
            continue;
        snmp_add_null_var(pdu, name, name_len);
    }

    struct snmp_pdu *response = NULL;
    int status                = snmp_synch_response(ss, pdu, &response);
    if (status != STAT_SUCCESS) {
        snprintf(error_message, error_size, "%s", snmp_api_errstring(ss->s_snmp_errno));
        if (response)
            snmp_free_pdu(response);
        snmp_close(ss);
        return false;
    }
    if (response->errstat != SNMP_ERR_NOERROR) {
        snprintf(error_message, error_size, "%s", snmp_errstring((int)response->errstat));
        snmp_free_pdu(response);
        snmp_close(ss);
        return false;
    }

    int idx                       = 0;
    struct variable_list *vars    = response->variables;
    while (vars && idx < count) {
        if (vars->type != SNMP_NOSUCHOBJECT && vars->type != SNMP_NOSUCHINSTANCE && vars->type != SNMP_ENDOFMIBVIEW)
            __snmp_extract(vars, &items[idx].value);
        vars = vars->next_variable;
        idx++;
    }

    snmp_free_pdu(response);
    snmp_close(ss);
    error_message[0] = '\0';
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool snmp_walk(const char *host, const char *community, const char *base_oid_str, snmp_walk_cb_t callback, void *userdata, char *error_message, size_t error_size) {

    oid base_oid[MAX_OID_LEN];
    size_t base_oid_len = MAX_OID_LEN;
    if (!read_objid(base_oid_str, base_oid, &base_oid_len)) {
        snprintf(error_message, error_size, "invalid oid '%s'", base_oid_str);
        return false;
    }

    struct snmp_session *ss = __snmp_session_open(host, community, SNMP_TIMEOUT_US);
    if (!ss) {
        snprintf(error_message, error_size, "session open failed");
        return false;
    }

    oid next_oid[MAX_OID_LEN];
    size_t next_oid_len = base_oid_len;
    memcpy(next_oid, base_oid, base_oid_len * sizeof(oid));

    bool ok = true;
    while (ok) {
        struct snmp_pdu *pdu = snmp_pdu_create(SNMP_MSG_GETNEXT);
        if (!pdu) {
            snprintf(error_message, error_size, "pdu create failed");
            ok = false;
            break;
        }
        snmp_add_null_var(pdu, next_oid, next_oid_len);

        struct snmp_pdu *response = NULL;
        int status                = snmp_synch_response(ss, pdu, &response);
        if (status != STAT_SUCCESS) {
            snprintf(error_message, error_size, "%s", snmp_api_errstring(ss->s_snmp_errno));
            if (response)
                snmp_free_pdu(response);
            ok = false;
            break;
        }
        if (response->errstat != SNMP_ERR_NOERROR) {
            snprintf(error_message, error_size, "%s", snmp_errstring((int)response->errstat));
            snmp_free_pdu(response);
            ok = false;
            break;
        }

        struct variable_list *vars = response->variables;
        if (!vars || vars->type == SNMP_ENDOFMIBVIEW || netsnmp_oid_is_subtree(base_oid, base_oid_len, vars->name, vars->name_length) != 0) {
            snmp_free_pdu(response);
            break;
        }

        char oid_str[SNMP_OID_MAX];
        snprint_objid(oid_str, sizeof(oid_str), vars->name, vars->name_length);

        snmp_value_t value;
        __snmp_extract(vars, &value);
        if (!callback(oid_str, &value, userdata)) {
            snmp_free_pdu(response);
            break;
        }

        memcpy(next_oid, vars->name, vars->name_length * sizeof(oid));
        next_oid_len = vars->name_length;
        snmp_free_pdu(response);
    }

    snmp_close(ss);
    if (ok)
        error_message[0] = '\0';
    return ok;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
