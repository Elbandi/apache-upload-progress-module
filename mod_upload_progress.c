#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <ap_config.h>
#include <http_core.h>
#include <http_log.h>
#include <http_protocol.h>
#include <http_request.h>
#include <apr_version.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include "unixd.h"

#include <mysql.h>
#include <errmsg.h>
#include <mysqld_error.h>

#if APR_HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "ap_backports.h"

#ifndef PROGRESS_ID
#  define PROGRESS_ID "X-Progress-ID"
#endif
#ifndef JSON_CB_PARAM
#  define JSON_CB_PARAM "callback"
#endif
#ifndef UP_DEBUG
#  define UP_DEBUG 0
#endif

#ifndef ARG_ALLOWED_PROGRESSID
#  define ARG_ALLOWED_PROGRESSID "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_:./!{}"
#endif
#ifndef ARG_ALLOWED_JSONPCALLBACK
#  define ARG_ALLOWED_JSONPCALLBACK "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._$"
#endif
#ifndef ARG_MINLEN_PROGRESSID
#  define ARG_MINLEN_PROGRESSID 8
#endif
#ifndef ARG_MAXLEN_PROGRESSID
#  define ARG_MAXLEN_PROGRESSID 64
#endif
#define UP_STRINGIFY(arg) #arg
#define UP_TOSTRING(arg) UP_STRINGIFY(arg)
#define UP_ID_FMT "." UP_TOSTRING(ARG_MAXLEN_PROGRESSID) "s"
#ifndef ARG_MINLEN_JSONPCALLBACK
#  define ARG_MINLEN_JSONPCALLBACK 1
#endif
#ifndef ARG_MAXLEN_JSONPCALLBACK
/* This limit is set by most JS implementations on identifier length */
#  define ARG_MAXLEN_JSONPCALLBACK 64
#endif

#if UP_DEBUG > 0
#  if UP_DEBUG > 1
#    define up_log(m,s,err,srv,fmtstr,...) ap_log_error( m, s, err, srv, "pid:%" APR_PID_T_FMT " " fmtstr, getpid(), ##__VA_ARGS__ )
#  else
#    define up_log(...) ap_log_error( __VA_ARGS__ )
#  endif
#else
#  define up_log(...)
#endif

typedef struct {
    int track_enabled;
    int report_enabled;
    char *track_table;
    char *progress_id;
} DirConfig;

typedef struct upload_progress_node_s{
    apr_off_t length;
    apr_off_t received;
    int err_status;
    time_t started_at;
    apr_off_t speed; /* bytes per second */
    time_t updated_at;
    int done;
    char key[ARG_MAXLEN_PROGRESSID];
} upload_progress_node_t;

typedef struct {
    apr_time_t cache_updated_at;
    upload_progress_node_t node;
} upload_progress_req_t;

typedef struct {
    char *db_host;
    char *db_name;
    char *db_user;
    char *db_pwd;
    char *db_socket;
    unsigned int db_port;
    MYSQL *dbh;

    /* Some MySQL errors are retryable; if we retry the operation
     * by recursing into the same function, we set this so we don't
     * recurse indefinitely if it's a permanent error.
     */
    unsigned char dbh_error_lastchance;
} ServerConfig;

static int upload_progress_cache_write(request_rec *, char *);
static apr_status_t upload_progress_mysql_cleanup(void *);
static apr_status_t upload_progress_cleanup(void *);
static const char *get_progress_id(request_rec *, int *);

extern module AP_MODULE_DECLARE_DATA upload_progress_module;

static server_rec *global_server = NULL;

static inline ServerConfig *get_server_config(server_rec *s)
{
    return (ServerConfig *)ap_get_module_config(s->module_config, &upload_progress_module);
}

static inline DirConfig *get_dir_config(request_rec *r)
{
    return (DirConfig *)ap_get_module_config(r->per_dir_config, &upload_progress_module);
}

static inline upload_progress_req_t *get_request_config(request_rec *r)
{
    return (upload_progress_req_t *)ap_get_module_config(r->request_config, &upload_progress_module);
}

static int upload_progress_handle_request(request_rec *r)
{
    server_rec *server = r->server;
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "upload_progress_handle_request()");

    DirConfig *dir = get_dir_config(r);

    if (!dir || (dir->track_enabled <= 0)) {
        return DECLINED;
    }
    if (r->method_number != M_POST) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                     "Upload Progress: Non-POST request in trackable location: %s.", r->uri);
        return DECLINED;
    }
    upload_progress_req_t *ri = get_request_config(r);
    if (ri) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                     "Upload Progress: Request info exists in trackable location: %s.", r->uri);
        return DECLINED;
    }

    int param_error;
    char *query;
    const char *id = get_progress_id(r, &param_error);

    if (id) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                     "Upload Progress: Upload id='%s' in trackable location: %s.", id, r->uri);

        upload_progress_req_t *reqinfo = (upload_progress_req_t *)apr_pcalloc(r->pool,
                                       sizeof(upload_progress_req_t));
        upload_progress_node_t *node = NULL;
        if (reqinfo) {
            reqinfo->cache_updated_at = apr_time_now();
            node = &(reqinfo->node);

            strncpy(node->key, id, ARG_MAXLEN_PROGRESSID);
            time_t t = time(NULL);

            node->received = 0;
            node->done = 0;
            node->err_status = 0;
            node->started_at = t;
            node->speed = 0;
            node->updated_at = t;
            const char *content_length = apr_table_get(r->headers_in, "Content-Length");
            node->length = 1;
            /* Content-Length is missing is case of chunked transfer encoding */
            if (content_length)
                sscanf(content_length, "%" APR_OFF_T_FMT, &(node->length));
        }

        ap_set_module_config(r->request_config, &upload_progress_module, reqinfo);
        apr_pool_cleanup_register(r->pool, r, upload_progress_cleanup, apr_pool_cleanup_null);
        ap_add_input_filter("UPLOAD_PROGRESS", NULL, r, r->connection);

        if (node) {
            query = (char *) apr_psprintf(r->pool, "INSERT IGNORE INTO %s (id, length, err_status, "
                "received, done, started_at, speed, updated_at)"
                " VALUES ('%s', %" APR_OFF_T_FMT ", %d, %" APR_OFF_T_FMT ", %d, %ld, %" APR_OFF_T_FMT ", %ld);",
                dir->track_table, node->key, node->length, node->err_status, node->received,
                node->done, node->started_at, node->speed, node->updated_at);
            if (upload_progress_cache_write(r, query) < 0) {
                return HTTP_INTERNAL_SERVER_ERROR;
            }
        }

    } else if (param_error < 0) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                     "Upload Progress: Upload with invalid ID in trackable location: %s.", r->uri);
        /*
        return HTTP_BAD_REQUEST;
        return HTTP_NOT_FOUND;
        */

    } else {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                     "Upload Progress: Upload without ID in trackable location: %s.", r->uri);
    }

    return DECLINED;
}

static const char *report_upload_progress_cmd(cmd_parms *cmd, void *config, int arg)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "report_upload_progress_cmd()");
    DirConfig *dir = (DirConfig *)config;
    dir->report_enabled = arg ? 1 : -1;
    return NULL;
}

static const char *track_upload_progress_cmd(cmd_parms *cmd, void *config, int arg)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "track_upload_progress_cmd()");
    DirConfig *dir = (DirConfig *)config;
    dir->track_enabled = arg ? 1 : -1;
    return NULL;
}

static const char *table_upload_progress_cmd(cmd_parms *cmd, void *config, const char *arg)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "table_upload_progress_cmd()");
    DirConfig *dir = (DirConfig *)config;
    dir->track_table = (char *)arg;
    return NULL;
}

static const char *key_upload_progress_cmd(cmd_parms *cmd, void *config, const char *arg)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "key_upload_progress_cmd()");
    DirConfig *dir = (DirConfig *)config;
    dir->progress_id = (char *)arg;
    return NULL;
}

static const char* upload_progress_mysql_info(cmd_parms *cmd,
                            void *dummy, const char *host, const char *user,
                            const char *pwd)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_mysql_info()");
    ServerConfig *config = get_server_config(cmd->server);

    if (*host != '.') {
        config->db_host = (char *)host;
    }

    if (*user != '.') {
        config->db_user = (char *)user;
    }

    if (*pwd != '.') {
        config->db_pwd = (char *)pwd;
    }

    return NULL;
}

static const char *upload_progress_mysql_socket(cmd_parms *cmd, void *dummy, const char *sock)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_mysql_socket()");
    ServerConfig *config = get_server_config(cmd->server);
    config->db_socket = (char *)socket;
    return NULL;
}

/* Set the server-wide database server port.
 */
static const char *upload_progress_mysql_port(cmd_parms *cmd, void *dummy, const char *port)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_mysql_port()");
    ServerConfig *config = get_server_config(cmd->server);
    config->db_port = (unsigned int) atoi(port);
    return NULL;
}

static const char *upload_progress_mysql_db(cmd_parms *cmd, void *dummy, const char *db)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_mysql_db()");
    ServerConfig *config = get_server_config(cmd->server);
    config->db_name = (char *)db;
    return NULL;
}

static void *upload_progress_create_dir_config(apr_pool_t *p, char *dirspec)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_create_dir_config()");
    DirConfig *dir = (DirConfig *)apr_pcalloc(p, sizeof(DirConfig));
    if (dir) {
        dir->track_enabled = 0;
        dir->report_enabled = 0;
        dir->track_table = "uploads";
        dir->progress_id = PROGRESS_ID;
    }
    return dir;
}

static void *upload_progress_merge_dir_config(apr_pool_t *p, void *basev, void *overridev)
{
    DirConfig *new = (DirConfig *)apr_pcalloc(p, sizeof(DirConfig));
    DirConfig *override = (DirConfig *)overridev;
    DirConfig *base = (DirConfig *)basev;
    new->track_enabled = (override->track_enabled == 0) ? base->track_enabled :
                             (override->track_enabled > 0 ? 1 : -1);
    new->report_enabled = (override->report_enabled == 0) ? base->report_enabled :
                              (override->report_enabled > 0 ? 1 : -1);
    new->track_table = (override->track_table == NULL) ? base->track_table :
                            override->track_table;
    new->progress_id = (override->progress_id == NULL) ? base->progress_id :
                            override->progress_id;
    return new;
}

static void *upload_progress_create_server_config(apr_pool_t *p, server_rec *s)
{
    if (!global_server) global_server = s;
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_create_server_config()");
    ServerConfig *config = (ServerConfig *)apr_pcalloc(p, sizeof(ServerConfig));
    if (config) {
        config->db_host = "localhost";
        config->db_user = "root";
        config->db_pwd = "";
        config->db_name = "upload";
        config->db_socket = NULL;
        config->db_port = 3306;

        config->dbh_error_lastchance = 0;
        config->dbh = NULL;
        apr_pool_cleanup_register(p, config, upload_progress_mysql_cleanup, apr_pool_cleanup_null);
    }
    return config;
}

static void *upload_progress_merge_server_config(apr_pool_t *p, void *basev,
                                    void *overridesv)
{
    return basev;
}

static int read_request_status(request_rec *r)
{
    int status;

    if (r) {
        /* error status rendered in status line is preferred because passenger
           clobbers request_rec->status when exception occurs */
        /* FIXME: Shouldn't we read r->status instead as it's already preparsed? */
        status = r->status_line ? atoi(r->status_line) : 0;
        if (!ap_is_HTTP_VALID_RESPONSE(status))
            status = r->status;
        return status;
    } else {
        return 0;
    }
}

static apr_status_t upload_progress_mysql_result_cleanup(void *result)
{
    mysql_free_result((MYSQL_RES *) result);
    return APR_SUCCESS;
}

/* Make a MySQL database link open and ready for business.  Returns 0 on
 * success, or the MySQL error number which caused the failure if there was
 * some sort of problem.
 */
static int open_upload_dblink(request_rec *r, ServerConfig *config)
{
    server_rec *server = r->server;
    void (*sigpipe_handler)();
    unsigned long client_flag = 0;
#if MYSQL_VERSION_ID >= 50013
    my_bool do_reconnect = 1;
#endif

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "Opening DB connection");

    /* MySQL likes to throw the odd SIGPIPE now and then - ignore it for now */
    sigpipe_handler = signal(SIGPIPE, SIG_IGN);

    config->dbh = mysql_init(NULL);

    if (!mysql_real_connect(config->dbh, config->db_host, config->db_user, config->db_pwd, config->db_name, config->db_port, config->db_socket, client_flag)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
             "Connection error: %s", mysql_error(config->dbh));
        errno = mysql_errno(config->dbh);
        mysql_close(config->dbh);
        config->dbh = NULL;
        return errno;
    }

#if MYSQL_VERSION_ID >= 50013
    /* The default is no longer to automatically reconnect on failure,
     * (as of 5.0.3) so we have to set that option here.  The option is
     * available from 5.0.13.  */
    mysql_options(config->dbh, MYSQL_OPT_RECONNECT, &do_reconnect);
#endif

    signal(SIGPIPE, sigpipe_handler);

    /* W00t!  We made it! */
    return 0;
}

/* Run a query against the database.  Doesn't assume nearly anything about
 * the state of affairs regarding the database connection.
 * Returns 0 on a successful query run, or the MySQL error number on
 * error.  It is the responsibility of the calling function to retrieve any
 * data which may have been obtained through the running of this function.
 */
static int safe_mysql_query(request_rec *r, char *query, ServerConfig *config)
{
    server_rec *server = r->server;
    int error = CR_UNKNOWN_ERROR;
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "safe_mysql_query()");

    up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "dbh is %p", config->dbh);
    if (config->dbh_error_lastchance)
    {
        up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "Last chance, bub");
    }
    else
    {
        up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "Ordinary query");
    }

    if (!config->dbh) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
            "No DB connection open - firing one up");
        if ((error = open_upload_dblink(r, config))) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                "open_auth_dblink returned %i", error);
            return error;
        }

        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
            "Correctly opened a new DB connection");
    }

    up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "Running query: [%s]", query);

    if (mysql_query(config->dbh, query)) {
        error = mysql_errno(config->dbh);

        up_log(APLOG_MARK, APLOG_DEBUG, 0, server,
            "Query maybe-failed: %s (%i), lastchance=%i", mysql_error(config->dbh), error, config->dbh_error_lastchance);
        up_log(APLOG_MARK, APLOG_DEBUG, 0, server,
            "Error numbers of interest are %i (SG) and %i (SL)",
            CR_SERVER_GONE_ERROR, CR_SERVER_LOST);
        if (config->dbh_error_lastchance)
        {
            /* No matter what error, we're moving out */
            return error;
        }
        else if (error == CR_SERVER_LOST || error == CR_SERVER_GONE_ERROR)
        {
            /* Try again, once more only */
            config->dbh_error_lastchance = 1;
            config->dbh = NULL;
            up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "Retrying query");
            return safe_mysql_query(r, query, config);
        }
        else
        {
            return error;
        }
    }

    return 0;
}

/* Store the result of a query in a result structure, and return it.  It's
 * "safe" in the fact that a cleanup function is registered for the structure
 * so it will be tidied up after the request.
 * Returns the result data on success, or NULL if there was no data to retrieve.
 */
static MYSQL_RES *safe_mysql_store_result(apr_pool_t *p, ServerConfig *config)
{
    MYSQL_RES *result;
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "safe_mysql_store_result()");

    result = mysql_store_result(config->dbh);

    if (result) {
        apr_pool_cleanup_register(p, (void *) result, upload_progress_mysql_result_cleanup, upload_progress_mysql_result_cleanup);
    }

    return result;
}


static int track_upload_progress(ap_filter_t *f, apr_bucket_brigade *bb,
                           ap_input_mode_t mode, apr_read_type_e block,
                           apr_off_t readbytes)
{
    server_rec *server = f->r->server;
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "track_upload_progress()");

    char *query;
    apr_status_t rv = ap_get_brigade(f->next, bb, mode, block, readbytes);

    upload_progress_req_t *reqinfo = get_request_config(f->r);
    if (!reqinfo) {
        up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "Request config not available");
        return rv;
    }
    upload_progress_node_t *node = &(reqinfo->node);

    time_t t = time(NULL);
    node->updated_at = t;
    if (rv == APR_SUCCESS) {
        apr_off_t length;
        apr_brigade_length(bb, 1, &length);
        node->received += length;
        if (node->received > node->length) /* handle chunked tranfer */
            node->length = node->received;
        int upload_time = t - node->started_at;
        if (upload_time > 0) {
            node->speed = node->received / upload_time;
        }
    } else {
        node->err_status = read_request_status(f->r);
        reqinfo->cache_updated_at = 0; /* force cache update */
    }

    apr_time_t now = apr_time_now();
    if ((now - reqinfo->cache_updated_at) > apr_time_from_msec(500)) {
        DirConfig *dir = get_dir_config(f->r);
        reqinfo->cache_updated_at = now;
        query = (char *) apr_psprintf(f->r->pool, "UPDATE %s SET err_status = %d, received = %" APR_OFF_T_FMT ", "
            "length = %" APR_OFF_T_FMT ", speed = %" APR_OFF_T_FMT ", updated_at = %ld WHERE id = '%s';",
            dir->track_table, node->err_status, node->received, node->length, node->speed, node->updated_at, node->key);
        upload_progress_cache_write(f->r, query);
    }

    return rv;
}

static int check_request_argument(const char *value, int len, char *allowed, int minlen, int maxlen) {
    /* Check the length of the argument */
    if (len > maxlen) return -1;
    if (len < minlen) return -2;
    /* If no whitelist given, assume everything whitelisted */
    if (!allowed) return 0;
    /* Check each char to be in the whitelist */
    if (strspn(value, allowed) < len) return -3;
    return 0;
}

static char *get_param_value(char *p, const char *param_name, int *len) {
    char pn1[3] = {toupper(param_name[0]), tolower(param_name[0]), 0};
    int pn_len = strlen(param_name);
    char *val_end;
    static char *param_sep = "&";

    while (p) {
        if ((strncasecmp(p, param_name, pn_len) == 0) && (p[pn_len] == '='))
            break;
        if (*p) p++;
        p = strpbrk(p, pn1);
    }
    if (p) {
        p += (pn_len + 1);
        *len = strcspn(p, param_sep);
    }
    return p;
}

static const char *get_progress_id(request_rec *r, int *param_error) {
    DirConfig *dir = get_dir_config(r);
    int len;

    /* try to find progress id in http headers */
    const char *id  = apr_table_get(r->headers_in, dir->progress_id);
    if (id) {
        *param_error = check_request_argument(id, strlen(id), ARG_ALLOWED_PROGRESSID,
            ARG_MINLEN_PROGRESSID, ARG_MAXLEN_PROGRESSID);
        if (*param_error) return NULL;
        return id;
    }

    /* if progress id not found in headers, check request args (query string) */
    id = get_param_value(r->args, dir->progress_id, &len);
    if (id) {
        *param_error = check_request_argument(id, len, ARG_ALLOWED_PROGRESSID,
            ARG_MINLEN_PROGRESSID, ARG_MAXLEN_PROGRESSID);
        if (*param_error) return NULL;
        return apr_pstrndup(r->connection->pool, id, len);
    }

    *param_error = 1; /* not found */
    return NULL;
}

static const char *get_json_callback_param(request_rec *r, int *param_error) {
    char *val;
    int len;

    val = get_param_value(r->args, JSON_CB_PARAM, &len);
    if (val) {
        *param_error = check_request_argument(val, len, ARG_ALLOWED_JSONPCALLBACK,
            ARG_MINLEN_JSONPCALLBACK, ARG_MAXLEN_JSONPCALLBACK);
        if (*param_error) return NULL;
        return apr_pstrndup(r->connection->pool, val, len);
    }

    *param_error = 1; /* not found */
    return NULL;
}

static inline int check_node(upload_progress_node_t *node, const char *key) {
    return ((node) && (strncasecmp(node->key, key, ARG_MAXLEN_PROGRESSID) == 0)) ? 1 : 0;
}

static apr_status_t upload_progress_mysql_cleanup(void *data)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_mysql_cleanup()");
    ServerConfig *config = data;

    if (config->dbh) {
        mysql_close(config->dbh);
        config->dbh = NULL;
    }
    return APR_SUCCESS;
}

static apr_status_t upload_progress_cleanup(void *data)
{
    request_rec *r = (request_rec *)data;
    server_rec *server = r->server;
    DirConfig *dir = get_dir_config(r);
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "upload_progress_cleanup()");

    char *query;
    upload_progress_req_t *reqinfo = get_request_config(r);
    if (!reqinfo) {
        up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "Request config not available");
        return APR_SUCCESS;
    }
    upload_progress_node_t *node = &(reqinfo->node);
    node->err_status = read_request_status(r);
    node->updated_at = time(NULL);
    node->done = 1;
    query = (char *) apr_psprintf(r->pool, "UPDATE %s SET err_status = %d, "
            "done = %d, updated_at = %ld WHERE id = '%s';",
            dir->track_table, node->err_status, node->done, node->updated_at, node->key);
    upload_progress_cache_write(r, query);

    return APR_SUCCESS;
}

static int upload_progress_cache_write(request_rec *r, char *query)
{
    server_rec *server = r->server;
    ServerConfig *config = get_server_config(server);
    int rv;

    if (!query) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
            "Failed to create query string - we're in deep poopy");
        return -1;
    }
    if ((rv = safe_mysql_query(r, query, config))) {
        if (config->dbh)
        {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
                "Query call failed: %s (%i)", mysql_error(config->dbh), rv);
        }

        ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Failed query was: [%s]", query);
        return -1;
    }

    return 0;
}

static int reportuploads_handler(request_rec *r)
{ 
    server_rec *server = r->server;
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, server, "reportuploads_handler()");

    int param_error;
    char *query;
    MYSQL_RES *result;
    MYSQL_ROW row;
    int rv;
    char *response;
    DirConfig *dir = get_dir_config(r);

    if (!dir || (dir->report_enabled <= 0)) {
        return DECLINED;
    }
    if (r->method_number != M_GET) {
        return HTTP_METHOD_NOT_ALLOWED;
    }

    /* get the tracking id if any */
    const char *id = get_progress_id(r, &param_error);

    if (id == NULL) {
        if (param_error < 0) {
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, server,
                         "Upload Progress: Report requested with invalid id. uri=%s", r->uri);
            return HTTP_BAD_REQUEST;
        } else {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                         "Upload Progress: Report requested without id. uri=%s", r->uri);
            return HTTP_NOT_FOUND;
        }
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                 "Upload Progress: Report requested with id='%s'. uri=%s", id, r->uri);

    ServerConfig *config = get_server_config(server);

    query = (char *) apr_psprintf(r->pool, "SELECT id, err_status, length, received, done, speed, started_at, updated_at "
                "FROM %s WHERE id = '%s'", dir->track_table, id);

    if (!query) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
                    "Failed to create query string - we're in deep poopy");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if ((rv = safe_mysql_query(r, query, config))) {
        if (config->dbh) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
                "Query call failed: %s (%i)", mysql_error(config->dbh), rv);
        }
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "Failed query was: [%s]", query);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    result = safe_mysql_store_result(r->pool, config);
    if (!result) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
                    "Failed to get MySQL result structure : %s", mysql_error(config->dbh));
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    ap_set_content_type(r, "application/json");

    apr_table_set(r->headers_out, "Expires", "Mon, 28 Sep 1970 06:00:00 GMT");
    apr_table_set(r->headers_out, "Cache-Control", "no-cache");

    /* There are 4 possibilities
     * request not yet started: found = false
     * request in error:        err_status >= NGX_HTTP_SPECIAL_RESPONSE
     * request finished:        done = true
     * request not yet started but registered:        length==0 && rest ==0
     * request in progress:     rest > 0
     */

    if (!result || (row=mysql_fetch_row(result))==NULL || !row[0]) {
        response = apr_psprintf(r->pool, "{ \"state\" : \"starting\", \"uuid\" : \"%s\" }", id);
    } else {
        int err_status = atoi(row[1]);
        if (err_status >= HTTP_BAD_REQUEST  ) {
            response = apr_psprintf(r->pool, "{ "
                "\"state\": \"error\", "
                "\"status\": %s, "
                "\"uuid\": \"%s\" "
                "}", row[1], id);
        } else if (strcmp(row[4], "1") == 0) {
            response = apr_psprintf(r->pool, "{ "
                "\"state\": \"done\", "
                "\"size\": %s, "
                "\"speed\": %s, "
                "\"started_at\": %s, "
                "\"completed_at\": %s, "
                "\"uuid\": \"%s\" "
                "}", row[2], row[5], row[6], row[7], id);
        } else if (strcmp(row[3], "0") == 0) {
            response = apr_psprintf(r->pool, "{ \"state\" : \"starting\", \"uuid\" : \"%s\" }", id);
        } else {
            time_t eta = 0, t = time(NULL);
            time_t started_at = atoi(row[6]);
            char *err;
            apr_off_t speed;
            apr_off_t length;
            if (apr_strtoff(&speed, row[5], &err, 10) || *err) {
                speed = 0;
            }
            if (apr_strtoff(&length, row[2], &err, 10) || *err) {
                length = 0;
            }
            if (speed > 0) eta = started_at + length / speed;
            if (eta <= t) eta = t + 1;
            response = apr_psprintf(r->pool, "{ "
                "\"state\": \"uploading\", "
                "\"received\": %s, "
                "\"size\": %s, "
                "\"speed\": %s, "
                "\"started_at\": %s, "
                "\"eta\": %li, "
                "\"uuid\": \"%s\" "
                "}", row[3], row[2], row[5], row[6], eta, id);
        }
    }

    char *completed_response;

    /* get the jsonp callback if any */
    const char *jsonp = get_json_callback_param(r, &param_error);

    if (param_error < 0) {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, server,
                     "Upload Progress: Report requested with invalid JSON-P callback. uri=%s", r->uri);
        return HTTP_BAD_REQUEST;
    }

    up_log(APLOG_MARK, APLOG_DEBUG, 0, server,
                       "Upload Progress: JSON-P callback: %s.", jsonp);

    // fix up response for jsonp request, if needed
    if (jsonp) {
        completed_response = apr_psprintf(r->pool, "%s(%s);\r\n", jsonp, response);
    } else {
        completed_response = apr_psprintf(r->pool, "%s\r\n", response);
    }

    ap_rputs(completed_response, r);

    return OK;
}

static const command_rec upload_progress_cmds[] =
{
    AP_INIT_FLAG("TrackUploads", track_upload_progress_cmd, NULL, OR_AUTHCFG,
                 "Track upload progress in this location"),
    AP_INIT_FLAG("ReportUploads", report_upload_progress_cmd, NULL, OR_AUTHCFG,
                 "Report upload progress in this location"),
    AP_INIT_TAKE1("TrackTable", table_upload_progress_cmd, NULL, OR_AUTHCFG,
                 "The name of table where uploads are saved"),
    AP_INIT_TAKE1("TrackID", key_upload_progress_cmd, NULL, OR_AUTHCFG,
                 "The name of progress id"),
    AP_INIT_TAKE3("Upload_Progress_MySQL_Info", upload_progress_mysql_info, NULL, RSRC_CONF,
                  "host, user, password of the MySQL database"),
    AP_INIT_TAKE1("Upload_Progress_MySQL_DefaultPort",    upload_progress_mysql_port, NULL, RSRC_CONF,
                  "Default MySQL server port"),
    AP_INIT_TAKE1("Upload_Progress_MySQL_DefaultSocket", upload_progress_mysql_socket, NULL, RSRC_CONF,
                  "Default MySQL server socket"),
    AP_INIT_TAKE1("Upload_Progress_MySQL_Database", upload_progress_mysql_db, NULL, RSRC_CONF,
                  "database for MySQL upload progress" ),
    { NULL }
};

static void upload_progress_register_hooks (apr_pool_t *p)
{
/**/up_log(APLOG_MARK, APLOG_DEBUG, 0, global_server, "upload_progress_register_hooks()");

    ap_hook_fixups(upload_progress_handle_request, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_handler(reportuploads_handler, NULL, NULL, APR_HOOK_FIRST);
    ap_register_input_filter("UPLOAD_PROGRESS", track_upload_progress, NULL, AP_FTYPE_RESOURCE);
}

module AP_MODULE_DECLARE_DATA upload_progress_module =
{
    STANDARD20_MODULE_STUFF,
    upload_progress_create_dir_config,
    upload_progress_merge_dir_config,
    upload_progress_create_server_config,
    NULL, // upload_progress_merge_server_config,
    upload_progress_cmds,
    upload_progress_register_hooks,      /* callback for registering hooks */
};
