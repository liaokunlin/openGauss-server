/*
 *
 * pgarch.cpp
 *
 *	PostgreSQL WAL archiver
 *
 *	All functions relating to archiver are included here
 *
 *	- All functions executed by archiver process
 *
 *	- archiver is forked from postmaster, and the two
 *	processes then communicate using signals. All functions
 *	executed by postmaster are included in this file.
 *
 *	Initial author: Simon Riggs		simon@2ndquadrant.com
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/process/postmaster/pgarch.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/fork_process.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/copydir.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "utils/guc.h"
#include "utils/ps_status.h"

#include "gssignal/gs_signal.h"
#include "alarm/alarm.h"

/* ----------
 * Timer definitions.
 * ----------
 */
#define PGARCH_AUTOWAKE_INTERVAL           \
    60 /* How often to force a poll of the \
        * archive status directory; in     \
        * seconds. */
#define PGARCH_RESTART_INTERVAL             \
    10 /* How often to attempt to restart a \
        * failed archiver; in seconds. */

/* ----------
 * Archiver control info.
 *
 * We expect that archivable files within pg_xlog will have names between
 * MIN_XFN_CHARS and MAX_XFN_CHARS in length, consisting only of characters
 * appearing in VALID_XFN_CHARS.  The status files in archive_status have
 * corresponding names with ".ready" or ".done" appended.
 * ----------
 */
#define MIN_XFN_CHARS 16
#define MAX_XFN_CHARS 40
#define VALID_XFN_CHARS "0123456789ABCDEF.history.backup"

#define NUM_ARCHIVE_RETRIES 3

NON_EXEC_STATIC void PgArchiverMain();
static void pgarch_exit(SIGNAL_ARGS);
static void ArchSigHupHandler(SIGNAL_ARGS);
static void ArchSigTermHandler(SIGNAL_ARGS);
static void pgarch_waken(SIGNAL_ARGS);
static void pgarch_waken_stop(SIGNAL_ARGS);
static void pgarch_MainLoop(void);
static void pgarch_ArchiverCopyLoop(void);
static bool pgarch_archiveXlog(char* xlog);
static bool pgarch_readyXlog(char* xlog, int xlog_length);
static void pgarch_archiveDone(const char* xlog);

AlarmCheckResult DataInstArchChecker(Alarm* alarm, AlarmAdditionalParam* additionalParam)
{
    if (true == g_instance.WalSegmentArchSucceed) {
        // fill the resume message
        WriteAlarmAdditionalInfo(
            additionalParam, g_instance.attr.attr_common.PGXCNodeName, "", "", alarm, ALM_AT_Resume);
        return ALM_ACR_Normal;
    } else {
        // fill the alarm message
        WriteAlarmAdditionalInfo(additionalParam,
            g_instance.attr.attr_common.PGXCNodeName,
            "",
            "",
            alarm,
            ALM_AT_Fault,
            g_instance.attr.attr_common.PGXCNodeName);
        return ALM_ACR_Abnormal;
    }
}
/* ------------------------------------------------------------
 * Public functions called from postmaster follow
 * ------------------------------------------------------------
 */

/*
 * pgarch_start
 *
 *	Called from postmaster at startup or after an existing archiver
 *	died.  Attempt to fire up a fresh archiver process.
 *
 *	Returns PID of child process, or 0 if fail.
 *
 *	Note: if fail, we will be called again from the postmaster main loop.
 */
ThreadId pgarch_start(void)
{
    time_t curtime;

    /*
     * Do nothing if no archiver needed
     */
    if (!XLogArchivingActive())
        return 0;

    /*
     * Do nothing if too soon since last archiver start.  This is a safety
     * valve to protect against continuous respawn attempts if the archiver is
     * dying immediately at launch. Note that since we will be re-called from
     * the postmaster main loop, we will get another chance later.
     */
    curtime = time(NULL);
    if ((unsigned int)(curtime - t_thrd.arch.last_pgarch_start_time) < (unsigned int)PGARCH_RESTART_INTERVAL)
        return 0;
    t_thrd.arch.last_pgarch_start_time = curtime;

    return initialize_util_thread(ARCH);
}

/* ------------------------------------------------------------
 * Local functions called by archiver follow
 * ------------------------------------------------------------
 */
/*
 * PgArchiverMain
 *
 *	The argc/argv parameters are valid only in EXEC_BACKEND case.  However,
 *	since we don't use 'em, it hardly matters...
 */
NON_EXEC_STATIC void PgArchiverMain()
{
    IsUnderPostmaster = true; /* we are a postmaster subprocess now */

    t_thrd.proc_cxt.MyProcPid = gs_thread_self(); /* reset t_thrd.proc_cxt.MyProcPid */

    t_thrd.proc_cxt.MyStartTime = time(NULL); /* record Start Time for logging */

    knl_thread_set_name("Archiver");

    t_thrd.myLogicTid = noProcLogicTid + PGARCH_LID;

    ereport(LOG, (errmsg("Archiver started")));

    InitializeLatchSupport(); /* needed for latch waits */

    InitLatch(&t_thrd.arch.mainloop_latch); /* initialize latch used in main loop */
    /*
     * Ignore all signals usually bound to some action in the postmaster,
     * except for SIGHUP, SIGTERM, SIGUSR1, SIGUSR2, and SIGQUIT.
     */

    (void)gspqsignal(SIGHUP, ArchSigHupHandler);
    (void)gspqsignal(SIGINT, SIG_IGN);
    (void)gspqsignal(SIGTERM, ArchSigTermHandler);
    (void)gspqsignal(SIGQUIT, pgarch_exit);
    (void)gspqsignal(SIGALRM, SIG_IGN);
    (void)gspqsignal(SIGPIPE, SIG_IGN);
    (void)gspqsignal(SIGUSR1, pgarch_waken);
    (void)gspqsignal(SIGUSR2, pgarch_waken_stop);
    (void)gspqsignal(SIGCHLD, SIG_DFL);
    (void)gspqsignal(SIGTTIN, SIG_DFL);
    (void)gspqsignal(SIGTTOU, SIG_DFL);
    (void)gspqsignal(SIGCONT, SIG_DFL);
    (void)gspqsignal(SIGWINCH, SIG_DFL);

    gs_signal_setmask(&t_thrd.libpq_cxt.UnBlockSig, NULL);
    (void)gs_signal_unblock_sigusr2();

    /*
     * Identify myself via ps
     */
    init_ps_display("Archiver process", "", "", "");

    pgarch_MainLoop();

    gs_thread_exit(0);
}

/* SIGQUIT signal handler for archiver process */
static void pgarch_exit(SIGNAL_ARGS)
{
    /* SIGQUIT means curl up and die ... */
    exit(1);
}

/* SIGHUP signal handler for archiver process */
static void ArchSigHupHandler(SIGNAL_ARGS)
{
    int save_errno = errno;

    /* set flag to re-read config file at next convenient time */
    t_thrd.arch.got_SIGHUP = true;
    SetLatch(&t_thrd.arch.mainloop_latch);

    errno = save_errno;
}

/* SIGTERM signal handler for archiver process */
static void ArchSigTermHandler(SIGNAL_ARGS)
{
    int save_errno = errno;

    /*
     * The postmaster never sends us SIGTERM, so we assume that this means
     * that init is trying to shut down the whole system.  If we hang around
     * too long we'll get SIGKILL'd.  Set flag to prevent starting any more
     * archive commands.
     */
    t_thrd.arch.got_SIGTERM = true;
    SetLatch(&t_thrd.arch.mainloop_latch);

    errno = save_errno;
}

/* SIGUSR1 signal handler for archiver process */
static void pgarch_waken(SIGNAL_ARGS)
{
    int save_errno = errno;

    /* set flag that there is work to be done */
    t_thrd.arch.wakened = true;
    SetLatch(&t_thrd.arch.mainloop_latch);

    errno = save_errno;
}

/* SIGUSR2 signal handler for archiver process */
static void pgarch_waken_stop(SIGNAL_ARGS)
{
    int save_errno = errno;

    /* set flag to do a final cycle and shut down afterwards */
    t_thrd.arch.ready_to_stop = true;
    SetLatch(&t_thrd.arch.mainloop_latch);

    errno = save_errno;
}

/*
 * pgarch_MainLoop
 *
 * Main loop for archiver
 */
static void pgarch_MainLoop(void)
{
    pg_time_t last_copy_time = 0;
    bool time_to_stop = false;

    /*
     * We run the copy loop immediately upon entry, in case there are
     * unarchived files left over from a previous database run (or maybe the
     * archiver died unexpectedly).  After that we wait for a signal or
     * timeout before doing more.
     */
    t_thrd.arch.wakened = true;

    /*
     * There shouldn't be anything for the archiver to do except to wait for a
     * signal ... however, the archiver exists to protect our data, so she
     * wakes up occasionally to allow herself to be proactive.
     */
    do {
        ResetLatch(&t_thrd.arch.mainloop_latch);

        /* When we get SIGUSR2, we do one more archive cycle, then exit */
        time_to_stop = t_thrd.arch.ready_to_stop;

        /* Check for config update */
        if (t_thrd.arch.got_SIGHUP) {
            t_thrd.arch.got_SIGHUP = false;
            ProcessConfigFile(PGC_SIGHUP);
            if (!XLogArchivingActive()) {
                ereport(LOG, (errmsg("PgArchiver exit")));
                return;
            }
        }

        /*
         * If we've gotten SIGTERM, we normally just sit and do nothing until
         * SIGUSR2 arrives.  However, that means a random SIGTERM would
         * disable archiving indefinitely, which doesn't seem like a good
         * idea.  If more than 60 seconds pass since SIGTERM, exit anyway, so
         * that the postmaster can start a new archiver if needed.
         */
        if (t_thrd.arch.got_SIGTERM) {
            time_t curtime = time(NULL);

            if (t_thrd.arch.last_sigterm_time == 0)
                t_thrd.arch.last_sigterm_time = curtime;
            else if ((unsigned int)(curtime - t_thrd.arch.last_sigterm_time) >= (unsigned int)60)
                break;
        }

        /* Do what we're here for */
        if (t_thrd.arch.wakened || time_to_stop) {
            t_thrd.arch.wakened = false;
            pgarch_ArchiverCopyLoop();
            last_copy_time = time(NULL);
        }

        /*
         * Sleep until a signal is received, or until a poll is forced by
         * PGARCH_AUTOWAKE_INTERVAL having passed since last_copy_time, or
         * until postmaster dies.
         */
        if (!time_to_stop) {
            /* Don't wait during last iteration */
            pg_time_t curtime = (pg_time_t)time(NULL);
            const int timeout = PGARCH_AUTOWAKE_INTERVAL - (curtime - last_copy_time);
            if (timeout > 0) {
                int rc;

                rc = WaitLatch(
                    &t_thrd.arch.mainloop_latch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, timeout * 1000L);
                if (rc & WL_TIMEOUT)
                    t_thrd.arch.wakened = true;
            } else
                t_thrd.arch.wakened = true;
        }

        /*
         * The archiver quits either when the postmaster dies (not expected)
         * or after completing one more archiving cycle after receiving
         * SIGUSR2.
         */
    } while (PostmasterIsAlive() && !time_to_stop);
}

/*
 * pgarch_ArchiverCopyLoop
 *
 * Archives all outstanding xlogs then returns
 */
static void pgarch_ArchiverCopyLoop(void)
{
    char xlog[MAX_XFN_CHARS + 1];

    /*
     * loop through all xlogs with archive_status of .ready and archive
     * them...mostly we expect this to be a single file, though it is possible
     * some backend will add files onto the list of those that need archiving
     * while we are still copying earlier archives
     */
    while (pgarch_readyXlog(xlog, MAX_XFN_CHARS + 1)) {
        int failures = 0;

        for (;;) {
            /*
             * Do not initiate any more archive commands after receiving
             * SIGTERM, nor after the postmaster has died unexpectedly. The
             * first condition is to try to keep from having init SIGKILL the
             * command, and the second is to avoid conflicts with another
             * archiver spawned by a newer postmaster.
             */
            if (t_thrd.arch.got_SIGTERM || !PostmasterIsAlive())
                return;

            /*
             * Check for config update.  This is so that we'll adopt a new
             * setting for archive_command as soon as possible, even if there
             * is a backlog of files to be archived.
             */
            if (t_thrd.arch.got_SIGHUP) {
                ProcessConfigFile(PGC_SIGHUP);
                if (!XLogArchivingActive()) {
                    return;
                }
                t_thrd.arch.got_SIGHUP = false;
            }

            /* can't do anything if no command ... */
            if (!XLogArchiveCommandSet()) {
                ereport(WARNING, (errmsg("archive_mode enabled, yet archive_command is not set")));
                return;
            }

            if (pgarch_archiveXlog(xlog)) {
                /* successful */
                pgarch_archiveDone(xlog);
                break; /* out of inner retry loop */
            } else {
                if (++failures >= NUM_ARCHIVE_RETRIES) {
                    ereport(WARNING,
                        (errmsg("transaction log file \"%s\" could not be archived: too many failures", xlog)));
                    return; /* give up archiving for now */
                }
                pg_usleep(1000000L); /* wait a bit before retrying */
            }
        }
    }
}

/*
 * pgarch_archiveXlog
 *
 * Invokes system(3) to copy one archive file to wherever it should go
 *
 * Returns true if successful
 */
static bool pgarch_archiveXlog(char* xlog)
{
    char xlogarchcmd[MAXPGPATH];
    char pathname[MAXPGPATH];
    char activitymsg[MAXFNAMELEN + 16];
    char* dp = NULL;
    char* endp = NULL;
    const char* sp = NULL;
    int rc = 0;

    rc = snprintf_s(pathname, MAXPGPATH, MAXPGPATH - 1, XLOGDIR "/%s", xlog);
    securec_check_ss(rc, "\0", "\0");

    /*
     * construct the command to be executed
     */
    dp = xlogarchcmd;
    endp = xlogarchcmd + MAXPGPATH - 1;
    *endp = '\0';

    for (sp = u_sess->attr.attr_storage.XLogArchiveCommand; *sp; sp++) {
        if (*sp == '%') {
            switch (sp[1]) {
                case 'p':
                    /* %p: relative path of source file */
                    sp++;
                    strlcpy(dp, pathname, endp - dp);
                    make_native_path(dp);
                    dp += strlen(dp);
                    break;
                case 'f':
                    /* %f: filename of source file */
                    sp++;
                    strlcpy(dp, xlog, endp - dp);
                    dp += strlen(dp);
                    break;
                case '%':
                    /* convert %% to a single % */
                    sp++;
                    if (dp < endp) {
                        *dp++ = *sp;
                    }
                    break;
                default:
                    /* otherwise treat the % as not special */
                    if (dp < endp) {
                        *dp++ = *sp;
                    }
                    break;
            }
        } else {
            if (dp < endp) {
                *dp++ = *sp;
            }
        }
    }
    *dp = '\0';

    ereport(DEBUG3, (errmsg_internal("executing archive command \"%s\"", xlogarchcmd)));

    /* Report archive activity in PS display */
    rc = snprintf_s(activitymsg, sizeof(activitymsg), sizeof(activitymsg) - 1, "archiving %s", xlog);
    securec_check_ss(rc, "\0", "\0");

    set_ps_display(activitymsg, false);

    rc = gs_popen_security(xlogarchcmd);
    if (rc != 0) {
        /* If execute archive command failed, we report an alarm */
        g_instance.WalSegmentArchSucceed = false;

        /*
         * If either the shell itself, or a called command, died on a signal,
         * abort the archiver.	We do this because system() ignores SIGINT and
         * SIGQUIT while waiting; so a signal is very likely something that
         * should have interrupted us too.	If we overreact it's no big deal,
         * the postmaster will just start the archiver again.
         *
         * Per the Single Unix Spec, shells report exit status > 128 when a
         * called command died on a signal.
         */
        int lev = (WIFSIGNALED(rc) || WEXITSTATUS(rc) > 128) ? FATAL : LOG;

        if (WIFEXITED(rc)) {
            ereport(lev,
                (errmsg("archive command failed with exit code %d", WEXITSTATUS(rc)),
                    errdetail("The failed archive command was: \"%s\" ", xlogarchcmd)));
        } else if (WIFSIGNALED(rc)) {
#if defined(WIN32)
            ereport(lev,
                (errmsg("archive command was terminated by exception 0x%X", WTERMSIG(rc)),
                    errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
                    errdetail("The failed archive command was: \"%s\" ", xlogarchcmd)));
#elif defined(HAVE_DECL_SYS_SIGLIST) && HAVE_DECL_SYS_SIGLIST
            ereport(lev,
                (errmsg("archive command was terminated by signal %d: %s",
                     WTERMSIG(rc),
                     WTERMSIG(rc) < NSIG ? sys_siglist[WTERMSIG(rc)] : "(unknown)"),
                    errdetail("The failed archive command was: \"%s\" ", xlogarchcmd)));
#else
            ereport(lev,
                (errmsg("archive command was terminated by signal %d", WTERMSIG(rc)),
                    errdetail("The failed archive command was: \"%s\" ", xlogarchcmd)));
#endif
        } else {
            ereport(lev,
                (errmsg("archive command exited with unrecognized status %d", rc),
                    errdetail("The failed archive command was: \"%s\" ", xlogarchcmd)));
        }

        rc = snprintf_s(activitymsg, sizeof(activitymsg), sizeof(activitymsg) - 1, "failed on %s", xlog);
        securec_check_ss(rc, "\0", "\0");

        set_ps_display(activitymsg, false);

        return false;
    }
    g_instance.WalSegmentArchSucceed = true;
    ereport(DEBUG1, (errmsg("archived transaction log file \"%s\"", xlog)));

    rc = snprintf_s(activitymsg, sizeof(activitymsg), sizeof(activitymsg) - 1, "last was %s", xlog);
    securec_check_ss(rc, "\0", "\0");

    set_ps_display(activitymsg, false);

    return true;
}

/*
 * pgarch_readyXlog
 *
 * Return name of the oldest xlog file that has not yet been archived.
 * No notification is set that file archiving is now in progress, so
 * this would need to be extended if multiple concurrent archival
 * tasks were created. If a failure occurs, we will completely
 * re-copy the file at the next available opportunity.
 *
 * It is important that we return the oldest, so that we archive xlogs
 * in order that they were written, for two reasons:
 * 1) to maintain the sequential chain of xlogs required for recovery
 * 2) because the oldest ones will sooner become candidates for
 * recycling at time of checkpoint
 *
 * NOTE: the "oldest" comparison will presently consider all segments of
 * a timeline with a smaller ID to be older than all segments of a timeline
 * with a larger ID; the net result being that past timelines are given
 * higher priority for archiving.  This seems okay, or at least not
 * obviously worth changing.
 */
static bool pgarch_readyXlog(char* xlog, int xlog_length)
{
    /*
     * open xlog status directory and read through list of xlogs that have the
     * .ready suffix, looking for earliest file. It is possible to optimise
     * this code, though only a single file is expected on the vast majority
     * of calls, so....
     */
    char XLogArchiveStatusDir[MAXPGPATH];
    char newxlog[MAX_XFN_CHARS + 6 + 1];
    DIR* rldir = NULL;
    struct dirent* rlde = NULL;
    bool found = false;
    int rc = 0;

    rc = snprintf_s(XLogArchiveStatusDir, MAXPGPATH, MAXPGPATH - 1, XLOGDIR "/archive_status");
    securec_check_ss(rc, "", "");

    rldir = AllocateDir(XLogArchiveStatusDir);
    if (rldir == NULL)
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg("could not open archive status directory \"%s\": %m", XLogArchiveStatusDir)));

    while ((rlde = ReadDir(rldir, XLogArchiveStatusDir)) != NULL) {
        int basenamelen = (int)strlen(rlde->d_name) - 6;

        if (basenamelen >= MIN_XFN_CHARS && basenamelen <= MAX_XFN_CHARS &&
            strspn(rlde->d_name, VALID_XFN_CHARS) >= (size_t)basenamelen &&
            strcmp(rlde->d_name + basenamelen, ".ready") == 0) {
            errno_t rc = EOK;

            if (!found) {
                rc = strcpy_s(newxlog, MAX_XFN_CHARS + 6 + 1, rlde->d_name);
                securec_check(rc, "\0", "\0");

                found = true;
            } else {
                if (strcmp(rlde->d_name, newxlog) < 0) {
                    rc = strcpy_s(newxlog, MAX_XFN_CHARS + 6 + 1, rlde->d_name);
                    securec_check(rc, "\0", "\0");
                }
            }
        }
    }
    FreeDir(rldir);

    if (found) {
        errno_t rc = EOK;

        /* truncate off the .ready */
        newxlog[strlen(newxlog) - 6] = '\0';
        rc = strcpy_s(xlog, (size_t)xlog_length, newxlog);
        securec_check(rc, "\0", "\0");
    }
    return found;
}

/*
 * pgarch_archiveDone
 *
 * Emit notification that an xlog file has been successfully archived.
 * We do this by renaming the status file from NNN.ready to NNN.done.
 * Eventually, a checkpoint process will notice this and delete both the
 * NNN.done file and the xlog file itself.
 */
static void pgarch_archiveDone(const char* xlog)
{
    char rlogready[MAXPGPATH];
    char rlogdone[MAXPGPATH];

    StatusFilePath(rlogready, xlog, ".ready");
    StatusFilePath(rlogdone, xlog, ".done");
    (void)durable_rename(rlogready, rlogdone, WARNING);
}
