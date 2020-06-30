/*
 *	server.c
 *
 *	database server functions
 *
 *	Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/server.c
 */

#include "postgres.h"
#include "knl/knl_variable.h"

#include "pg_upgrade.h"

#define BUILD_RETRY 30

static PGconn* get_db_conn(ClusterInfo* cluster, const char* db_name);

bool check_is_stanby_instance(ClusterInfo* cluster)
{
    PGconn* conn = get_db_conn(cluster, "template1");

    if (conn == NULL || PQstatus(conn) != CONNECTION_OK) {
        if (NULL != strstr(PQerrorMessage(conn), "can not accept connection in standby mode")) {
            if (conn)
                PQfinish(conn);
            return true;
        }
    }

    if (conn)
        PQfinish(conn);

    return false;
}

/*
 * connectToServer()
 *
 *	Connects to the desired database on the designated server.
 *	If the connection attempt fails, this function logs an error
 *	message and calls exit() to kill the program.
 */
PGconn* connectToServer(ClusterInfo* cluster, const char* db_name)
{
    PGconn* conn = get_db_conn(cluster, db_name);

    if (conn == NULL || PQstatus(conn) != CONNECTION_OK) {
        pg_log(PG_REPORT, "connection to database failed: %s\n", PQerrorMessage(conn));

        if (conn)
            PQfinish(conn);

        printf("Failure, exiting\n");
        exit(1);
    }

    return conn;
}

/*
 * get_db_conn()
 *
 * get database connection, using named database + standard params for cluster
 */
static PGconn* get_db_conn(ClusterInfo* cluster, const char* db_name)
{
    char conn_opts[2 * NAMEDATALEN + MAXPGPATH + 100];
    int nRet = 0;

    if (cluster->sockdir) {
        nRet = snprintf_s(conn_opts,
            sizeof(conn_opts),
            sizeof(conn_opts) - 1,
            "dbname = '%s' user = '%s' host = '%s' port = %d",
            db_name,
            cluster->user,
            cluster->sockdir,
            cluster->port);
    } else {
        nRet = snprintf_s(conn_opts,
            sizeof(conn_opts),
            sizeof(conn_opts) - 1,
            "dbname = '%s' user = '%s' port = %d",
            db_name,
            cluster->user,
            cluster->port);
    }
    securec_check_ss_c(nRet, "\0", "\0");

    return PQconnectdb(conn_opts);
}

/*
 * cluster_conn_opts()
 *
 * Return standard command-line options for connecting to this cluster when
 * using psql, pg_dump, etc.  Ideally this would match what get_db_conn()
 * sets, but the utilities we need aren't very consistent about the treatment
 * of database name options, so we leave that out.
 *
 * Note result is in static storage, so use it right away.
 */
char* cluster_conn_opts(ClusterInfo* cluster)
{
    static char conn_opts[MAXPGPATH + NAMEDATALEN + 100];
    int nRet = 0;

    if (cluster->sockdir) {
        nRet = snprintf_s(conn_opts,
            sizeof(conn_opts),
            sizeof(conn_opts) - 1,
            "--host \"%s\" --port %d --username \"%s\"",
            cluster->sockdir,
            cluster->port,
            cluster->user);
    } else {
        nRet = snprintf_s(conn_opts,
            sizeof(conn_opts),
            sizeof(conn_opts) - 1,
            "--port %d --username \"%s\"",
            cluster->port,
            cluster->user);
    }
    securec_check_ss_c(nRet, "\0", "\0");

    return conn_opts;
}

/*
 * executeQueryOrDie()
 *
 *	Formats a query string from the given arguments and executes the
 *	resulting query.  If the query fails, this function logs an error
 *	message and calls exit() to kill the program.
 */
PGresult* executeQueryOrDie(PGconn* conn, const char* fmt, ...)
{
    static char command[8192];
    va_list args;
    PGresult* result = NULL;
    ExecStatusType status;
    int nRet = 0;

    va_start(args, fmt);
    nRet = vsnprintf_s(command, sizeof(command), sizeof(command), fmt, args);
    securec_check_ss_c(nRet, "\0", "\0");
    va_end(args);

    pg_log(PG_VERBOSE, "executing: %s\n", command);
    result = PQexec(conn, command);
    status = PQresultStatus(result);

    if ((status != PGRES_TUPLES_OK) && (status != PGRES_COMMAND_OK)) {
        pg_log(PG_REPORT, "SQL command failed\n%s\n%s\n", command, PQerrorMessage(conn));
        PQclear(result);
        PQfinish(conn);
        printf("Failure, exiting\n");
        exit(1);
    } else
        return result;
}

/*
 * get_major_server_version()
 *
 * gets the version (in unsigned int form) for the given datadir. Assumes
 * that datadir is an absolute path to a valid pgdata directory. The version
 * is retrieved by reading the PG_VERSION file.
 */
uint32 get_major_server_version(ClusterInfo* cluster)
{
    FILE* version_fd = NULL;
    char ver_filename[MAXPGPATH];
    int integer_version = 0;
    int fractional_version = 0;
    int nRet = 0;

    nRet = snprintf_s(ver_filename, sizeof(ver_filename), sizeof(ver_filename) - 1, "%s/PG_VERSION", cluster->pgdata);
    securec_check_ss_c(nRet, "\0", "\0");

    if ((version_fd = fopen(ver_filename, "r")) == NULL)
        pg_log(PG_FATAL, "could not open version file: %s\n", ver_filename);

    if (fscanf(version_fd, "%63s", cluster->major_version_str) == 0 ||
        sscanf(cluster->major_version_str, "%d.%d", &integer_version, &fractional_version) != 2)
        pg_log(PG_FATAL, "could not get version from %s\n", cluster->pgdata);

    fclose(version_fd);

    return (100 * integer_version + fractional_version) * 100;
}

static void stop_postmaster_atexit(void)
{
    stop_postmaster(true);
}

void start_postmaster(ClusterInfo* cluster)
{
    char cmd[MAXPGPATH * 4 + 1000];
    PGconn* conn = NULL;
    static bool exit_hook_registered = false;
    bool pg_ctl_return = false;
    char socket_string[MAXPGPATH + 200];
    int i;
    int nRet = 0;

    if (!exit_hook_registered) {
        atexit(stop_postmaster_atexit);
        exit_hook_registered = true;
    }

    socket_string[0] = '\0';

#ifdef HAVE_UNIX_SOCKETS
    /* prevent TCP/IP connections, restrict socket access */
    nRet = strcat_s(socket_string, sizeof(socket_string), " -c listen_addresses='' -c unix_socket_permissions=0700");
    securec_check_c(nRet, "\0", "\0");
    /* Have a sockdir?  Tell the postmaster. */
    if (cluster->sockdir) {
        nRet = snprintf_s(socket_string + strlen(socket_string),
            sizeof(socket_string) - strlen(socket_string),
            sizeof(socket_string) - strlen(socket_string) - 1,
            " -c %s='%s'",
            (GET_MAJOR_VERSION(cluster->major_version) < 903) ? "unix_socket_directory" : "unix_socket_directories",
            cluster->sockdir);
        securec_check_ss_c(nRet, "\0", "\0");
    }
#endif

    /*
     * Using autovacuum=off disables cleanup vacuum and analyze, but freeze
     * vacuums can still happen, so we set autovacuum_freeze_max_age to its
     * maximum.  We assume all datfrozenxid and relfrozen values are less than
     * a gap of 2000000000 from the current xid counter, so autovacuum will
     * not touch them.
     *
     *	synchronous_commit=off improves object creation speed, and we only
     *	modify the new cluster, so only use it there.  If there is a crash,
     *	the new cluster has to be recreated anyway.
     */
    if (os_info.is_root_user) {
        nRet = snprintf_s(cmd,
            sizeof(cmd),
            sizeof(cmd) - 1,
            "sudo -u %s LD_LIBRARY_PATH=\"%s/../lib\" sh -c \"" /*QUOTE - START*/
            "\\\"%s/gs_ctl\\\" -w -l \\\"%s\\\" -D \\\"%s\\\" -Z %s -o \\\"-p %d -b -c password_policy=0 -c "
            "support_extended_features=on %s %s%s\\\" start"
            "\"" /*QUOTE - END*/,
            cluster->user,
            cluster->bindir,
            cluster->bindir,
            SERVER_LOG_FILE,
            cluster->pgconfig,
            cluster == &new_cluster ? "restoremode" : cluster_type,
            cluster->port,
            (cluster == &new_cluster) ? " -c synchronous_commit=off" : "",
            cluster->pgopts ? cluster->pgopts : "",
            socket_string);
    } else {
        nRet = snprintf_s(cmd,
            sizeof(cmd),
            sizeof(cmd) - 1,
            "export LD_LIBRARY_PATH=%s/../lib:\\$LD_LIBRARY_PATH; "
            "\"%s/gs_ctl\" -w -l \"%s\" -D \"%s\" -Z %s -o \"-p %d -c password_policy=0 -c "
            "support_extended_features=on -b%s %s%s\" start",
            cluster->bindir,
            cluster->bindir,
            SERVER_LOG_FILE,
            cluster->pgconfig,
            cluster == &new_cluster ? "restoremode" : cluster_type,
            cluster->port,
            (cluster == &new_cluster) ? " -c synchronous_commit=off" : "",
            cluster->pgopts ? cluster->pgopts : "",
            socket_string);
    }
    securec_check_ss_c(nRet, "\0", "\0");

    /*
     * Don't throw an error right away, let connecting throw the error because
     * it might supply a reason for the failure.
     */
    for (i = 0; i < BUILD_RETRY; i++) {
        pg_ctl_return = exec_prog(SERVER_START_LOG_FILE,
            /* pass both file names if they differ */
            (strcmp(SERVER_LOG_FILE, SERVER_START_LOG_FILE) != 0) ? SERVER_LOG_FILE : NULL,
            false,
            "%s",
            cmd);

        if (pg_ctl_return) {
            break;
        }
        sleep(60);
    }

    /* Check to see if we can connect to the server; if not, report it. */
    if ((conn = get_db_conn(cluster, "template1")) == NULL || PQstatus(conn) != CONNECTION_OK) {
        pg_log(PG_REPORT, "\nconnection to database failed: %s\n", PQerrorMessage(conn));
        if (conn)
            PQfinish(conn);
        pg_log(PG_FATAL,
            "could not connect to %s postmaster started with the command:\n"
            "%s\n",
            CLUSTER_NAME(cluster),
            cmd);
    }
    PQfinish(conn);

    /* If the connection didn't fail, fail now */
    if (!pg_ctl_return)
        pg_log(PG_FATAL, "gs_ctl failed to start the %s server, or connection failed\n", CLUSTER_NAME(cluster));

    os_info.running_cluster = cluster;
}

void start_postmaster_in_replication_mode(ClusterInfo* cluster)
{
    char cmd[MAXPGPATH * 4 + 1000];
    bool exit_hook_registered = false;
    bool pg_ctl_return = false;
    int i;
    int nRet = 0;

    if (!exit_hook_registered) {
        atexit(stop_postmaster_atexit);
        exit_hook_registered = true;
    }

    if (os_info.is_root_user) {
        nRet = snprintf_s(cmd,
            sizeof(cmd),
            sizeof(cmd) - 1,
            "sudo -u %s LD_LIBRARY_PATH=\"%s/../lib\" sh -c \"" /*QUOTE - START*/
            "\\\"%s/gs_ctl\\\" -w -l \\\"%s\\\" -D \\\"%s\\\" -Z %s -M primary -o \\\"-b%s %s\\\" start"
            "\"" /*QUOTE - END*/,
            cluster->user,
            cluster->bindir,
            cluster->bindir,
            SERVER_LOG_FILE,
            cluster->pgconfig,
            cluster == &new_cluster ? "restoremode" : cluster_type,
            (cluster == &new_cluster) ? " -c synchronous_commit=off" : "",
            cluster->pgopts ? cluster->pgopts : "");
    } else {
        nRet = snprintf_s(cmd,
            sizeof(cmd),
            sizeof(cmd) - 1,
            "export LD_LIBRARY_PATH=%s/../lib:\\$LD_LIBRARY_PATH; "
            "\"%s/gs_ctl\" -w -l \"%s\" -D \"%s\" -Z %s -o \"-b%s %s\" -M primary start",
            cluster->bindir,
            cluster->bindir,
            SERVER_LOG_FILE,
            cluster->pgconfig,
            cluster == &new_cluster ? "restoremode" : cluster_type,
            (cluster == &new_cluster) ? " -c synchronous_commit=off" : "",
            cluster->pgopts ? cluster->pgopts : "");
    }
    securec_check_ss_c(nRet, "\0", "\0");

    /*
     * Don't throw an error right away, let connecting throw the error because
     * it might supply a reason for the failure.
     */
    for (i = 0; i < BUILD_RETRY; i++) {
        pg_ctl_return = exec_prog(SERVER_START_LOG_FILE,
            /* pass both file names if they differ */
            (strcmp(SERVER_LOG_FILE, SERVER_START_LOG_FILE) != 0) ? SERVER_LOG_FILE : NULL,
            false,
            "%s",
            cmd);
        if (pg_ctl_return) {
            break;
        }
        sleep(60);
    }

    /* If the connection didn't fail, fail now */
    if (!pg_ctl_return)
        pg_log(PG_FATAL, "gs_ctl failed to start the %s server, or connection failed\n", CLUSTER_NAME(cluster));

    os_info.running_cluster = cluster;
}

void stop_postmaster(bool fast)
{
    ClusterInfo* cluster = NULL;

    if (os_info.running_cluster == &old_cluster)
        cluster = &old_cluster;
    else if (os_info.running_cluster == &new_cluster)
        cluster = &new_cluster;
    else
        return; /* no cluster running */

    if (os_info.is_root_user) {
        exec_prog(SERVER_STOP_LOG_FILE,
            NULL,
            !fast,
            "sudo -u %s LD_LIBRARY_PATH=\"%s/../lib\" sh -c \""
            "\\\"%s/gs_ctl\\\" -w -D \\\"%s\\\" -o \\\"%s\\\" %s stop"
            "\"",
            cluster->user,
            cluster->bindir,
            cluster->bindir,
            cluster->pgconfig,
            cluster->pgopts ? cluster->pgopts : "",
            fast ? "-m immediate" : "-t 1200");
    } else {
        exec_prog(SERVER_STOP_LOG_FILE,
            NULL,
            !fast,
            "export LD_LIBRARY_PATH=%s/../lib:\\$LD_LIBRARY_PATH; "
            "\"%s/gs_ctl\" -w -D \"%s\" -o \"%s\" %s stop",
            cluster->bindir,
            cluster->bindir,
            cluster->pgconfig,
            cluster->pgopts ? cluster->pgopts : "",
            fast ? "-m immediate" : "-t 1200");
    }

    os_info.running_cluster = NULL;
}

/*
 * check_pghost_envvar()
 *
 * Tests that PGHOST does not point to a non-local server
 */
void check_pghost_envvar(void)
{
    PQconninfoOption* option = NULL;
    PQconninfoOption* start = NULL;

    /* Get valid libpq env vars from the PQconndefaults function */

    start = PQconndefaults();

    for (option = start; option->keyword != NULL; option++) {
        if (option->envvar && (strcmp(option->envvar, "PGHOST") == 0 || strcmp(option->envvar, "PGHOSTADDR") == 0)) {
            const char* value = getenv(option->envvar);

            if (value && strlen(value) > 0 &&
                /* check for 'local' host values */
                (strcmp(value, "localhost") != 0 && strcmp(value, "127.0.0.1") != 0 && strcmp(value, "::1") != 0 &&
                    value[0] != '/'))
                pg_log(PG_FATAL,
                    "libpq environment variable %s has a non-local server value: %s\n",
                    option->envvar,
                    value);
        }
    }

    /* Free the memory that libpq allocated on our behalf */
    PQconninfoFree(start);
}