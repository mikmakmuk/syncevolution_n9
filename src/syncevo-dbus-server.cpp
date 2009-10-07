/*
 * Copyright (C) 2009 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gdbus-cxx-bridge.h"
#include <syncevo/Logging.h>
#include <syncevo/util.h>
#include <syncevo/SyncContext.h>
#include <syncevo/SoupTransportAgent.h>
#include <syncevo/SyncSource.h>

#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include <list>
#include <map>
#include <memory>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <boost/noncopyable.hpp>

#include <glib-object.h>

using namespace SyncEvo;

static GMainLoop *loop = NULL;

/**
 * Anything that can be owned by a client, like a connection
 * or session.
 */
class Resource {
public:
    virtual ~Resource() {}
};

class Session;
class Connection;
class Client;
class DBusTransportAgent;

/**
 * Implements the read-only methods in a Session and the Server.
 * Only data is the server configuration name, everything else
 * is created and destroyed inside the methods.
 */
class ReadOperations
{
public:
    const std::string m_configName;

    ReadOperations(const std::string &config_name);

    /** the double dictionary used to represent configurations */
    typedef std::map< std::string, StringMap > Config_t;

    /** the array of reports filled by getReports() */
    typedef std::vector< StringMap > Reports_t;

    /** the array of databases used by getDatabases() */
    typedef SyncSource::Database SourceDatabase;
    typedef SyncSource::Databases SourceDatabases_t;

    /** implementation of D-Bus GetConfig() for m_configName as server configuration */
    void getConfig(bool getTemplate,
                   Config_t &config);

    /** implementation of D-Bus GetReports() for m_configName as server configuration */
    void getReports(uint32_t start, uint32_t count,
                    Reports_t &reports);

    void checkSource(const string &sourceName);

    void getDatabases(const string &sourceName, SourceDatabases_t &databases);
};

/**
 * dbus_traits for SourceDatabase. Put it here for 
 * avoiding polluting gxx-dbus-bridge.h
 */
template<> struct dbus_traits<ReadOperations::SourceDatabase> :
    public dbus_struct_traits<ReadOperations::SourceDatabase,
                              dbus_member<ReadOperations::SourceDatabase, std::string, &ReadOperations::SourceDatabase::m_name,
                              dbus_member<ReadOperations::SourceDatabase, std::string, &ReadOperations::SourceDatabase::m_uri,
                              dbus_member_single<ReadOperations::SourceDatabase, bool, &ReadOperations::SourceDatabase::m_isDefault> > > >{}; 


/**
 * Implements the main org.syncevolution.Server interface.
 *
 * All objects created by it get a reference to the creating
 * DBusServer instance so that they can call some of its
 * methods. Because that instance holds references to all
 * of these objects and deletes them before destructing itself,
 * that reference is guaranteed to remain valid.
 */
class DBusServer : public DBusObjectHelper
{
    GMainLoop *m_loop;
    uint32_t m_lastSession;
    typedef std::list< std::pair< boost::shared_ptr<Watch>, boost::shared_ptr<Client> > > Clients_t;
    Clients_t m_clients;

    /**
     * The session which currently holds the main lock on the server.
     * To avoid issues with concurrent modification of data or configs,
     * only one session may make such modifications at a time. A
     * plain pointer which is reset by the session's deconstructor.
     *
     * A weak pointer alone did not work because it does not provide access
     * to the underlying pointer after the last corresponding shared
     * pointer is gone (which triggers the deconstructing of the session).
     */
    Session *m_activeSession;

    /**
     * The weak pointer that corresponds to m_activeSession.
     */
    boost::weak_ptr<Session> m_activeSessionRef;

    /**
     * The running sync session. Having a separate reference to it
     * ensures that the object won't go away prematurely, even if all
     * clients disconnect.
     */
    boost::shared_ptr<Session> m_syncSession;

    typedef std::list< boost::weak_ptr<Session> > WorkQueue_t;
    /**
     * A queue of pending, idle Sessions. Sorted by priority, most
     * important one first. Currently this is used to give client
     * requests a boost over remote connections and (in the future)
     * automatic syncs.
     *
     * Active sessions are removed from this list and then continue
     * to exist as long as a client in m_clients references it or
     * it is the currently running sync session (m_syncSession).
     */
    WorkQueue_t m_workQueue;

    /**
     * Watch callback for a specific client or connection.
     */
    void clientGone(Client *c);

    /**
     * Returns new unique session ID. Implemented with a running
     * counter. Checks for overflow, but not currently for active
     * sessions.
     */
    std::string getNextSession();

    void attachClient(const Caller_t &caller,
                      const boost::shared_ptr<Watch> &watch);

    void detachClient(const Caller_t &caller);

    void connect(const Caller_t &caller,
                 const boost::shared_ptr<Watch> &watch,
                 const StringMap &peer,
                 bool must_authenticate,
                 const std::string &session,
                 DBusObject_t &object);

    void startSession(const Caller_t &caller,
                      const boost::shared_ptr<Watch> &watch,
                      const std::string &server,
                      DBusObject_t &object);

    void getConfig(const std::string &config_name,
                   bool getTemplate,
                   ReadOperations::Config_t &config)
    {
        ReadOperations ops(config_name);
        ops.getConfig(getTemplate , config);
    }

    void getReports(const std::string &config_name,
                    uint32_t start, uint32_t count,
                    ReadOperations::Reports_t &reports)
    {
        ReadOperations ops(config_name);
        ops.getReports(start, count, reports);
    }

    void checkPresence(const std::string &server,
                       std::string &status,
                       std::vector<std::string> &transports);

    EmitSignal2<const DBusObject_t &,
                bool> sessionChanged;

    EmitSignal3<const std::string &,
                const std::string &,
                const std::string &> presence;

public:
    DBusServer(GMainLoop *loop, const DBusConnectionPtr &conn);
    ~DBusServer();

    /** access to the GMainLoop reference used by this DBusServer instance */
    GMainLoop *getLoop() { return m_loop; }

    /** register in D-Bus */
    void activate();
    /** process D-Bus calls until the server is ready to quit */
    void run();

    /**
     * look up client by its ID
     */
    boost::shared_ptr<Client> findClient(const Caller_t &ID);

    /**
     * find client by its ID or create one anew
     */
    boost::shared_ptr<Client> addClient(const DBusConnectionPtr &conn,
                                        const Caller_t &ID,
                                        const boost::shared_ptr<Watch> &watch);

    /** detach this resource from all clients which own it */
    void detach(Resource *resource);

    /**
     * Enqueue a session. Might also make it ready immediately,
     * if nothing else is first in the queue. To be called
     * by the creator of the session, *after* the session is
     * ready to run.
     */
    void enqueue(const boost::shared_ptr<Session> &session);

    /**
     * Remove a session from the work queue. If it is running a sync,
     * it will keep running and nothing will change. Otherwise, if it
     * is "ready" (= holds a lock on its configuration), then release
     * that lock.
     */
    void dequeue(Session *session);

    /**
     * Checks whether the server is ready to run another session
     * and if so, activates the first one in the queue.
     */
    void checkQueue();
};


/**
 * Tracks a single client and all sessions and connections that it is
 * connected to. Referencing them ensures that they stay around as
 * long as needed.
 */
class Client
{
    typedef std::list< boost::shared_ptr<Resource> > Resources_t;
    Resources_t m_resources;

public:
    const Caller_t m_ID;

    Client(const Caller_t &ID) :
        m_ID(ID)
    {}

    ~Client()
    {
        SE_LOG_DEBUG(NULL, NULL, "D-Bus client %s is destructing", m_ID.c_str());
    }
        

    /**
     * Attach a specific resource to this client. As long as the
     * resource is attached, it cannot be freed. Can be called
     * multiple times, which means that detach() also has to be called
     * the same number of times to finally detach the resource.
     */
    void attach(boost::shared_ptr<Resource> resource)
    {
        m_resources.push_back(resource);
    }

    /**
     * Detach once from the given resource. Has to be called as
     * often as attach() to really remove all references to the
     * session. It's an error to call detach() more often than
     * attach().
     */
    void detach(Resource *resource)
    {
        for (Resources_t::iterator it = m_resources.begin();
             it != m_resources.end();
             ++it) {
            if (it->get() == resource) {
                // got it
                m_resources.erase(it);
                return;
            }
        }

        throw std::runtime_error("cannot detach from resource that client is not attached to");
    }
    void detach(boost::shared_ptr<Resource> resource)
    {
        detach(resource.get());
    }

    /**
     * Remove all references to the given resource, regardless whether
     * it was referenced not at all or multiple times.
     */
    void detachAll(Resource *resource) {
        Resources_t::iterator it = m_resources.begin();
        while (it != m_resources.end()) {
            if (it->get() == resource) {
                it = m_resources.erase(it);
            } else {
                ++it;
            }
        }
    }
    void detachAll(boost::shared_ptr<Resource> resource)
    {
        detachAll(resource.get());
    }

    /**
     * return corresponding smart pointer for a certain resource,
     * empty pointer if not found
     */
    boost::shared_ptr<Resource> findResource(Resource *resource)
    {
        for (Resources_t::iterator it = m_resources.begin();
             it != m_resources.end();
             ++it) {
            if (it->get() == resource) {
                // got it
                return *it;
            }
        }
        return boost::shared_ptr<Resource>();
    }
};

struct SourceStatus
{
    SourceStatus() :
        m_mode("none"),
        m_status("idle"),
        m_error(0)
    {}

    std::string m_mode;
    std::string m_status;
    uint32_t m_error;
};

template<> struct dbus_traits<SourceStatus> :
    public dbus_struct_traits<SourceStatus,
                              dbus_member<SourceStatus, std::string, &SourceStatus::m_mode,
                              dbus_member<SourceStatus, std::string, &SourceStatus::m_status,
                              dbus_member_single<SourceStatus, uint32_t, &SourceStatus::m_error> > > >
{};

struct SourceProgress
{
    SourceProgress() :
        m_phase(""),
        m_prepareCount(-1), m_prepareTotal(-1),
        m_sendCount(-1), m_sendTotal(-1),
        m_receiveCount(-1), m_receiveTotal(-1)
    {}

    std::string m_phase;
    int32_t m_prepareCount, m_prepareTotal;
    int32_t m_sendCount, m_sendTotal;
    int32_t m_receiveCount, m_receiveTotal;
};

template<> struct dbus_traits<SourceProgress> :
    public dbus_struct_traits<SourceProgress,
                              dbus_member<SourceProgress, std::string, &SourceProgress::m_phase,
                              dbus_member<SourceProgress, int32_t, &SourceProgress::m_prepareCount,
                              dbus_member<SourceProgress, int32_t, &SourceProgress::m_prepareTotal,
                              dbus_member<SourceProgress, int32_t, &SourceProgress::m_sendCount,
                              dbus_member<SourceProgress, int32_t, &SourceProgress::m_sendTotal,
                              dbus_member<SourceProgress, int32_t, &SourceProgress::m_receiveCount,
                              dbus_member_single<SourceProgress, int32_t, &SourceProgress::m_receiveTotal> > > > > > > >
{};

/**
 * A running sync engine which keeps answering on D-Bus whenever
 * possible and updates the Session while the sync runs.
 */
class DBusSync : public SyncContext
{
    Session &m_session;

public:
    DBusSync(const std::string &config,
             Session &session);
    ~DBusSync() {}

    virtual boost::shared_ptr<TransportAgent> createTransportAgent();
    virtual void displaySyncProgress(sysync::TProgressEventEnum type,
                                     int32_t extra1, int32_t extra2, int32_t extra3);
    virtual void displaySourceProgress(sysync::TProgressEventEnum type,
                                       SyncSource &source,
                                       int32_t extra1, int32_t extra2, int32_t extra3);

    // TODO: hook up abort and suspend requests,
    // activate CTRL-C handling, implement sleep()
};

/**
 * Represents and implements the Session interface.  Use
 * boost::shared_ptr to track it and ensure that there are references
 * to it as long as the connection is needed.
 */
class Session : public DBusObjectHelper,
                public Resource,
                private ReadOperations,
                private boost::noncopyable
{
    DBusServer &m_server;
    const std::string m_sessionID;

    bool m_serverMode;
    SharedBuffer m_initialMessage;
    string m_initialMessageType;

    boost::weak_ptr<Connection> m_connection;
    std::string m_connectionError;
    bool m_useConnection;

    /** temporary config changes */
    FilterConfigNode::ConfigFilter m_syncFilter;
    FilterConfigNode::ConfigFilter m_sourceFilter;
    std::map<std::string, FilterConfigNode::ConfigFilter> m_sourceFilters;

    /**
     * True while clients are allowed to make calls other than Detach(),
     * which is always allowed. Some calls are not allowed while this
     * session runs a sync, which is indicated by a non-NULL m_sync
     * pointer.
     */
    bool m_active;

    /**
     * The SyncEvolution instance which currently prepares or runs a sync.
     */
    boost::shared_ptr<DBusSync> m_sync;

    /** sync was run */
    bool m_done;

    /** premature sync end requested */
    bool m_abort, m_suspend;

    /**
     * Priority which determines position in queue.
     * Lower is more important. PRI_DEFAULT is zero.
     */
    int m_priority;

    int32_t m_progress;
    typedef std::map<std::string, SourceStatus> SourceStatuses_t;
    SourceStatuses_t m_sourceStatus;

    uint32_t m_error;
    typedef std::map<std::string, SourceProgress> SourceProgresses_t;
    SourceProgresses_t m_sourceProgress;

    void detach(const Caller_t &caller);

    void setConfig(bool update, bool temporary,
                   const ReadOperations::Config_t &config);

    void getStatus(std::string &status,
                   uint32_t &error,
                   SourceStatuses_t &sources);
    void getProgress(int32_t &progress,
                     SourceProgresses_t &sources);

    void getConfig(bool getTemplate,
                   ReadOperations::Config_t &config)
    {
        ReadOperations::getConfig(getTemplate , config);
    }

    void getReports(uint32_t start, uint32_t count,
                    ReadOperations::Reports_t &reports)
    {
        ReadOperations::getReports(start, count, reports);
    }
    void checkSource(const string &sourceName) 
    {
        ReadOperations::checkSource(sourceName);
    }
    void getDatabases(const string &sourceName, ReadOperations::SourceDatabases_t &databases)
    {
        ReadOperations::getDatabases(sourceName, databases);
    }

    /**
     * Must be called each time that properties changing the
     * overall status are changed. Ensures that the corresponding
     * D-Bus signal is sent.
     *
     * Doesn't always send the signal immediately, because often it is
     * likely that more status changes will follow shortly. To ensure
     * that the "final" status is sent, call with flush=true.
     *
     * @param flush      force sending the current status
     */
    void fireStatus(bool flush = false);
    /** like fireStatus() for progress information */
    void fireProgress(bool flush = false);

    EmitSignal3<const std::string &,
                uint32_t,
                const SourceStatuses_t &> emitStatus;
    EmitSignal2<int32_t,
                const SourceProgresses_t &> emitProgress;

public:
    Session(DBusServer &server,
            const std::string &config_name,
            const std::string &session);
    ~Session();

    enum {
        PRI_DEFAULT = 0,
        PRI_CONNECTION = 10
    };

    /**
     * Default priority is 0. Higher means less important.
     */
    void setPriority(int priority) { m_priority = priority; }
    int getPriority() const { return m_priority; }

    void initServer(SharedBuffer data, const std::string &messageType);
    void setConnection(const boost::shared_ptr<Connection> c) { m_connection = c; m_useConnection = c; }
    boost::weak_ptr<Connection> getConnection() { return m_connection; }
    bool useConnection() { return m_useConnection; }

    /**
     * After the connection closes, the Connection instance is
     * destructed immediately. This is necessary so that the
     * corresponding cleanup can remove all other classes
     * only referenced by the Connection.
     *
     * This leads to the problem that an active sync cannot
     * query the final error code of the connection. This
     * is solved by setting a generic error code here when
     * the sync starts and overwriting it when the connection
     * closes.
     */
    void setConnectionError(const std::string error) { m_connectionError = error; }
    std::string getConnectionError() { return m_connectionError; }


    DBusServer &getServer() { return m_server; }

    /**
     * activate D-Bus object, session itself not ready yet
     */
    void activate();

    /**
     * TRUE if the session is ready to take over control
     */
    bool readyToRun() { return !m_done && m_sync; }

    /**
     * transfer control to the session for the duration of the sync,
     * returns when the sync is done (successfully or unsuccessfully)
     */
    void run();

    /**
     * called when the session is ready to run (true) or
     * lost the right to make changes (false)
     */
    void setActive(bool active);

    void syncProgress(sysync::TProgressEventEnum type,
                      int32_t extra1, int32_t extra2, int32_t extra3);
    void sourceProgress(sysync::TProgressEventEnum type,
                        SyncSource &source,
                        int32_t extra1, int32_t extra2, int32_t extra3);

    typedef StringMap SourceModes_t;
    void sync(const std::string &mode, const SourceModes_t &source_modes);
    void abort();
    void suspend();
};


/**
 * Represents and implements the Connection interface.
 *
 * The connection interacts with a Session by creating the Session and
 * exchanging data with it. For that, the connection registers itself
 * with the Session and unregisters again when it goes away.
 *
 * In contrast to clients, the Session only keeps a weak_ptr, which
 * becomes invalid when the referenced object gets deleted. Typically
 * this means the Session has to abort, unless reconnecting is
 * supported.
 */
class Connection : public DBusObjectHelper, public Resource
{
    DBusServer &m_server;
    StringMap m_peer;
    bool m_mustAuthenticate;
    enum {
        SETUP,          /**< ready for first message */
        PROCESSING,     /**< received message, waiting for engine's reply */
        WAITING,        /**< waiting for next follow-up message */
        FINAL,          /**< engine has sent final reply, wait for ACK by peer */
        DONE,           /**< peer has closed normally after the final reply */
        FAILED          /**< in a failed state, no further operation possible */
    } m_state;
    std::string m_failure;

    const std::string m_sessionID;
    boost::shared_ptr<Session> m_session;

    /**
     * main loop that our DBusTransportAgent is currently waiting in,
     * NULL if not waiting
     */
    GMainLoop *m_loop;

    /**
     * buffer for received data, waiting here for engine to ask
     * for it via DBusTransportAgent::getReply().
     */
    SharedBuffer m_incomingMsg;
    std::string m_incomingMsgType;

    /**
     * records the reason for the failure, sends Abort signal and puts
     * the connection into the FAILED state.
     */
    void failed(const std::string &reason);

    /**
     * returns "<description> (<ID> via <transport> <transport_description>)"
     */
    static std::string buildDescription(const StringMap &peer);

    void process(const Caller_t &caller,
                 const std::pair<size_t, const uint8_t *> &message,
                 const std::string &message_type);
    void close(const Caller_t &caller,
               bool normal,
               const std::string &error);
    EmitSignal0 abort;
    EmitSignal5<const std::pair<size_t, const uint8_t *> &,
                const std::string &,
                const StringMap &,
                bool,
                const std::string &> reply;

    friend class DBusTransportAgent;

public:
    const std::string m_description;

    Connection(DBusServer &server,
               const DBusConnectionPtr &conn,
               const std::string &session_num,
               const StringMap &peer,
               bool must_authenticate);

    ~Connection();

    void activate();

    /** session requested by us is ready to run a sync */
    void ready();

    /** connection is no longer needed, ensure that it gets deleted */
    void shutdown();
};

/**
 * A proxy for a Connection instance. The Connection instance can go
 * away (weak pointer, must be locked and and checked each time it is
 * needed). The agent must remain available as long as the engine
 * needs and basically becomes unusuable once the connection dies.
 *
 * Reconnecting is not currently supported.
 */ 
class DBusTransportAgent : public TransportAgent
{
    GMainLoop *m_loop;
    Session &m_session;
    boost::weak_ptr<Connection> m_connection;

    std::string m_url;
    std::string m_type;
    TransportCallback m_callback;
    void *m_callbackData;
    int m_callbackInterval;

    SharedBuffer m_incomingMsg;
    std::string m_incomingMsgType;

    void doWait(boost::shared_ptr<Connection> &connection);

 public:
    DBusTransportAgent(GMainLoop *loop,
                       Session &session,
                       boost::weak_ptr<Connection> connection);
    ~DBusTransportAgent();

    virtual void setURL(const std::string &url) { m_url = url; }
    virtual void setContentType(const std::string &type) { m_type = type; }
    virtual void send(const char *data, size_t len);
    virtual void cancel() {}
    virtual void shutdown();
    virtual Status wait(bool noReply = false);
    virtual void setCallback (TransportCallback cb, void * udata, int interval)
    {
        m_callback = cb;
        m_callbackData = udata;
        m_callbackInterval = interval;
    }
    virtual void getReply(const char *&data, size_t &len, std::string &contentType);
};

/***************** ReadOperations implementation ****************/

ReadOperations::ReadOperations(const std::string &config_name) :
    m_configName(config_name)
{}

void ReadOperations::getConfig(bool getTemplate,
                               Config_t &config)
{
    if(m_configName.empty()) {
        throw std::runtime_error("Template name must be given");
    }
    map<string, string> localConfigs;
    boost::shared_ptr<SyncConfig> syncConfig;
    /** get server template */
    if(getTemplate) {
        syncConfig = SyncConfig::createServerTemplate(m_configName);
        if(!syncConfig.get()) {
            throw std::runtime_error("No template '" + m_configName + "' found");
        }
    } else { ///< get a matching server configuration
        boost::shared_ptr<SyncConfig> from;
        syncConfig.reset(new SyncConfig(m_configName));
        /* if config does not exist, create from template */
        if (!syncConfig->exists()) {
            from = SyncConfig::createServerTemplate(m_configName);
            if(!from.get()) {
                throw runtime_error("No server or template '" + m_configName + "' found");
            }
            syncConfig->copy(*from, NULL);
        }
    }

    /** get sync properties and their values */
    ConfigPropertyRegistry &syncRegistry = SyncConfig::getRegistry();
    BOOST_FOREACH(const ConfigProperty *prop, syncRegistry) {
        bool isDefault = false;
        string value = prop->getProperty(syncConfig->getConfigNode(), &isDefault);
        if(!isDefault) {
            localConfigs.insert(pair<string, string>(prop->getName(), value));
        }
    }

    config.insert(pair<string,map<string, string> >("", localConfigs));

    /* get configurations from sources */
    list<string> sources = syncConfig->getSyncSources();
    BOOST_FOREACH(const string &name, sources) {
        localConfigs.clear();
        SyncSourceNodes sourceNodes = syncConfig->getSyncSourceNodes(name);
        ConfigPropertyRegistry &sourceRegistry = SyncSourceConfig::getRegistry();
        BOOST_FOREACH(const ConfigProperty *prop, sourceRegistry) {
            bool isDefault = false;
            string value = prop->getProperty(*sourceNodes.m_configNode, &isDefault);
            if(!isDefault) {
                localConfigs.insert(pair<string, string>(prop->getName(), value));
            }
        }
        config.insert(pair<string, map<string, string> >( "source/" + name, localConfigs));
    }
}

void ReadOperations::getReports(uint32_t start, uint32_t count,
                                Reports_t &reports)
{
    if(m_configName.empty()) {
        throw std::runtime_error("Template name must be given");
    }
    SyncContext client(m_configName, false);
    std::vector<string> dirs;
    client.getSessions(dirs);

    uint32_t index = 0;
    /** add this table to send custormized reports to client */
    static int statTable[SyncSourceReport::ITEM_LOCATION_MAX]
                        [SyncSourceReport::ITEM_STATE_MAX]
                        [SyncSourceReport::ITEM_RESULT_MAX] = 
                                  {{{1, 0, 0, 0, 0, 0, 0, 0}, 
                                   {1, 0, 0, 0, 0, 0, 0, 0}, 
                                   {1, 0, 0, 0, 0, 0, 0, 0}, 
                                   {0, 1, 0, 0, 0, 0, 1, 1}}, 
                                  {{1, 0, 0, 0, 0, 0, 0, 0}, 
                                   {1, 0, 0, 0, 0, 0, 0, 0}, 
                                   {1, 0, 0, 0, 0, 0, 0, 0}, 
                                   {0, 1, 0, 1, 1, 1, 0, 0}} };
    /** separator when joining strings as key */
    static char sep = '-';
    BOOST_FOREACH( const string &dir, dirs) {
        /** if start plus count is bigger than actual size, then return actual - size reports */
        if(index >= start && index < start + count) {
            std::map<string, string> aReport;
            SyncReport report;
            client.readSessionInfo(dir,report);
            for (SyncReport::iterator it = report.begin(); it != report.end(); ++it) {
                const string &sourceName = it->first;
                SyncSourceReport sourceReport = it->second;
                for(uint32_t i = 0; i < SyncSourceReport::ITEM_LOCATION_MAX; i++) {
                    string locStr = SyncSourceReport::LocationToString((SyncSourceReport::ItemLocation)i);
                    for(uint32_t j = 0; j < SyncSourceReport::ITEM_STATE_MAX; j++) {
                        string stateStr = SyncSourceReport::StateToString((SyncSourceReport::ItemState)j);
                        for(uint32_t k = 0; k < SyncSourceReport::ITEM_RESULT_MAX; k++) {
                            if(statTable[i][j][k] == 0) {
                                continue;
                            }
                            string resultStr = SyncSourceReport::ResultToString((SyncSourceReport::ItemResult)k);
                            int stat = sourceReport.getItemStat((SyncSourceReport::ItemLocation)i, 
                                                                (SyncSourceReport::ItemState)j, 
                                                                (SyncSourceReport::ItemResult)k);
                            /** generate key */
                            string key = "source";
                            key += sep;
                            key += sourceName;
                            key += sep;
                            key += "stat";
                            key += sep;
                            key += locStr;
                            key += sep;
                            key += stateStr;
                            key += sep;
                            key += resultStr;
                            stringstream value;
                            value << stat;
                            aReport.insert(pair<string, string>(key, value.str()));
                        }
                    }
                }
            }
            reports.push_back(aReport);
        }
        index++;
    }
}

void ReadOperations::checkSource(const std::string &sourceName)
{
    if(m_configName.empty()) {
        throw std::runtime_error("Template or server name must be given");
    }
    boost::shared_ptr<SyncConfig> config(new SyncConfig(m_configName));
    if(!config->exists()) {
        // TODO: it is an error if the config does not exist!
        config = SyncConfig::createServerTemplate(m_configName);
        if(!config.get()) {
            throw runtime_error("No server or template '" + m_configName + "' found");
        }
    }
    list<std::string> sourceNames = config->getSyncSources();
    list<std::string>::iterator it;
    for(it = sourceNames.begin(); it != sourceNames.end(); ++it) {
        if(*it == sourceName) {
            break;
        }
    }
    if(it == sourceNames.end()) {
        throw runtime_error("'" + m_configName + "' has no " + sourceName + " source");
    }
    SyncSourceParams params(sourceName, config->getSyncSourceNodes(sourceName), "");
    auto_ptr<SyncSource> syncSource(SyncSource::createSource(params, false));
    try {
        if (syncSource.get()) {
            syncSource->open();
        }
    } catch (...) {
        throw runtime_error("The source '" + sourceName + "' configuration is not correct");
    }
}
void ReadOperations::getDatabases(const string &sourceName, SourceDatabases_t &databases)
{
    if(m_configName.empty()) {
        throw std::runtime_error("Template or server name must be given");
    }
    boost::shared_ptr<SyncConfig> config(new SyncConfig(m_configName));
    if(!config->exists()) {
        // TODO: it is an error if the config does not exist!
        config = SyncConfig::createServerTemplate(m_configName);
        if(!config.get()) {
            throw std::runtime_error("No server or template '" + m_configName + "' found");
        }
    }
    SyncSourceParams params(sourceName, config->getSyncSourceNodes(sourceName), "");
    const SourceRegistry &registry(SyncSource::getSourceRegistry());
    BOOST_FOREACH(const RegisterSyncSource *sourceInfo, registry) {
        SyncSource *source = sourceInfo->m_create(params);
        if(source && (source != RegisterSyncSource::InactiveSource)) {
            auto_ptr<SyncSource> autoSource(source);
            databases = autoSource->getDatabases();
        }
    }
}

/***************** DBusSync implementation **********************/

DBusSync::DBusSync(const std::string &config,
                   Session &session) :
    SyncContext(config, true),
    m_session(session)
{
}

boost::shared_ptr<TransportAgent> DBusSync::createTransportAgent()
{
    if (m_session.useConnection()) {
        // use the D-Bus Connection to send and receive messages
        return boost::shared_ptr<TransportAgent>(new DBusTransportAgent(m_session.getServer().getLoop(),
                                                                        m_session,
                                                                        m_session.getConnection()));
    } else {
        // no connection, use HTTP via libsoup/GMainLoop
        GMainLoop *loop = m_session.getServer().getLoop();
        g_main_loop_ref(loop);
        boost::shared_ptr<HTTPTransportAgent> agent(new SoupTransportAgent(loop));
        agent->setConfig(*this);
        return agent;
    }
}

void DBusSync::displaySyncProgress(sysync::TProgressEventEnum type,
                                   int32_t extra1, int32_t extra2, int32_t extra3)
{
    SyncContext::displaySyncProgress(type, extra1, extra2, extra3);
    m_session.syncProgress(type, extra1, extra2, extra3);
}

void DBusSync::displaySourceProgress(sysync::TProgressEventEnum type,
                                     SyncSource &source,
                                     int32_t extra1, int32_t extra2, int32_t extra3)
{
    SyncContext::displaySourceProgress(type, source, extra1, extra2, extra3);
    m_session.sourceProgress(type, source, extra1, extra2, extra3);
}

/***************** Session implementation ***********************/

void Session::detach(const Caller_t &caller)
{
    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }
    client->detach(this);
}

static void setSyncFilters(const ReadOperations::Config_t &config,FilterConfigNode::ConfigFilter &syncFilter,std::map<std::string, FilterConfigNode::ConfigFilter> &sourceFilters)
{
    ReadOperations::Config_t::const_iterator it;
    for (it = config.begin(); it != config.end(); it++) {
        map<string, string>::const_iterator sit;
        if(it->first.empty()) {
            for (sit = it->second.begin(); sit != it->second.end(); sit++) {
                syncFilter.insert(*sit);
            }
        } else {
            string name = it->first;
            if(name.find("source/") == 0) {
                name = name.substr(7); ///> 7 is the length of "source/"
                FilterConfigNode::ConfigFilter &sourceFilter = sourceFilters[name];
                for (sit = it->second.begin(); sit != it->second.end(); sit++) {
                    sourceFilter.insert(*sit);
                }
            }
        }
    }
}
void Session::setConfig(bool update, bool temporary,
                        const ReadOperations::Config_t &config)
{
    if (!m_active) {
        throw std::runtime_error("session is not active, call not allowed at this time");
    }
    if (m_sync) {
        throw std::runtime_error("sync started, cannot change configuration at this time");
    }
    if(m_configName.empty()) {
        throw std::runtime_error("Template name must be given");
    }
    if (!update && temporary) {
        throw std::runtime_error("Clearing existing configuration and temporary configuration changes which only affects the duration of the session are mutually exclusive");
    }

    /** check whether we need remove the entire configuration */
    if(!update) {
        boost::shared_ptr<SyncConfig> syncConfig(new SyncConfig(m_configName));
        // TODO: this code here needs to be changed. It throws away
        // everything from the config (including meta information
        // which cannot be recreated) even if the provides properties
        // and thus wants to keep the configurtion.
        if(syncConfig.get()) {
            syncConfig->remove();
        }
        if(config.empty()) {
            return;
        }
    }
    if(temporary) {
        setSyncFilters(config, m_syncFilter, m_sourceFilters);
    } else {
        FilterConfigNode::ConfigFilter syncFilter;
        std::map<std::string, FilterConfigNode::ConfigFilter> sourceFilters;
        setSyncFilters(config, syncFilter, sourceFilters);
        /* need to save configurations */
        boost::shared_ptr<SyncConfig> from(new SyncConfig(m_configName));
        /* if it is clear mode and config does not exist, create from template */
        if(update && !from->exists()) {
            from = SyncConfig::createServerTemplate(m_configName);
            if (!from.get()) {
                from = SyncConfig::createServerTemplate(string("default"));
            }
        }
        /** generate new sources in the config map */
        for (ReadOperations::Config_t::const_iterator it = config.begin(); it != config.end(); ++it) {
            string sourceName = it->first;
            if(sourceName.find("source/") == 0) {
                sourceName = sourceName.substr(7); ///> 7 is the length of "source/"
                from->getSyncSourceNodes(sourceName);
            }
        }
        /* apply user settings */
        from->setConfigFilter(true, "", syncFilter);
        map<string, FilterConfigNode::ConfigFilter>::iterator it;
        for ( it = sourceFilters.begin(); it != sourceFilters.end(); it++ ) {
            from->setConfigFilter(false, it->first, it->second);
        }
        boost::shared_ptr<DBusSync> syncConfig(new DBusSync(m_configName, *this));
        syncConfig->copy(*from, NULL);

        syncConfig->preFlush(*syncConfig);
        syncConfig->flush();
    }
}

void Session::initServer(SharedBuffer data, const std::string &messageType)
{
    m_serverMode = true;
    m_initialMessage = data;
    m_initialMessageType = messageType;
}

void Session::sync(const std::string &mode, const SourceModes_t &source_modes)
{
    if (!m_active) {
        throw std::runtime_error("session is not active, call not allowed at this time");
    }
    if (m_sync) {
        throw std::runtime_error("sync started, cannot start again");
    }

    m_sync.reset(new DBusSync(m_configName, *this));
    if (m_serverMode) {
        m_sync->initServer(m_sessionID,
                           m_initialMessage,
                           m_initialMessageType);
    }

    // Apply temporary config filters. The parameters of this function
    // override the source filters, if set.
    m_sync->setConfigFilter(true, "", m_syncFilter);
    FilterConfigNode::ConfigFilter filter;
    filter = m_sourceFilter;
    if (!mode.empty()) {
        filter[SyncSourceConfig::m_sourcePropSync.getName()] = mode;
    }
    m_sync->setConfigFilter(false, "", filter);
    BOOST_FOREACH(const std::string &source,
                  m_sync->getSyncSources()) {
        filter = m_sourceFilters[source];
        SourceModes_t::const_iterator it = source_modes.find(source);
        if (it != source_modes.end()) {
            filter[SyncSourceConfig::m_sourcePropSync.getName()] = it->second;
        }
        m_sync->setConfigFilter(false, source, filter);
    }

    // Update status and progress. From now on, all configured sources
    // have their default entry (referencing them by name creates the
    // entry).
    BOOST_FOREACH(const std::string source,
                  m_sync->getSyncSources()) {
        m_sourceStatus[source];
        m_sourceProgress[source];
    }
    fireProgress(true);
    fireStatus(true);

    // now that we have a DBusSync object, return from the main loop
    // and once that is done, transfer control to that object
    g_main_loop_quit(loop);
}

void Session::abort()
{
    if (!m_sync) {
        throw std::runtime_error("sync not started, cannot abort at this time");
    }
    // TODO
    throw std::runtime_error("not implemented yet");
}

void Session::suspend()
{
    if (!m_sync) {
        throw std::runtime_error("sync not started, cannot abort at this time");
    }
    // TODO
    throw std::runtime_error("not implemented yet");
}

void Session::getStatus(std::string &status,
                        uint32_t &error,
                        SourceStatuses_t &sources)
{
    if (!m_active) {
        status = m_done ? "done" : "queueing";
    } else {
        status = m_abort ? "aborting" :
            m_suspend ? "suspending" :
            m_done ? "done" :
            "running";
    }
    // TODO: append ";processing" or ";waiting"

    error = m_error;
    sources = m_sourceStatus;
}

void Session::getProgress(int32_t &progress,
                          SourceProgresses_t &sources)
{
    progress = m_progress;
    sources = m_sourceProgress;
}

void Session::fireStatus(bool flush)
{
    std::string status;
    uint32_t error;
    SourceStatuses_t sources;

    /**
     * TODO: Remember when the last signal was triggered, then only
     * send it anew after a certain timeout (0.1s?).
     */

    getStatus(status, error, sources);
    emitStatus(status, error, sources);
}

void Session::fireProgress(bool flush)
{
    int32_t progress;
    SourceProgresses_t sources;

    /** TODO: timeout */

    getProgress(progress, sources);
    emitProgress(progress, sources);
}

Session::Session(DBusServer &server,
                 const std::string &config_name,
                 const std::string &session) :
    DBusObjectHelper(server.getConnection(),
                     std::string("/org/syncevolution/Session/") + session,
                     "org.syncevolution.Session"),
    ReadOperations(config_name),
    m_server(server),
    m_sessionID(session),
    m_serverMode(false),
    m_useConnection(false),
    m_active(false),
    m_done(false),
    m_abort(false),
    m_suspend(false),
    m_priority(PRI_DEFAULT),
    m_progress(-1),
    m_error(0),
    emitStatus(*this, "Status"),
    emitProgress(*this, "Progress")
{}

Session::~Session()
{
    m_server.dequeue(this);
}
    

void Session::activate()
{
    static GDBusMethodTable methods[] = {
        makeMethodEntry<Session,
                        const Caller_t &,
                        typeof(&Session::detach), &Session::detach>
                        ("Detach"),
        makeMethodEntry<Session,
                        bool,
                        ReadOperations::Config_t &,
                        typeof(&Session::getConfig), &Session::getConfig>
                        ("GetConfig"),
        makeMethodEntry<Session,
                        bool, bool,
                        const ReadOperations::Config_t &,
                        typeof(&Session::setConfig), &Session::setConfig>
                        ("SetConfig"),
        makeMethodEntry<Session,
                        uint32_t,
                        uint32_t,
                        ReadOperations::Reports_t &,
                        typeof(&Session::getReports), &Session::getReports>
                        ("GetReports"),
        makeMethodEntry<Session,
                        const std::string &,
                        const SourceModes_t &,
                        typeof(&Session::sync), &Session::sync>
                        ("Sync"),
        makeMethodEntry<Session,
                        typeof(&Session::abort), &Session::abort>
                        ("Abort"),
        makeMethodEntry<Session,
                        typeof(&Session::suspend), &Session::suspend>
                        ("Suspend"),
        makeMethodEntry<Session,
                        std::string &,
                        uint32_t &,
                        SourceStatuses_t &,
                        typeof(&Session::getStatus), &Session::getStatus>
                        ("GetStatus"),
        makeMethodEntry<Session,
                        int32_t &,
                        SourceProgresses_t &,
                        typeof(&Session::getProgress), &Session::getProgress>
                        ("GetProgress"),
        makeMethodEntry<Session,
                        const std::string &,
                        typeof(&Session::checkSource), &Session::checkSource>
                        ("CheckSource"),
        makeMethodEntry<Session,
                        const std::string &,
                        ReadOperations::SourceDatabases_t &,
                        typeof(&Session::getDatabases), &Session::getDatabases>
                        ("GetDatabases"),
       {}
    };

    static GDBusSignalTable signals[] = {
        emitStatus.makeSignalEntry("Status"),
        emitProgress.makeSignalEntry("Progress"),
        { },
    };

    DBusObjectHelper::activate(methods,
                               signals,
                               NULL,
                               this);
}

void Session::setActive(bool active)
{
    m_active = active;
    if (active) {
        boost::shared_ptr<Connection> c = m_connection.lock();
        if (c) {
            c->ready();
        }
    }
}

void Session::syncProgress(sysync::TProgressEventEnum type,
                           int32_t extra1, int32_t extra2, int32_t extra3)
{
    // TODO: update our progress and status
}

void Session::sourceProgress(sysync::TProgressEventEnum type,
                             SyncSource &source,
                             int32_t extra1, int32_t extra2, int32_t extra3)
{
    // TODO: update our progress and status
}

void Session::run()
{
    if (m_sync) {
        SyncMLStatus status;
        try {
            status = m_sync->sync();
        } catch (...) {
            status = m_sync->handleException();
        }
        if (!m_error) {
            m_error = status;
        }
        m_done = true;
        fireStatus(true);

        // if there is a connection, then it is no longer needed
        boost::shared_ptr<Connection> c = m_connection.lock();
        if (c) {
            c->shutdown();
        }
    }
}

/************************ Connection implementation *****************/

void Connection::failed(const std::string &reason)
{
    if (m_failure.empty()) {
        m_failure = reason;
        if (m_session) {
            m_session->setConnectionError(reason);
        }
    }
    if (m_state != FAILED) {
        abort();
    }
    m_state = FAILED;
}

std::string Connection::buildDescription(const StringMap &peer)
{
    StringMap::const_iterator
        desc = peer.find("description"),
        id = peer.find("id"),
        trans = peer.find("transport"),
        trans_desc = peer.find("transport_description");
    std::string buffer;
    buffer.reserve(256);
    if (desc != peer.end()) {
        buffer += desc->second;
    }
    if (id != peer.end() || trans != peer.end()) {
        if (!buffer.empty()) {
            buffer += " ";
        }
        buffer += "(";
        if (id != peer.end()) {
            buffer += id->second;
            if (trans != peer.end()) {
                buffer += " via ";
            }
        }
        if (trans != peer.end()) {
            buffer += trans->second;
            if (trans_desc != peer.end()) {
                buffer += " ";
                buffer += trans_desc->second;
            }
        }
        buffer += ")";
    }
    return buffer;
}

void Connection::process(const Caller_t &caller,
             const std::pair<size_t, const uint8_t *> &message,
             const std::string &message_type)
{
    SE_LOG_DEBUG(NULL, NULL, "D-Bus client %s sends %lu bytes, %s",
                 caller.c_str(),
                 message.first,
                 message_type.c_str());

    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }

    boost::shared_ptr<Connection> myself =
        boost::static_pointer_cast<Connection, Resource>(client->findResource(this));
    if (!myself) {
        throw runtime_error("client does not own connection");
    }

    // any kind of error from now on terminates the connection
    try {
        switch (m_state) {
        case SETUP: {
            std::string config;
            bool serverMode = false;
            // check message type, determine whether we act
            // as client or server, choose config
            if (message_type == "HTTP Config") {
                // type used for testing, payload is config name
                config.assign(reinterpret_cast<const char *>(message.second),
                              message.first);
            } else if (message_type == TransportAgent::m_contentTypeServerAlertedNotificationDS) {
                // TODO: extract server ID and match it against a server configuration.
                // At the moment, always pick "default" as configuration name.
                // sysync::SanPackage san;
                // san.PassSan(message.second, message.first);
                // san.GetHeader();
                // std::string serverID = san.fServerID;
                config = "default";

                // TODO: extract number of sources
                int numSources = 0;
                if (!numSources) {
                    // Synchronize all known sources with the selected mode.
                } else {
                    // TODO: check what the server wants us to synchronize.
                    // Create the necessary local configuration temporarily,
                    // using heuristics if necessary.
                }

                // TODO: use the session ID set by the server if non-null
            } else if (message_type == TransportAgent::m_contentTypeSyncML ||
                       message_type == TransportAgent::m_contentTypeSyncWBXML) {
                // run a new SyncML session as server
                serverMode = true;
                if (m_peer.find("config") == m_peer.end()) {
                    throw runtime_error("must choose config in Server.Connect()");
                }
                config = m_peer["config"];
            } else {
                throw runtime_error("message type not supported for starting a sync");
            }

            // run session as client (server not supported yet)
            m_state = PROCESSING;
            m_session.reset(new Session(m_server,
                                        config,
                                        m_sessionID));
            if (serverMode) {
                m_session->initServer(SharedBuffer(reinterpret_cast<const char *>(message.second),
                                                   message.first),
                                      message_type);
            }
            m_session->setPriority(Session::PRI_CONNECTION);
            m_session->setConnection(myself);
            // this will be reset only when the connection shuts down okay
            // or overwritten with the error given to us in
            // Connection::close()
            m_session->setConnectionError("closed prematurely");
            m_server.enqueue(m_session);
            break;
        }
        case PROCESSING:
            throw std::runtime_error("protocol error: already processing a message");
            break;        
        case WAITING:
            m_incomingMsg = SharedBuffer(reinterpret_cast<const char *>(message.second),
                                         message.first);
            m_incomingMsgType = message_type;
            m_state = PROCESSING;
            // get out of DBusTransportAgent::wait()
            if (m_loop) {
                g_main_loop_quit(m_loop);
                m_loop = NULL;
            }
            break;
        case FINAL:
            if (m_loop) {
                g_main_loop_quit(m_loop);
                m_loop = NULL;
            }
            throw std::runtime_error("protocol error: final reply sent, no further message processing possible");
        case DONE:
            throw std::runtime_error("protocol error: connection closed, no further message processing possible");
            break;
        case FAILED:
            throw std::runtime_error(m_failure);
            break;
        default:
            throw std::runtime_error("protocol error: unknown internal state");
            break;
        }
    } catch (const std::exception &error) {
        failed(error.what());
        throw;
    } catch (...) {
        failed("unknown exception in Connection::process");
        throw;
    }
}

void Connection::close(const Caller_t &caller,
                       bool normal,
                       const std::string &error)
{
    SE_LOG_DEBUG(NULL, NULL, "D-Bus client %s closes %s%s%s",
                 caller.c_str(),
                 normal ? "normally" : "with error",
                 error.empty() ? "" : ": ",
                 error.c_str());

    boost::shared_ptr<Client> client(m_server.findClient(caller));
    if (!client) {
        throw runtime_error("unknown client");
    }

    if (!normal ||
        m_state != FINAL) {
        std::string err = error.empty() ?
            "connection closed unexpectedly" :
            error;
        if (m_session) {
            m_session->setConnectionError(err);
        }
        failed(err);
    } else {
        m_state = DONE;
        if (m_session) {
            m_session->setConnectionError("");
        }
    }

    // remove reference to us from client, will destruct *this*
    // instance!
    client->detach(this);
}

void Connection::shutdown()
{
    // trigger removal of this connection by removing all
    // references to it
    m_server.detach(this);
}

Connection::Connection(DBusServer &server,
                       const DBusConnectionPtr &conn,
                       const std::string &sessionID,
                       const StringMap &peer,
                       bool must_authenticate) :
    DBusObjectHelper(conn.get(),
                     std::string("/org/syncevolution/Connection/") + sessionID,
                     "org.syncevolution.Connection"),
    m_server(server),
    m_peer(peer),
    m_mustAuthenticate(must_authenticate),
    m_state(SETUP),
    m_sessionID(sessionID),
    m_loop(NULL),
    abort(*this, "Abort"),
    reply(*this, "Reply"),
    m_description(buildDescription(peer))
{}

Connection::~Connection()
{
    SE_LOG_DEBUG(NULL, NULL, "done with connection to '%s'%s%s%s",
                 m_description.c_str(),
                 m_state == DONE ? ", normal shutdown" : " unexpectedly",
                 m_failure.empty() ? "" : ": ",
                 m_failure.c_str());
    try {
        if (m_state != DONE) {
            abort();
        }
        // DBusTransportAgent waiting? Wake it up.
        if (m_loop) {
            g_main_loop_quit(m_loop);
            m_loop = NULL;
        }
        m_session.use_count();
        m_session.reset();
    } catch (...) {
        // log errors, but do not propagate them because we are
        // destructing
        Exception::handle();
    }
}

void Connection::activate()
{
    static GDBusMethodTable methods[] = {
        makeMethodEntry<Connection,
                        const Caller_t &,
                        const std::pair<size_t, const uint8_t *> &,
                        const std::string &,
                        typeof(&Connection::process), &Connection::process>
                        ("Process"),
        makeMethodEntry<Connection,
                        const Caller_t &,
                        bool,
                        const std::string &,
                        typeof(&Connection::close), &Connection::close>
                        ("Close"),
        {}
    };

    static GDBusSignalTable signals[] = {
        abort.makeSignalEntry("Abort"),
        reply.makeSignalEntry("Reply"),
        { },
    };

    DBusObjectHelper::activate(methods,
                               signals,
                               NULL,
                               this);
}

void Connection::ready()
{
    // proceed with sync now that our session is ready
    m_session->sync("", Session::SourceModes_t());
}

/****************** DBusTransportAgent implementation **************/

DBusTransportAgent::DBusTransportAgent(GMainLoop *loop,
                                       Session &session,
                                       boost::weak_ptr<Connection> connection) :
    m_loop(loop),
    m_session(session),
    m_connection(connection)
{
}

DBusTransportAgent::~DBusTransportAgent()
{
    boost::shared_ptr<Connection> connection = m_connection.lock();
    if (connection) {
        connection->shutdown();
    }
}

void DBusTransportAgent::send(const char *data, size_t len)
{
    boost::shared_ptr<Connection> connection = m_connection.lock();

    if (!connection) {
        SE_THROW_EXCEPTION(TransportException,
                           "D-Bus peer has disconnected");
    }

    if (connection->m_state != Connection::PROCESSING) {
        SE_THROW_EXCEPTION(TransportException,
                           "cannot send to our D-Bus peer");
    }

    // Change state in advance. If we fail while replying, then all
    // further resends will fail with the error above.
    connection->m_state = Connection::WAITING;
    connection->m_incomingMsg = SharedBuffer();

    // TODO: turn D-Bus exceptions into transport exceptions
    StringMap meta;
    meta["URL"] = m_url;
    connection->reply(std::make_pair(len, reinterpret_cast<const uint8_t *>(data)),
                      m_type, meta, false, connection->m_sessionID);
}

void DBusTransportAgent::shutdown()
{
    boost::shared_ptr<Connection> connection = m_connection.lock();

    if (!connection) {
        SE_THROW_EXCEPTION(TransportException,
                           "D-Bus peer has disconnected");
    }

    // send final, empty message and wait for close
    connection->m_state = Connection::FINAL;
    connection->reply(std::pair<size_t, const uint8_t *>(0, 0),
                      "", StringMap(),
                      true, connection->m_sessionID);
}

void DBusTransportAgent::doWait(boost::shared_ptr<Connection> &connection)
{
    // let Connection wake us up when it has a reply or
    // when it closes down
    connection->m_loop = m_loop;

    // release our reference so that the Connection instance can
    // be destructed when requested by the D-Bus peer
    connection.reset();

    // TODO: setup regular callback

    // now wait
    g_main_loop_run(m_loop);
}

DBusTransportAgent::Status DBusTransportAgent::wait(bool noReply)
{
    boost::shared_ptr<Connection> connection = m_connection.lock();

    if (!connection) {
        SE_THROW_EXCEPTION(TransportException,
                           "D-Bus peer has disconnected");
    }

    switch (connection->m_state) {
    case Connection::PROCESSING:
        m_incomingMsg = connection->m_incomingMsg;
        m_incomingMsgType = connection->m_incomingMsgType;
        return GOT_REPLY;
        break;
    case Connection::FINAL:
        doWait(connection);

        // if the connection is still available, then keep waiting
        connection = m_connection.lock();
        if (connection) {
            return ACTIVE;
        } else if (m_session.getConnectionError().empty()) {
            return INACTIVE;
        } else {
            SE_THROW_EXCEPTION(TransportException, m_session.getConnectionError());
            return FAILED;
        }
        break;
    case Connection::WAITING:
        if (noReply) {
            // message is sent as far as we know, so return
            return INACTIVE;
        }

        doWait(connection);

        // tell caller to check again
        return ACTIVE;
        break;
    case Connection::DONE:
        if (!noReply) {
            SE_THROW_EXCEPTION(TransportException,
                               "internal error: transport has shut down, can no longer receive reply");
        }
        
        return CLOSED;
    default:
        SE_THROW_EXCEPTION(TransportException,
                           "internal error: send() on connection which is not ready");
        break;
    }

    return FAILED;
}

void DBusTransportAgent::getReply(const char *&data, size_t &len, std::string &contentType)
{
    data = m_incomingMsg.get();
    len = m_incomingMsg.size();
    contentType = m_incomingMsgType;
}


/********************** DBusServer implementation ******************/

void DBusServer::clientGone(Client *c)
{
    for(Clients_t::iterator it = m_clients.begin();
        it != m_clients.end();
        ++it) {
        if (it->second.get() == c) {
            SE_LOG_DEBUG(NULL, NULL, "D-Bus client %s has disconnected",
                         c->m_ID.c_str());
            m_clients.erase(it);
            return;
        }
    }
    SE_LOG_DEBUG(NULL, NULL, "unknown client has disconnected?!");
}

std::string DBusServer::getNextSession()
{
    // TODO: make the session ID more random. This protects to
    // some extend against injecting unwanted messages into the
    // communication.
    m_lastSession++;
    if (!m_lastSession) {
        m_lastSession++;
    }
    return StringPrintf("%u", m_lastSession);
}

void DBusServer::attachClient(const Caller_t &caller,
                              const boost::shared_ptr<Watch> &watch)
{
    // TODO: implement idle detection and automatic shutdown of the server
}

void DBusServer::detachClient(const Caller_t &caller)
{
}

void DBusServer::connect(const Caller_t &caller,
                         const boost::shared_ptr<Watch> &watch,
                         const StringMap &peer,
                         bool must_authenticate,
                         const std::string &session,
                         DBusObject_t &object)
{
    if (!session.empty()) {
        // reconnecting to old connection is not implemented yet
        throw std::runtime_error("not implemented");
    }
    std::string new_session = getNextSession();

    boost::shared_ptr<Connection> c(new Connection(*this,
                                                   getConnection(),
                                                   new_session,
                                                   peer,
                                                   must_authenticate));
    SE_LOG_DEBUG(NULL, NULL, "connecting D-Bus client %s with '%s'",
                 caller.c_str(),
                 c->m_description.c_str());
        
    boost::shared_ptr<Client> client = addClient(getConnection(),
                                                 caller,
                                                 watch);
    client->attach(c);
    c->activate();

    object = c->getPath();
}

void DBusServer::startSession(const Caller_t &caller,
                              const boost::shared_ptr<Watch> &watch,
                              const std::string &server,
                              DBusObject_t &object)
{
    boost::shared_ptr<Client> client = addClient(getConnection(),
                                                 caller,
                                                 watch);
    std::string new_session = getNextSession();   
    boost::shared_ptr<Session> session(new Session(*this,
                                                   server,
                                                   new_session));
    // TODO: how do we decide whether this is a client or server session?
    client->attach(session);
    session->activate();
    enqueue(session);
    object = session->getPath();
}

void DBusServer::checkPresence(const std::string &server,
                               std::string &status,
                               std::vector<std::string> &transports)
{
    // TODO: implement this, right now always return status = "" = available
}

DBusServer::DBusServer(GMainLoop *loop, const DBusConnectionPtr &conn) :
    DBusObjectHelper(conn.get(), "/org/syncevolution/Server", "org.syncevolution.Server"),
    m_loop(loop),
    m_lastSession(time(NULL)),
    m_activeSession(NULL),
    sessionChanged(*this, "SessionChanged"),
    presence(*this, "Presence")
{}

DBusServer::~DBusServer()
{
    // make sure all other objects are gone before destructing ourselves
    m_syncSession.reset();
    m_workQueue.clear();
    m_clients.clear();
}

void DBusServer::activate()
{
    static GDBusMethodTable methods[] = {
        makeMethodEntry<DBusServer,
                        const Caller_t &,
                        const boost::shared_ptr<Watch> &,
                        typeof(&DBusServer::attachClient), &DBusServer::attachClient>
                        ("Attach"),
        makeMethodEntry<DBusServer,
                        const Caller_t &,
                        typeof(&DBusServer::detachClient), &DBusServer::detachClient>
                        ("Detach"),
        makeMethodEntry<DBusServer,
                        const Caller_t &,
                        const boost::shared_ptr<Watch> &,
                        const StringMap &,
                        bool,
                        const std::string &,
                        DBusObject_t &,
                        typeof(&DBusServer::connect), &DBusServer::connect
                        >("Connect"),
        makeMethodEntry<DBusServer,
                        const Caller_t &,
                        const boost::shared_ptr<Watch> &,
                        const std::string &,
                        DBusObject_t &,
                        typeof(&DBusServer::startSession), &DBusServer::startSession
                        >("StartSession"),
        makeMethodEntry<DBusServer,
                        const std::string &,
                        bool,
                        ReadOperations::Config_t &,
                        typeof(&DBusServer::getConfig), &DBusServer::getConfig>
                        ("GetConfig"),
        makeMethodEntry<DBusServer,
                        const std::string &,
                        uint32_t,
                        uint32_t,
                        ReadOperations::Reports_t &,
                        typeof(&DBusServer::getReports), &DBusServer::getReports>
                        ("GetReports"),
        makeMethodEntry<DBusServer,
                        const std::string &,
                        std::string &,
                        std::vector<std::string> &,
                        typeof(&DBusServer::checkPresence), &DBusServer::checkPresence>
                        ("CheckPresence"),
        {}
    };

    static GDBusSignalTable signals[] = {
        sessionChanged.makeSignalEntry("SessionChanged"),
        presence.makeSignalEntry("Presence"),
        { },
    };

    DBusObjectHelper::activate(methods,
                               signals,
                               NULL,
                               this);
}

void DBusServer::run()
{
    while (true) {
        if (!m_activeSession ||
            !m_activeSession->readyToRun()) {
            g_main_loop_run(m_loop);
        }
        if (m_activeSession &&
            m_activeSession->readyToRun()) {
            // this session must be owned by someone, otherwise
            // it would not be set as active session
            boost::shared_ptr<Session> session = m_activeSessionRef.lock();
            if (!session) {
                throw runtime_error("internal error: session no longer available");
            }
            try {
                // ensure that the session doesn't go away
                m_syncSession.swap(session);
                m_activeSession->run();
            } catch (const std::exception &ex) {
                SE_LOG_ERROR(NULL, NULL, "%s", ex.what());
            } catch (...) {
                SE_LOG_ERROR(NULL, NULL, "unknown error");
            }
            session.swap(m_syncSession);
            dequeue(session.get());
        } else {
            // the only reasons to get out of the main loop are
            // running a sync and quitting; no active session, so quit
            break;
        }
    }
}


/**
 * look up client by its ID
 */
boost::shared_ptr<Client> DBusServer::findClient(const Caller_t &ID)
{
    for(Clients_t::iterator it = m_clients.begin();
        it != m_clients.end();
        ++it) {
        if (it->second->m_ID == ID) {
            return it->second;
        }
    }
    return boost::shared_ptr<Client>();
}

boost::shared_ptr<Client> DBusServer::addClient(const DBusConnectionPtr &conn,
                                                const Caller_t &ID,
                                                const boost::shared_ptr<Watch> &watch)
{
    boost::shared_ptr<Client> client(findClient(ID));
    if (client) {
        return client;
    }
    client.reset(new Client(ID));
    // add to our list *before* checking that peer exists, so
    // that clientGone() can remove it if the check fails
    m_clients.push_back(std::make_pair(watch, client));
    watch->setCallback(boost::bind(&DBusServer::clientGone, this, client.get()));
    return client;
}


void DBusServer::detach(Resource *resource)
{
    BOOST_FOREACH(const Clients_t::value_type &client_entry,
                  m_clients) {
        client_entry.second->detachAll(resource);
    }
}

void DBusServer::enqueue(const boost::shared_ptr<Session> &session)
{
    WorkQueue_t::iterator it = m_workQueue.end();
    while (it != m_workQueue.begin()) {
        --it;
        if (it->lock()->getPriority() <= session->getPriority()) {
            ++it;
            break;
        }
    }
    m_workQueue.insert(it, session);

    checkQueue();
}

void DBusServer::dequeue(Session *session)
{
    if (m_syncSession.get() == session) {
        // This is the running sync session.
        // It's not in the work queue and we have to
        // keep it active, so nothing to do.
        return;
    }

    for (WorkQueue_t::iterator it = m_workQueue.begin();
         it != m_workQueue.end();
         ++it) {
        if (it->lock().get() == session) {
            // remove from queue
            m_workQueue.erase(it);
            // session was idle, so nothing else to do
            return;
        }
    }

    if (m_activeSession == session) {
        // The session is releasing the lock, so someone else might
        // run now.
        session->setActive(false);
        sessionChanged(session->getPath(), false);
        m_activeSession = NULL;
        m_activeSessionRef.reset();
        checkQueue();
        return;
    }
}

void DBusServer::checkQueue()
{
    if (m_activeSession) {
        // still busy
        return;
    }

    while (!m_workQueue.empty()) {
        boost::shared_ptr<Session> session = m_workQueue.front().lock();
        m_workQueue.pop_front();
        if (session) {
            // activate the session
            m_activeSession = session.get();
            m_activeSessionRef = session;
            session->setActive(true);
            sessionChanged(session->getPath(), true);
            return;
        }
    }
}

/**************************** main *************************/

void niam(int sig)
{
    g_main_loop_quit (loop);
}

int main()
{
    try {
        g_type_init();
        g_thread_init(NULL);
        g_set_application_name("SyncEvolution");
        loop = g_main_loop_new (NULL, FALSE);

        signal(SIGTERM, niam);
        signal(SIGINT, niam);

        LoggerBase::instance().setLevel(LoggerBase::DEBUG);

        DBusErrorCXX err;
        DBusConnectionPtr conn = g_dbus_setup_bus(DBUS_BUS_SESSION,
                                                  "org.syncevolution",
                                                  &err);
        if (!conn) {
            err.throwFailure("g_dbus_setup_bus()");
        }

        DBusServer server(loop, conn);
        server.activate();
        server.run();
	return 0;
    } catch ( const std::exception &ex ) {
        SE_LOG_ERROR(NULL, NULL, "%s", ex.what());
    } catch (...) {
        SE_LOG_ERROR(NULL, NULL, "unknown error");
    }

    return 1;
}
