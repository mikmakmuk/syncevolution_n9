/*
 * Copyright (C) 2005-2008 Patrick Ohly
 */

#include "EvolutionSyncClient.h"
#include "EvolutionSyncSource.h"
#include "SyncEvolutionUtil.h"

#include "Logging.h"
#include "LogStdout.h"
#include "TransportAgent.h"
#include "CurlTransportAgent.h"
#include "SoupTransportAgent.h"
using namespace SyncEvolution;

#include <list>
#include <memory>
#include <vector>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
using namespace std;

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/foreach.hpp>

#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "synthesis/enginemodulebridge.h"
#include "synthesis/SDK_util.h"

SourceList *EvolutionSyncClient::m_sourceListPtr;

EvolutionSyncClient::EvolutionSyncClient(const string &server,
                                         bool doLogging,
                                         const set<string> &sources) :
    EvolutionSyncConfig(server),
    m_server(server),
    m_sources(sources),
    m_doLogging(doLogging),
    m_quiet(false),
    m_engine(new sysync::TEngineModuleBridge())
{
    // Use libsynthesis that we were linked against.  The name of a
    // .so could be given here, too, to use that instead.  This
    // instance of the engine is used outside of the sync session
    // itself. doSync() then creates another engine for the sync
    // itself. That is necessary because the engine shutdown depends
    // on the context of the sync (in particular instantiated sync
    // sources).
    sysync::TSyError err = m_engine->Connect("[]", 0,
                                             sysync::DBG_PLUGIN_NONE|
                                             sysync::DBG_PLUGIN_INT|
                                             sysync::DBG_PLUGIN_DB|
                                             sysync::DBG_PLUGIN_EXOT|
                                             sysync::DBG_PLUGIN_ALL);
    if (err) {
        throwError("Connect");
    }
}

EvolutionSyncClient::~EvolutionSyncClient()
{
    delete m_engine;
}


// this class owns the logging directory and is responsible
// for redirecting output at the start and end of sync (even
// in case of exceptions thrown!)
class LogDir : public LoggerStdout {
    string m_logdir;         /**< configured backup root dir, empty if none */
    int m_maxlogdirs;        /**< number of backup dirs to preserve, 0 if unlimited */
    string m_prefix;         /**< common prefix of backup dirs */
    string m_path;           /**< path to current logging and backup dir */
    const string &m_server;  /**< name of the server for this synchronization */
    string m_logfile;        /**< path to log file there, empty if not writing one */
    FILE *m_file;            /**< file handle for log file, NULL if not writing one */

public:
    LogDir(const string &server) : m_server(server), m_file(NULL)
    {
        // SyncEvolution-<server>-<yyyy>-<mm>-<dd>-<hh>-<mm>
        m_prefix = "SyncEvolution-";
        m_prefix += m_server;

        // default: $TMPDIR/SyncEvolution-<username>-<server>
        stringstream path;
        char *tmp = getenv("TMPDIR");
        if (tmp) {
            path << tmp;
        } else {
            path << "/tmp";
        }
        path << "/SyncEvolution-";
        struct passwd *user = getpwuid(getuid());
        if (user && user->pw_name) {
            path << user->pw_name;
        } else {
            path << getuid();
        }
        path << "-" << m_server;

        m_path = path.str();
    }

    /**
     * Finds previous log directory. Must be called before setLogdir().
     *
     * @param path        path to configured backup directy, NULL if defaulting to /tmp, "none" if not creating log file
     * @return full path of previous log directory, empty string if not found
     */
    string previousLogdir(const char *path) {
        string logdir;

        if (path && !strcasecmp(path, "none")) {
            return "";
        } else if (path && path[0]) {
            vector<string> entries;
            try {
                getLogdirs(path, entries);
            } catch(...) {
                SyncEvolutionException::handle();
                return "";
            }

            logdir = entries.size() ? string(path) + "/" + entries[entries.size()-1] : "";
        } else {
            logdir = m_path;
        }

        if (access(logdir.c_str(), R_OK|X_OK) == 0) {
            return logdir;
        } else {
            return "";
        }
    }

    // setup log directory and redirect logging into it
    // @param path        path to configured backup directy, NULL if defaulting to /tmp, "none" if not creating log file
    // @param maxlogdirs  number of backup dirs to preserve in path, 0 if unlimited
    // @param logLevel    0 = default, 1 = ERROR, 2 = INFO, 3 = DEBUG
    // @param usePath     write directly into path, don't create and manage subdirectories
    void setLogdir(const char *path, int maxlogdirs, int logLevel = 0, bool usePath = false) {
        m_maxlogdirs = maxlogdirs;
        if (path && !strcasecmp(path, "none")) {
            m_path = "";
            m_logfile = "";
        } else if (path && path[0]) {
            m_logdir = path;

            if (!usePath) {
                // create unique directory name in the given directory
                time_t ts = time(NULL);
                struct tm *tm = localtime(&ts);
                stringstream base;
                base << path << "/"
                     << m_prefix
                     << "-"
                     << setfill('0')
                     << setw(4) << tm->tm_year + 1900 << "-"
                     << setw(2) << tm->tm_mon + 1 << "-"
                     << setw(2) << tm->tm_mday << "-"
                     << setw(2) << tm->tm_hour << "-"
                     << setw(2) << tm->tm_min;
                int seq = 0;
                while (true) {
                    stringstream path;
                    path << base.str();
                    if (seq) {
                        path << "-" << seq;
                    }
                    m_path = path.str();
                    if (!mkdir(m_path.c_str(), S_IRWXU)) {
                        break;
                    }
                    if (errno != EEXIST) {
                        SE_LOG_DEBUG(NULL, NULL, "%s: %s", m_path.c_str(), strerror(errno));
                        EvolutionSyncClient::throwError(m_path, errno);
                    }
                    seq++;
                }
            } else {
                m_path = path;
                if (mkdir(m_path.c_str(), S_IRWXU) &&
                    errno != EEXIST) {
                    SE_LOG_DEBUG(NULL, NULL, "%s: %s", m_path.c_str(), strerror(errno));
                    EvolutionSyncClient::throwError(m_path, errno);
                }
            }
            m_logfile = m_path + "/client.log";
        } else {
            // use the default temp directory
            if (mkdir(m_path.c_str(), S_IRWXU)) {
                if (errno != EEXIST) {
                    EvolutionSyncClient::throwError(m_path, errno);
                }
            }
            m_logfile = m_path + "/client.log";
        }

        if (m_logfile.size()) {
            // redirect logging into that directory, including stderr,
            // after truncating it
            m_file = fopen(m_logfile.c_str(), "w");
            if (!m_file) {
                SE_LOG_ERROR(NULL, NULL, "creating log file %s failed", m_logfile.c_str());
            }
        }

        // update log level of default logger and our own replacement
        Level level;
        switch (logLevel) {
        case 0:
            // default for console output
            level = INFO;
            break;
        case 1:
            level = ERROR;
            break;
        case 2:
            level = INFO;
            break;
        default:
            level = DEBUG;
            break;
        }
        if (!usePath) {
            instance().setLevel(level);
        }
        setLevel(level);
        pushLogger(this);
    }

    /** sets a fixed directory for database files without redirecting logging */
    void setPath(const string &path) { m_path = path; }

    // return log directory, empty if not enabled
    const string &getLogdir() {
        return m_path;
    }

    // return log file, empty if not enabled
    const string &getLogfile() {
        return m_logfile;
    }

    /** find all entries in a given directory, return as sorted array */
    void getLogdirs(const string &logdir, vector<string> &entries) {
        ReadDir dir(logdir);
        BOOST_FOREACH(const string &entry, dir) {
            if (boost::starts_with(entry, m_prefix)) {
                entries.push_back(entry);
            }
        }
        sort(entries.begin(), entries.end());
    }


    // remove oldest backup dirs if exceeding limit
    void expire() {
        if (m_logdir.size() && m_maxlogdirs > 0 ) {
            vector<string> entries;
            getLogdirs(m_logdir, entries);

            int deleted = 0;
            for (vector<string>::iterator it = entries.begin();
                 it != entries.end() && (int)entries.size() - deleted > m_maxlogdirs;
                 ++it, ++deleted) {
                string path = m_logdir + "/" + *it;
                string msg = "removing " + path;
                SE_LOG_INFO(NULL, NULL, "%s", msg.c_str());
                rm_r(path);
            }
        }
    }

    // remove redirection of logging
    void restore() {
        if (&instance() == this) {
            popLogger();
        }
        if (m_file) {
            fclose(m_file);
            m_file = NULL;
        }
    }

    ~LogDir() {
        restore();
    }


    virtual void messagev(Level level,
                          const char *prefix,
                          const char *file,
                          int line,
                          const char *function,
                          const char *format,
                          va_list args)
    {
        if (m_file) {
            va_list argscopy;
            va_copy(argscopy, args);
            // once to file, with full debugging
            // TODO: configurable level of detail for file
            LoggerStdout::messagev(m_file, level, DEBUG,
                                   prefix, file, line, function,
                                   format, argscopy);
            va_end(argscopy);
        }
        // to stdout
        LoggerStdout::messagev(level, prefix, file, line, function, format, args);
    }
};

// this class owns the sync sources and (together with
// a logdir) handles writing of per-sync files as well
// as the final report (
class SourceList : public vector<EvolutionSyncSource *> {
    LogDir m_logdir;     /**< our logging directory */
    bool m_prepared;     /**< remember whether syncPrepare() dumped databases successfully */
    bool m_doLogging;    /**< true iff the normal logdir handling is enabled
                            (creating and expiring directoties, before/after comparison) */
    bool m_reportTodo;   /**< true if syncDone() shall print a final report */
    const bool m_quiet;  /**< avoid redundant printing to screen */
    string m_previousLogdir; /**< remember previous log dir before creating the new one */

    /** create name in current (if set) or previous logdir */
    string databaseName(EvolutionSyncSource &source, const string suffix, string logdir = "") {
        if (!logdir.size()) {
            logdir = m_logdir.getLogdir();
        }
        return logdir + "/" +
            source.getName() + "." + suffix + "." +
            source.fileSuffix();
    }

public:
    /**
     * dump into files with a certain suffix
     */
    void dumpDatabases(const string &suffix) {
        ofstream out;
#ifndef IPHONE
        // output stream on iPhone raises exception even though it is in a good state;
        // perhaps the missing C++ exception support is the reason:
        // http://code.google.com/p/iphone-dev/issues/detail?id=48
        out.exceptions(ios_base::badbit|ios_base::failbit|ios_base::eofbit);
#endif

        BOOST_FOREACH(EvolutionSyncSource *source, *this) {
            string file = databaseName(*source, suffix);
            SE_LOG_DEBUG(NULL, NULL, "creating %s", file.c_str());
            out.open(file.c_str());
            source->exportData(out);
            out.close();
            SE_LOG_DEBUG(NULL, NULL, "%s created", file.c_str());
        }
    }

    /** remove database dumps with a specific suffix */
    void removeDatabases(const string &removeSuffix) {
        BOOST_FOREACH(EvolutionSyncSource *source, *this) {
            string file;

            file = databaseName(*source, removeSuffix);
            unlink(file.c_str());
        }
    }
        
    SourceList(const string &server, bool doLogging, bool quiet) :
        m_logdir(server),
        m_prepared(false),
        m_doLogging(doLogging),
        m_reportTodo(true),
        m_quiet(quiet)
    {
    }
    
    // call as soon as logdir settings are known
    void setLogdir(const char *logDirPath, int maxlogdirs, int logLevel) {
        m_previousLogdir = m_logdir.previousLogdir(logDirPath);
        if (m_doLogging) {
            m_logdir.setLogdir(logDirPath, maxlogdirs, logLevel);
        } else {
            // Run debug session without paying attention to
            // the normal logdir handling. The log level here
            // refers to stdout. The log file will be as complete
            // as possible.
            m_logdir.setLogdir(logDirPath, 0, 1, true);
        }
    }

    /** return log directory, empty if not enabled */
    const string &getLogdir() {
        return m_logdir.getLogdir();
    }

    /** return previous log dir found in setLogdir() */
    const string &getPrevLogdir() const { return m_previousLogdir; }

    /** set directory for database files without actually redirecting the logging */
    void setPath(const string &path) { m_logdir.setPath(path); }

    /**
     * If possible (m_previousLogdir found) and enabled (!m_quiet),
     * then dump changes applied locally.
     *
     * @param oldSuffix      suffix of old database dump: usually "after"
     * @param currentSuffix  the current database dump suffix: "current"
     *                       when not doing a sync, otherwise "before"
     */
    bool dumpLocalChanges(const string &oldSuffix, const string &newSuffix) {
        if (m_quiet || !m_previousLogdir.size()) {
            return false;
        }

        cout << "Local changes to be applied to server during synchronization:\n";
        BOOST_FOREACH(EvolutionSyncSource *source, *this) {
            string oldFile = databaseName(*source, oldSuffix, m_previousLogdir);
            string newFile = databaseName(*source, newSuffix);
            cout << "*** " << source->getName() << " ***\n" << flush;
            string cmd = string("env CLIENT_TEST_COMPARISON_FAILED=10 CLIENT_TEST_LEFT_NAME='after last sync' CLIENT_TEST_RIGHT_NAME='current data' CLIENT_TEST_REMOVED='removed since last sync' CLIENT_TEST_ADDED='added since last sync' synccompare 2>/dev/null '" ) +
                oldFile + "' '" + newFile + "'";
            int ret = system(cmd.c_str());
            switch (ret == -1 ? ret : WEXITSTATUS(ret)) {
            case 0:
                cout << "no changes\n";
                break;
            case 10:
                break;
            default:
                cout << "Comparison was impossible.\n";
                break;
            }
        }
        cout << "\n";
        return true;
    }

    // call when all sync sources are ready to dump
    // pre-sync databases
    void syncPrepare() {
        if (m_logdir.getLogfile().size() &&
            m_doLogging) {
            // dump initial databases
            dumpDatabases("before");
            // compare against the old "after" database dump
            dumpLocalChanges("after", "before");
            // now remove the old database dump
            removeDatabases("after");
            m_prepared = true;
        }
    }

    // call at the end of a sync with success == true
    // if all went well to print report
    void syncDone(bool success, const SyncReport *report) {
        if (m_doLogging) {
            // ensure that stderr is seen again
            m_logdir.restore();

            if (m_reportTodo) {
                // haven't looked at result of sync yet;
                // don't do it again
                m_reportTodo = false;

                // dump datatbase after sync, but not if already dumping it at the beginning didn't complete
                if (m_prepared) {
                    try {
                        dumpDatabases("after");
                    } catch (...) {
                        SyncEvolutionException::handle();
                        m_prepared = false;
                    }
                }

                string logfile = m_logdir.getLogfile();
                cout << flush;
                cerr << flush;
                cout << "\n";
                if (success) {
                    cout << "Synchronization successful.\n";
                } else if (logfile.size()) {
                    cout << "Synchronization failed, see "
                         << logfile
                         << " for details.\n";
                } else {
                    cout << "Synchronization failed.\n";
                }

                // pretty-print report
                if (!m_quiet) {
                    cout << "\nChanges applied during synchronization:\n";
                }
                if (!m_quiet && report) {
                    cout << *report;
                }

                // compare databases?
                if (!m_quiet && m_prepared) {
                    cout << "\nChanges applied to client during synchronization:\n";
                    BOOST_FOREACH(EvolutionSyncSource *source, *this) {
                        cout << "*** " << source->getName() << " ***\n" << flush;

                        string before = databaseName(*source, "before");
                        string after = databaseName(*source, "after");
                        string cmd = string("synccompare '" ) +
                            before + "' '" + after +
                            "' && echo 'no changes'";
                        system(cmd.c_str());
                    }
                    cout << "\n";
                }

                if (success) {
                    m_logdir.expire();
                }
            }
        }
    }

    /** returns names of active sources */
    set<string> getSources() {
        set<string> res;

        BOOST_FOREACH(EvolutionSyncSource *source, *this) {
            res.insert(source->getName());
        }
        return res;
    }
   
    ~SourceList() {
        // free sync sources
        BOOST_FOREACH(EvolutionSyncSource *source, *this) {
            delete source;
        }
    }

    /** find sync source by name */
    EvolutionSyncSource *operator [] (const string &name) {
        BOOST_FOREACH(EvolutionSyncSource *source, *this) {
            if (name == source->getName()) {
                return source;
            }
        }
        return NULL;
    }

    /** find by index */
    EvolutionSyncSource *operator [] (int index) { return vector<EvolutionSyncSource *>::operator [] (index); }
};

void unref(SourceList *sourceList)
{
    delete sourceList;
}

string EvolutionSyncClient::askPassword(const string &descr)
{
    char buffer[256];

    printf("Enter password for %s: ",
           descr.c_str());
    fflush(stdout);
    if (fgets(buffer, sizeof(buffer), stdin) &&
        strcmp(buffer, "\n")) {
        size_t len = strlen(buffer);
        if (len && buffer[len - 1] == '\n') {
            buffer[len - 1] = 0;
        }
        return buffer;
    } else {
        throwError(string("could not read password for ") + descr);
        return "";
    }
}

boost::shared_ptr<TransportAgent> EvolutionSyncClient::createTransportAgent()
{
#ifdef ENABLE_LIBSOUP
    boost::shared_ptr<SoupTransportAgent> agent(new SoupTransportAgent());
#elif defined(ENABLE_LIBCURL)
    boost::shared_ptr<CurlTransportAgent> agent(new CurlTransportAgent());
#else
    boost::shared_ptr<TransportAgent> agent;
    throw std::string("libsyncevolution was compiled without default transport, client must implement EvolutionSyncClient::createTransportAgent()");
#endif
    return agent;
}

void EvolutionSyncClient::displayServerMessage(const string &message)
{
    SE_LOG_INFO(NULL, NULL, "message from server: %s", message.c_str());
}

void EvolutionSyncClient::displaySyncProgress(sysync::TProgressEventEnum type,
                                              int32_t extra1, int32_t extra2, int32_t extra3)
{
    
}

void EvolutionSyncClient::displaySourceProgress(sysync::TProgressEventEnum type,
                                                EvolutionSyncSource &source,
                                                int32_t extra1, int32_t extra2, int32_t extra3)
{
    switch(type) {
    case sysync::PEV_PREPARING:
        /* preparing (e.g. preflight in some clients), extra1=progress, extra2=total */
        /* extra2 might be zero */
        if (extra2) {
            SE_LOG_INFO(NULL, NULL, "%s: preparing %d/%d",
                        source.getName(), extra1, extra2);
        } else {
            SE_LOG_INFO(NULL, NULL, "%s: preparing %d",
                        source.getName(), extra1);
        }
        break;
    case sysync::PEV_DELETING:
        /* deleting (zapping datastore), extra1=progress, extra2=total */
        if (extra2) {
            SE_LOG_INFO(NULL, NULL, "%s: deleting %d/%d",
                        source.getName(), extra1, extra2);
        } else {
            SE_LOG_INFO(NULL, NULL, "%s: deleting %d",
                        source.getName(), extra1);
        }
        break;
    case sysync::PEV_ALERTED: {
        /* datastore alerted (extra1=0 for normal, 1 for slow, 2 for first time slow, 
           extra2=1 for resumed session,
           extra3 0=twoway, 1=fromserver, 2=fromclient */
        SE_LOG_INFO(NULL, NULL, "%s: %s %s sync%s",
                    source.getName(),
                    extra2 ? "resuming" : "starting",
                    extra1 == 0 ? "normal" :
                    extra1 == 1 ? "slow" :
                    extra1 == 2 ? "first time" :
                    "unknown",
                    extra3 == 0 ? ", two-way" :
                    extra3 == 1 ? " from server" :
                    extra3 == 2 ? " from client" :
                    ", unknown direction");

        SyncMode mode = SYNC_NONE;
        switch (extra1) {
        case 0:
            switch (extra3) {
            case 0:
                mode = SYNC_TWO_WAY;
                break;
            case 1:
                mode = SYNC_ONE_WAY_FROM_SERVER;
                break;
            case 2:
                mode = SYNC_ONE_WAY_FROM_CLIENT;
                break;
            }
            break;
        case 1:
        case 2:
            switch (extra3) {
            case 0:
                mode = SYNC_SLOW;
                break;
            case 1:
                mode = SYNC_REFRESH_FROM_SERVER;
                break;
            case 2:
                mode = SYNC_REFRESH_FROM_CLIENT;
                break;
            }
            break;
        }
        source.recordFinalSyncMode(mode);
        source.recordFirstSync(extra1 == 2);
        source.recordResumeSync(extra2 == 1);
        break;
    }
    case sysync::PEV_SYNCSTART:
        /* sync started */
        SE_LOG_INFO(NULL, NULL, "%s: started",
                    source.getName());
        break;
    case sysync::PEV_ITEMRECEIVED:
        /* item received, extra1=current item count,
           extra2=number of expected changes (if >= 0) */
        if (extra2 > 0) {
            SE_LOG_INFO(NULL, NULL, "%s: received %d/%d",
                        source.getName(), extra1, extra2);
        } else {
            SE_LOG_INFO(NULL, NULL, "%s: received %d",
                     source.getName(), extra1);
        }
        break;
    case sysync::PEV_ITEMSENT:
        /* item sent,     extra1=current item count,
           extra2=number of expected items to be sent (if >=0) */
        if (extra2 > 0) {
            SE_LOG_INFO(NULL, NULL, "%s: sent %d/%d",
                     source.getName(), extra1, extra2);
        } else {
            SE_LOG_INFO(NULL, NULL, "%s: sent %d",
                     source.getName(), extra1);
        }
        break;
    case sysync::PEV_ITEMPROCESSED:
        /* item locally processed,               extra1=# added, 
           extra2=# updated,
           extra3=# deleted */
        SE_LOG_INFO(NULL, NULL, "%s: added %d, updated %d, removed %d",
                 source.getName(), extra1, extra2, extra3);
        break;
    case sysync::PEV_SYNCEND:
        /* sync finished, probably with error in extra1 (0=ok),
           syncmode in extra2 (0=normal, 1=slow, 2=first time), 
           extra3=1 for resumed session) */
        SE_LOG_INFO(NULL, NULL, "%s: %s%s sync done %s",
                 source.getName(),
                 extra3 ? "resumed " : "",
                 extra2 == 0 ? "normal" :
                 extra2 == 1 ? "slow" :
                 extra2 == 2 ? "first time" :
                 "unknown",
                 extra1 ? "unsuccessfully" : "successfully");
        switch (extra1) {
        case 401:
            // TODO: reset cached password
            SE_LOG_INFO(NULL, NULL, "authorization failed, check username '%s' and password", getUsername());
            break;
        case 403:
            SE_LOG_INFO(&source, NULL, "log in succeeded, but server refuses access - contact server operator");
            break;
        case 407:
            SE_LOG_INFO(NULL, NULL, "proxy authorization failed, check proxy username and password");
            break;
        case 404:
            SE_LOG_INFO(&source, NULL, "server database not found, check URI '%s'", source.getURI());
            break;
        }
        source.recordStatus(SyncMLStatus(extra1));
        break;
    case sysync::PEV_DSSTATS_L:
        /* datastore statistics for local       (extra1=# added, 
           extra2=# updated,
           extra3=# deleted) */
        source.setItemStat(EvolutionSyncSource::ITEM_LOCAL,
                           EvolutionSyncSource::ITEM_ADDED,
                           EvolutionSyncSource::ITEM_TOTAL,
                           extra1);
        source.setItemStat(EvolutionSyncSource::ITEM_LOCAL,
                           EvolutionSyncSource::ITEM_UPDATED,
                           EvolutionSyncSource::ITEM_TOTAL,
                           extra2);
        source.setItemStat(EvolutionSyncSource::ITEM_LOCAL,
                           EvolutionSyncSource::ITEM_REMOVED,
                           EvolutionSyncSource::ITEM_TOTAL,
                           extra3);
        break;
    case sysync::PEV_DSSTATS_R:
        /* datastore statistics for remote      (extra1=# added, 
           extra2=# updated,
           extra3=# deleted) */
        source.setItemStat(EvolutionSyncSource::ITEM_REMOTE,
                           EvolutionSyncSource::ITEM_ADDED,
                           EvolutionSyncSource::ITEM_TOTAL,
                           extra1);
        source.setItemStat(EvolutionSyncSource::ITEM_REMOTE,
                           EvolutionSyncSource::ITEM_UPDATED,
                           EvolutionSyncSource::ITEM_TOTAL,
                           extra2);
        source.setItemStat(EvolutionSyncSource::ITEM_REMOTE,
                           EvolutionSyncSource::ITEM_REMOVED,
                           EvolutionSyncSource::ITEM_TOTAL,
                           extra3);
        break;
    case sysync::PEV_DSSTATS_E:
        /* datastore statistics for local/remote rejects (extra1=# locally rejected, 
           extra2=# remotely rejected) */
        source.setItemStat(EvolutionSyncSource::ITEM_LOCAL,
                           EvolutionSyncSource::ITEM_ANY,
                           EvolutionSyncSource::ITEM_REJECT,
                           extra1);
        source.setItemStat(EvolutionSyncSource::ITEM_REMOTE,
                           EvolutionSyncSource::ITEM_ANY,
                           EvolutionSyncSource::ITEM_REJECT,
                           extra2);
        break;
    case sysync::PEV_DSSTATS_S:
        /* datastore statistics for server slowsync  (extra1=# slowsync matches) */
        source.setItemStat(EvolutionSyncSource::ITEM_REMOTE,
                           EvolutionSyncSource::ITEM_ANY,
                           EvolutionSyncSource::ITEM_MATCH,
                           extra1);
        break;
    case sysync::PEV_DSSTATS_C:
        /* datastore statistics for server conflicts (extra1=# server won,
           extra2=# client won,
           extra3=# duplicated) */
        source.setItemStat(EvolutionSyncSource::ITEM_REMOTE,
                           EvolutionSyncSource::ITEM_ANY,
                           EvolutionSyncSource::ITEM_CONFLICT_SERVER_WON,
                           extra1);
        source.setItemStat(EvolutionSyncSource::ITEM_REMOTE,
                           EvolutionSyncSource::ITEM_ANY,
                           EvolutionSyncSource::ITEM_CONFLICT_CLIENT_WON,
                           extra2);
        source.setItemStat(EvolutionSyncSource::ITEM_REMOTE,
                           EvolutionSyncSource::ITEM_ANY,
                           EvolutionSyncSource::ITEM_CONFLICT_DUPLICATED,
                           extra3);
        break;
    case sysync::PEV_DSSTATS_D:
        /* datastore statistics for data   volume    (extra1=outgoing bytes,
           extra2=incoming bytes) */
        source.setItemStat(EvolutionSyncSource::ITEM_LOCAL,
                           EvolutionSyncSource::ITEM_ANY,
                           EvolutionSyncSource::ITEM_SENT_BYTES,
                           extra1);
        source.setItemStat(EvolutionSyncSource::ITEM_LOCAL,
                           EvolutionSyncSource::ITEM_ANY,
                           EvolutionSyncSource::ITEM_RECEIVED_BYTES,
                           extra2);
        break;
    default:
        SE_LOG_DEBUG(NULL, NULL, "%s: progress event %d, extra %d/%d/%d",
                  source.getName(),
                  type, extra1, extra2, extra3);
    }
}

void EvolutionSyncClient::throwError(const string &error)
{
#ifdef IPHONE
    /*
     * Catching the runtime_exception fails due to a toolchain problem,
     * so do the error handling now and abort: because there is just
     * one sync source this is probably the only thing that can be done.
     * Still, it's not nice on the server...
     */
    fatalError(NULL, error.c_str());
#else
    throw runtime_error(error);
#endif
}

void EvolutionSyncClient::throwError(const string &action, int error)
{
    throwError(action + ": " + strerror(error));
}

void EvolutionSyncClient::fatalError(void *object, const char *error)
{
    SE_LOG_ERROR(NULL, NULL, "%s", error);
    if (m_sourceListPtr) {
        m_sourceListPtr->syncDone(false, NULL);
    }
    exit(1);
}

/*
 * There have been segfaults inside glib in the background
 * thread which ran the second event loop. Disabled it again,
 * even though the synchronous EDS API calls will block then
 * when EDS dies.
 */
#if 0 && defined(HAVE_GLIB) && defined(HAVE_EDS)
# define RUN_GLIB_LOOP
#endif

#ifdef RUN_GLIB_LOOP
#include <pthread.h>
#include <signal.h>
static void *mainLoopThread(void *)
{
    // The test framework uses SIGALRM for timeouts.
    // Block the signal here because a) the signal handler
    // prints a stack back trace when called and we are not
    // interessted in the background thread's stack and b)
    // it seems to have confused glib/libebook enough to
    // access invalid memory and segfault when it gets the SIGALRM.
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);

    GMainLoop *mainloop = g_main_loop_new(NULL, TRUE);
    if (mainloop) {
        g_main_loop_run(mainloop);
        g_main_loop_unref(mainloop);
    }
    return NULL;
}
#endif

void EvolutionSyncClient::startLoopThread()
{
#ifdef RUN_GLIB_LOOP
    // when using Evolution we must have a running main loop,
    // otherwise loss of connection won't be reported to us
    static pthread_t loopthread;
    static bool loopthreadrunning;
    if (!loopthreadrunning) {
        loopthreadrunning = !pthread_create(&loopthread, NULL, mainLoopThread, NULL);
    }
#endif
}

EvolutionSyncSource *EvolutionSyncClient::findSource(const char *name)
{
    return m_sourceListPtr ? (*m_sourceListPtr)[name] : NULL;
}

void EvolutionSyncClient::createSyncReport(SyncReport &report, SourceList &sourceList) const
{
    report.clear();
    BOOST_FOREACH(EvolutionSyncSource *source, sourceList) {
        report.addSyncSourceReport(source->getName(), *source);
    }
}


void EvolutionSyncClient::setConfigFilter(bool sync, const FilterConfigNode::ConfigFilter &filter)
{
    map<string, string>::const_iterator hasSync = filter.find(EvolutionSyncSourceConfig::m_sourcePropSync.getName());

    if (!sync && hasSync != filter.end()) {
        m_overrideMode = hasSync->second;
        FilterConfigNode::ConfigFilter strippedFilter = filter;
        strippedFilter.erase(EvolutionSyncSourceConfig::m_sourcePropSync.getName());
        EvolutionSyncConfig::setConfigFilter(sync, strippedFilter);
    } else {
        EvolutionSyncConfig::setConfigFilter(sync, filter);
    }
}

void EvolutionSyncClient::initSources(SourceList &sourceList)
{
    set<string> unmatchedSources = m_sources;
    list<string> configuredSources = getSyncSources();
    BOOST_FOREACH(const string &name, configuredSources) {
        boost::shared_ptr<PersistentEvolutionSyncSourceConfig> sc(getSyncSourceConfig(name));
        
        // is the source enabled?
        string sync = sc->getSync();
        bool enabled = sync != "disabled";
        string overrideMode = m_overrideMode;

        // override state?
        if (m_sources.size()) {
            if (m_sources.find(sc->getName()) != m_sources.end()) {
                if (!enabled) {
                    if (overrideMode.empty()) {
                        overrideMode = "two-way";
                    }
                    enabled = true;
                }
                unmatchedSources.erase(sc->getName());
            } else {
                enabled = false;
            }
        }
        
        if (enabled) {
            string url = getSyncURL();
            boost::replace_first(url, "https://", "http://"); // do not distinguish between protocol in change tracking
            string changeId = string("sync4jevolution:") + url + "/" + name;
            EvolutionSyncSourceParams params(name,
                                             getSyncSourceNodes(name),
                                             changeId);
            // the sync mode has to be set before instantiating the source
            // because the client library reads the preferredSyncMode at that time
            if (!overrideMode.empty()) {
                params.m_nodes.m_configNode->addFilter(EvolutionSyncSourceConfig::m_sourcePropSync.getName(),
                                                       overrideMode);
            }
            EvolutionSyncSource *syncSource =
                EvolutionSyncSource::createSource(params);
            if (!syncSource) {
                throwError(name + ": type unknown" );
            }
            sourceList.push_back(syncSource);
        }
    }

    // check whether there were any sources specified which do not exist
    if (unmatchedSources.size()) {
        throwError(string("no such source(s): ") + boost::join(unmatchedSources, " "));
    }
}

// syncevolution.xml converted to C string constant
extern "C" {
    extern const char *SyncEvolutionXML;
}

void EvolutionSyncClient::getConfigTemplateXML(string &xml, string &configname)
{
    try {
        configname = "syncevolution.xml";
        if (ReadFile(configname, xml)) {
            return;
        }
    } catch (...) {
        SyncEvolutionException::handle();
    }

    /**
     * @TODO read from config directory
     */

    configname = "builtin XML configuration";
    xml = SyncEvolutionXML;
}



void EvolutionSyncClient::getConfigXML(string &xml, string &configname)
{
    getConfigTemplateXML(xml, configname);

    string tag;
    size_t index;

    tag = "<debug/>";
    index = xml.find(tag);
    if (index != xml.npos) {
        stringstream debug;
        bool logging = !m_sourceListPtr->getLogdir().empty();

        // @TODO be more selective about which Synthesis logging
        // options we enable. Currently it logs everything when logging
        // at all.

        debug <<
            "  <debug>\n"
            // logpath is a config variable set by EvolutionSyncClient::doSync()
            "    <logpath>$(logpath)</logpath>\n"
            "    <logflushmode>flush</logflushmode>\n"
            "    <logformat>html</logformat>\n"
            "    <folding>auto</folding>\n"
            "    <timestamp>yes</timestamp>\n"
            "    <timestampall>no</timestampall>\n"
            "    <timedsessionlognames>no</timedsessionlognames>\n"
            "    <subthreadmode>suppress</subthreadmode>\n"
            "    <singlegloballog>yes</singlegloballog>\n";
        if (logging) {
            debug <<
                "    <sessionlogs>yes</sessionlogs>\n"
                "    <globallogs>yes</globallogs>\n"
                "    <msgdump>yes</msgdump>\n"
                "    <xmltranslate>yes</xmltranslate>\n"
                "    <enable option=\"all\"/>\n"
                "    <enable option=\"userdata\"/>\n"
                "    <enable option=\"scripts\"/>\n"
                "    <enable option=\"exotic\"/>\n";
        } else {
            debug <<
                "    <sessionlogs>no</sessionlogs>\n"
                "    <globallogs>no</globallogs>\n"
                "    <msgdump>no</msgdump>\n"
                "    <xmltranslate>no</xmltranslate>\n"
                "    <disable option=\"all\"/>";
        }
        debug <<
            "  </debug>\n";

        xml.replace(index, tag.size(), debug.str());
    }

    tag = "<datastore/>";
    index = xml.find(tag);
    if (index != xml.npos) {
        stringstream datastores;

        BOOST_FOREACH(EvolutionSyncSource *source, *m_sourceListPtr) {
            string fragment;
            source->getDatastoreXML(fragment);
            unsigned long hash = Hash(source->getName()) % INT_MAX;

            /**
             * @TODO handle hash collisions
             */
            if (!hash) {
                hash = 1;
            }
            datastores << "    <datastore name='" << source->getName() << "' type='plugin'>\n" <<
                "      <dbtypeid>" << hash << "</dbtypeid>\n" <<
                fragment <<
                "    </datastore>\n\n";
        }
        xml.replace(index, tag.size(), datastores.str());
    }

    tag = "<fakedeviceid/>";
    index = xml.find(tag);
    if (index != xml.npos) {
        string fakeid;
        fakeid += "<fakedeviceid>";
        fakeid += getDevID();
        fakeid += "</fakedeviceid>";
        xml.replace(index, tag.size(), fakeid);
    }
}

SyncMLStatus EvolutionSyncClient::sync(SyncReport *report)
{
    SyncMLStatus status = STATUS_OK;
    
    if (!exists()) {
        SE_LOG_ERROR(NULL, NULL, "No configuration for server \"%s\" found.", m_server.c_str());
        throwError("cannot proceed without configuration");
    }

    // redirect logging as soon as possible
    SourceList sourceList(m_server, m_doLogging, m_quiet);
    m_sourceListPtr = &sourceList;

    SyncReport buffer;
    if (!report) {
        report = &buffer;
    }

    try {
        // let derived classes override settings, like the log dir
        prepare();

        sourceList.setLogdir(getLogDir(),
                             getMaxLogDirs(),
                             getLogLevel());

        // dump some summary information at the beginning of the log
        SE_LOG_DEV(NULL, NULL, "SyncML server account: %s", getUsername());
        SE_LOG_DEV(NULL, NULL, "client: SyncEvolution %s for %s", getSwv(), getDevType());
        SE_LOG_DEV(NULL, NULL, "device ID: %s", getDevID());
        SE_LOG_DEV(NULL, NULL, "%s", EDSAbiWrapperDebug());

        // instantiate backends, but do not open them yet
        initSources(sourceList);

        // request all config properties once: throwing exceptions
        // now is okay, whereas later it would lead to leaks in the
        // not exception safe client library
        EvolutionSyncConfig dummy;
        set<string> activeSources = sourceList.getSources();
        dummy.copy(*this, &activeSources);

        // start background thread if not running yet:
        // necessary to catch problems with Evolution backend
        startLoopThread();

        // ask for passwords now
        checkPassword(*this);
        if (getUseProxy()) {
            checkProxyPassword(*this);
        }
        BOOST_FOREACH(EvolutionSyncSource *source, sourceList) {
            source->checkPassword(*this);
        }

        // open each source - failing now is still safe
        BOOST_FOREACH(EvolutionSyncSource *source, sourceList) {
            source->open();
        }

        // give derived class also a chance to update the configs
        prepare(sourceList);

	// ready to go: dump initial databases and prepare for final report
	sourceList.syncPrepare();

        // run sync session
        status = doSync();
    } catch (...) {
        SyncEvolutionException::handle(&status);
    }

    try {
        // Print final report before cleaning up.
        // Status was okay only if all sources succeeded.
        createSyncReport(*report, sourceList);
        BOOST_FOREACH(EvolutionSyncSource *source, sourceList) {
            if (source->getStatus() != STATUS_OK &&
                status == STATUS_OK) {
                status = source->getStatus();
                break;
            }
        }
        sourceList.syncDone(status == STATUS_OK, report);
    } catch(...) {
        SyncEvolutionException::handle(&status);
    }

    m_sourceListPtr = NULL;
    return status;
}

SyncMLStatus EvolutionSyncClient::doSync()
{
    SyncMLStatus status = STATUS_OK;

    // Synthesis SDK
    sysync::TSyError err;
    sysync::KeyH keyH;
    sysync::KeyH subkeyH;
    string s;

    // create new sync engine for the duration of this function
    class SwapEngine {
        EvolutionSyncClient &m_client;
        sysync::TEngineModuleBridge *m_oldengine;

    public:
        SwapEngine(EvolutionSyncClient &client) :
            m_client(client) {
            sysync::TEngineModuleBridge *syncengine = new sysync::TEngineModuleBridge();
            sysync::TSyError err;
            if (!syncengine ||
                (err = syncengine->Connect("[]", 0,
                                           sysync::DBG_PLUGIN_NONE|
                                           sysync::DBG_PLUGIN_INT|
                                           sysync::DBG_PLUGIN_DB|
                                           sysync::DBG_PLUGIN_EXOT|
                                           sysync::DBG_PLUGIN_ALL))) {
                m_client.throwError("create Syntesis engine for sync session");
            }
            m_oldengine = m_client.swapEngine(syncengine);
        }

        ~SwapEngine() {
            sysync::TEngineModuleBridge *syncengine =
                m_client.swapEngine(m_oldengine);
            delete syncengine;
        }
    } swapengine(*this);

    err = getEngine().OpenKeyByPath(keyH, NULL, "/configvars", 0);
    if (err) {
        throwError("open config vars");
    }
    string logdir = m_sourceListPtr->getLogdir();
    if (logdir.size()) {
        getEngine().SetStrValue(keyH, "defout_path", logdir);
    } else {
        getEngine().SetStrValue(keyH, "defout_path", "/dev/null");
    }
    getEngine().SetStrValue(keyH, "device_uri", getDevID());
    getEngine().SetStrValue(keyH, "device_name", getDevType());
    // TODO: redirect to log file?
    getEngine().SetStrValue(keyH, "conferrpath", "console");
    getEngine().SetStrValue(keyH, "binfilepath", getRootPath() + "/.synthesis");
    getEngine().CloseKey(keyH);

    string xml, configname;
    getConfigXML(xml, configname);
    SE_LOG_DEBUG(NULL, NULL, "%s", xml.c_str());
    err = getEngine().InitEngineXML(xml.c_str());
    if (err) {
        throwError(string("error parsing ") + configname);
    }

    err = getEngine().OpenKeyByPath(keyH, NULL, "/profiles", 0);
    if (err) {
        throwError("open profiles");
    }
  
    // check the settings status (MUST BE DONE TO MAKE SETTINGS READY)
    err = getEngine().GetStrValue(keyH, "settingsstatus", s);
    // allow creating new settings when existing settings are not up/downgradeable
    err = getEngine().SetStrValue(keyH, "overwrite",  "1" );
    // check status again
    err = getEngine().GetStrValue(keyH, "settingsstatus", s);
    
    // open first profile
    err = getEngine().OpenSubkey(subkeyH, keyH, sysync::KEYVAL_ID_FIRST, 0);
    if (err == 204) { // DB_NoContent
        // no profile already exists, create default profile
        err = getEngine().OpenSubkey(subkeyH, keyH, sysync::KEYVAL_ID_NEW_DEFAULT, 0);
    }
    if (err) {
        throwError("open first profile");
    }
         
    getEngine().SetStrValue(subkeyH, "serverURI", getSyncURL());
    getEngine().SetStrValue(subkeyH, "serverUser", getUsername());
    getEngine().SetStrValue(subkeyH, "serverPassword", getPassword());

    // TODO(?): make XML vs WBXML configurable
    // getEngine().SetInt32Value(subkeyH, "encoding", 2);

    // Iterate over all data stores in the XML config
    // and match them with sync sources.
    // TODO: let sync sources provide their own
    // XML snippets (inside <client> and inside <datatypes>).
    sysync::KeyH targetsH, targetH;
    err = getEngine().OpenKeyByPath(targetsH, subkeyH, "targets", 0);
    if (err) {
        throwError("targets");
    }
    err = getEngine().OpenSubkey(targetH, targetsH, sysync::KEYVAL_ID_FIRST, 0);
    while (err != 204) {
        if (err) {
            throwError("reading target");
        }
        err = getEngine().GetStrValue(targetH, "dbname", s);
        if (err) {
            throwError("reading target name");
        }
        EvolutionSyncSource *source = (*m_sourceListPtr)[s];
        if (source) {
            getEngine().SetInt32Value(targetH, "enabled", 1);
            int slow = 0;
            int direction = 0;
            string mode = source->getSync();
            if (!strcasecmp(mode.c_str(), "slow")) {
                slow = 1;
                direction = 0;
            } else if (!strcasecmp(mode.c_str(), "two-way")) {
                slow = 0;
                direction = 0;
            } else if (!strcasecmp(mode.c_str(), "refresh-from-server")) {
                slow = 1;
                direction = 1;
            } else if (!strcasecmp(mode.c_str(), "refresh-from-client")) {
                slow = 1;
                direction = 2;
            } else if (!strcasecmp(mode.c_str(), "one-way-from-server")) {
                slow = 0;
                direction = 1;
            } else if (!strcasecmp(mode.c_str(), "one-way-from-client")) {
                slow = 0;
                direction = 2;
            } else {
                source->throwError(string("invalid sync mode: ") + mode);
            }
            getEngine().SetInt32Value(targetH, "forceslow", slow);
            getEngine().SetInt32Value(targetH, "syncmode", direction);

            getEngine().SetStrValue(targetH, "remotepath", source->getURI());
        } else {
            getEngine().SetInt32Value(targetH, "enabled", 0);
        }
        getEngine().CloseKey(targetH);
        err = getEngine().OpenSubkey(targetH, targetsH, sysync::KEYVAL_ID_NEXT, 0);
    }

    // run an HTTP client sync session
    boost::shared_ptr<TransportAgent> agent(createTransportAgent());
    if (getUseProxy()) {
        agent->setProxy(getProxyHost());
        agent->setProxyAuth(getProxyUsername(),
                            getProxyPassword());
    }
    agent->setUserAgent(getUserAgent());
    // TODO: SSL settings

    // Close all keys so that engine can flush the modified config.
    // Otherwise the session reads the unmodified values from the
    // created files while the updated values are still in memory.
    getEngine().CloseKey(targetsH);
    getEngine().CloseKey(subkeyH);
    getEngine().CloseKey(keyH);

    // reopen profile keys
    err = getEngine().OpenKeyByPath(keyH, NULL, "/profiles", 0);
    if (err) {
        throwError("open config vars");
    }
    err = getEngine().GetStrValue(keyH, "settingsstatus", s);
    if (err) {
        throwError("settings not ready");
    }
    err = getEngine().OpenSubkey(subkeyH, keyH, sysync::KEYVAL_ID_FIRST, 0);
    if (err) {
        throwError("open first profile");
    }
    err = getEngine().OpenKeyByPath(targetsH, subkeyH, "targets", 0);
    if (err) {
        throwError("targets");
    }

    sysync::SessionH sessionH;
    sysync::TEngineProgressInfo progressInfo;
    sysync::uInt16 stepCmd = sysync::STEPCMD_CLIENTSTART; // first step
    err = getEngine().OpenSession(sessionH, 0, "syncevolution_session");
    if (err) {
        throwError("OpenSession");
    }

    // Sync main loop: runs until SessionStep() signals end or error.
    // Exceptions are caught and lead to a call of SessionStep() with
    // parameter STEPCMD_ABORT -> abort session as soon as possible.
    sysync::memSize length = 0;
    bool aborting = false;
    sysync::uInt16 previousStepCmd = stepCmd;
    do {
        try {
            // take next step, but don't abort twice: instead
            // let engine contine with its shutdown
            if (stepCmd == sysync::STEPCMD_ABORT) {
                if (aborting) {
                    stepCmd = previousStepCmd;
                } else {
                    aborting = true;
                }
            }
            err = getEngine().SessionStep(sessionH, stepCmd, &progressInfo);
            previousStepCmd = stepCmd;
            if (err != sysync::LOCERR_OK) {
                // error, terminate with error
                stepCmd = sysync::STEPCMD_ERROR;
            } else {
                // step ran ok, evaluate step command
                switch (stepCmd) {
                case sysync::STEPCMD_OK:
                    // no progress info, call step again
                    stepCmd = sysync::STEPCMD_STEP;
                    break;
                case sysync::STEPCMD_PROGRESS:
                    // new progress info to show
                    // Check special case of interactive display alert
                    if (progressInfo.eventtype == sysync::PEV_DISPLAY100) {
                        // alert 100 received from remote, message text is in
                        // SessionKey's "displayalert" field
                        sysync::KeyH sessionKeyH;
                        err = getEngine().OpenSessionKey(sessionH, sessionKeyH, 0);
                        if (err != sysync::LOCERR_OK) {
                            throwError("session key");
                        }
                        // get message from server to display
                        getEngine().GetStrValue(sessionKeyH,
                                                "displayalert",
                                                s);
                        displayServerMessage(s);
                        getEngine().CloseKey(sessionKeyH);
                    } else {
                        switch (progressInfo.targetID) {
                        case sysync::KEYVAL_ID_UNKNOWN:
                        case 0 /* used with PEV_SESSIONSTART?! */:
                            displaySyncProgress(sysync::TProgressEventEnum(progressInfo.eventtype),
                                                progressInfo.extra1,
                                                progressInfo.extra2,
                                                progressInfo.extra3);
                            break;
                        default:
                            // specific for a certain sync source:
                            // find it...
                            err = getEngine().OpenSubkey(targetH, targetsH, progressInfo.targetID, 0);
                            if (err) {
                                throwError("reading target");
                            }
                            err = getEngine().GetStrValue(targetH, "dbname", s);
                            if (err) {
                                throwError("reading target name");
                            }
                            EvolutionSyncSource *source = (*m_sourceListPtr)[s];
                            if (source) {
                                displaySourceProgress(sysync::TProgressEventEnum(progressInfo.eventtype),
                                                      *source,
                                                      progressInfo.extra1,
                                                      progressInfo.extra2,
                                                      progressInfo.extra3);
                            } else {
                                throwError(std::string("unknown target ") + s);
                            }
                            getEngine().CloseKey(targetH);
                            break;
                        }
                    }
                    stepCmd = sysync::STEPCMD_STEP;
                    break;
                case sysync::STEPCMD_ERROR:
                    // error, terminate (should not happen, as status is
                    // already checked above)
                    break;
                case sysync::STEPCMD_RESTART:
                    // make sure connection is closed and will be re-opened for next request
                    // tbd: close communication channel if still open to make sure it is
                    //       re-opened for the next request
                    stepCmd = sysync::STEPCMD_STEP;
                    break;
                case sysync::STEPCMD_SENDDATA: {
                    // send data to remote

                    // use OpenSessionKey() and GetValue() to retrieve "connectURI"
                    // and "contenttype" to be used to send data to the server
                    sysync::KeyH sessionKeyH;
                    err = getEngine().OpenSessionKey(sessionH, sessionKeyH, 0);
                    if (err != sysync::LOCERR_OK) {
                        throwError("session key");
                    }
                    getEngine().GetStrValue(sessionKeyH,
                                            "connectURI",
                                            s);
                    agent->setURL(s);
                    string contenttype;
                    getEngine().GetStrValue(sessionKeyH,
                                            "contenttype",
                                            contenttype);
                    getEngine().CloseKey(sessionKeyH);
                    agent->setContentType(contenttype);
                        
                    // use GetSyncMLBuffer()/RetSyncMLBuffer() to access the data to be
                    // sent or have it copied into caller's buffer using
                    // ReadSyncMLBuffer(), then send it to the server
                    sysync::appPointer buffer;
                    err = getEngine().GetSyncMLBuffer(sessionH, true, buffer, length);
                    if (err) {
                        throwError("buffer");
                    }
                    agent->send(static_cast<const char *>(buffer), length);
                    stepCmd = sysync::STEPCMD_SENTDATA; // we have sent the data
                    break;
                }
                case sysync::STEPCMD_NEEDDATA:
                    switch (agent->wait()) {
                    case TransportAgent::ACTIVE:
                        stepCmd = sysync::STEPCMD_SENTDATA; // still sending the data?!
                        break;
                    case TransportAgent::GOT_REPLY:
                        getEngine().RetSyncMLBuffer(sessionH, true, length);
                        const char *reply;
                        size_t replylen;
                        agent->getReply(reply, replylen);

                        // put answer r eceived earlier into SyncML engine's buffer
                        err = getEngine().WriteSyncMLBuffer(sessionH,
                                                            const_cast<void *>(static_cast<const void *>(reply)),
                                                            replylen);
                        if (err) {
                            throwError("write buffer");
                        }
                        stepCmd = sysync::STEPCMD_GOTDATA; // we have received response data
                        break;
                    default:
                        stepCmd = sysync::STEPCMD_TRANSPFAIL; // communication with server failed
                        break;
                    }
                }
            }
            // check for suspend or abort, if so, modify step command for next step
            if (false /* tdb: check if user requests suspending the session */) {
                stepCmd = sysync::STEPCMD_SUSPEND;
            }
            if (false /* tdb: check if user requests aborting the session */) {
                stepCmd = sysync::STEPCMD_ABORT;
            }
            // loop until session done or aborted with error
        } catch (...) {
            SyncEvolutionException::handle(&status);
            stepCmd = sysync::STEPCMD_ABORT;
        }
    } while (stepCmd != sysync::STEPCMD_DONE && stepCmd != sysync::STEPCMD_ERROR);
    getEngine().CloseKey(targetsH);
    getEngine().CloseKey(subkeyH);
    getEngine().CloseKey(keyH);
    getEngine().CloseSession(sessionH);

    return status;
}


void EvolutionSyncClient::status()
{
    EvolutionSyncConfig config(m_server);
    if (!exists()) {
        SE_LOG_ERROR(NULL, NULL, "No configuration for server \"%s\" found.", m_server.c_str());
        throwError("cannot proceed without configuration");
    }

    SourceList sourceList(m_server, false, false);
    initSources(sourceList);
    BOOST_FOREACH(EvolutionSyncSource *source, sourceList) {
        source->checkPassword(*this);
    }
    BOOST_FOREACH(EvolutionSyncSource *source, sourceList) {
        source->open();
    }

    sourceList.setLogdir(getLogDir(), 0, 0);
    LoggerBase::instance().setLevel(Logger::INFO);
    string prevLogdir = sourceList.getPrevLogdir();
    bool found = access(prevLogdir.c_str(), R_OK|X_OK) == 0;

    if (found) {
        try {
            sourceList.setPath(prevLogdir);
            sourceList.dumpDatabases("current");
            sourceList.dumpLocalChanges("after", "current");
        } catch(...) {
            SyncEvolutionException::handle();
        }
    } else {
        cerr << "Previous log directory not found.\n";
        if (!getLogDir() || !getLogDir()[0]) {
            cerr << "Enable the 'logdir' option and synchronize to use this feature.\n";
        }
    }
}
