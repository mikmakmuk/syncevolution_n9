/*
 * Copyright (C) 2005-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1
#endif
#include <dlfcn.h>
#include <sys/types.h>
#include <dirent.h>

#include "SyncSource.h"
#include "EvolutionSyncClient.h"
#include "SyncEvolutionUtil.h"

#include "SynthesisEngine.h"
#include <synthesis/SDK_util.h>
#include <synthesis/sync_dbapidef.h>

#include <boost/bind.hpp>
#include <boost/algorithm/string/join.hpp>

#include <ctype.h>
#include <errno.h>

#include <fstream>
#include <iostream>

void SyncSourceBase::throwError(const string &action, int error)
{
    throwError(action + ": " + strerror(error));
}

void SyncSourceBase::throwError(const string &failure)
{
    EvolutionSyncClient::throwError(string(getName()) + ": " + failure);
}

SyncMLStatus SyncSourceBase::handleException()
{
    SyncMLStatus res = SyncEvolutionException::handle(this);
    return res == STATUS_FATAL ?
        STATUS_DATASTORE_FAILURE :
        res;
}

void SyncSourceBase::messagev(Level level,
                              const char *prefix,
                              const char *file,
                              int line,
                              const char *function,
                              const char *format,
                              va_list args)
{
    string newprefix = getName();
    if (prefix) {
        newprefix += ": ";
        newprefix += prefix;
    }
    LoggerBase::instance().messagev(level, newprefix.c_str(),
                                    file, line, function,
                                    format, args);
}

void SyncSourceBase::getDatastoreXML(string &xml, XMLConfigFragments &fragments)
{
    stringstream xmlstream;
    SynthesisInfo info;

    getSynthesisInfo(info, fragments);

    xmlstream <<
        "      <plugin_module>SyncEvolution</plugin_module>\n"
        "      <plugin_datastoreadmin>no</plugin_datastoreadmin>\n"
        "\n"
        "      <!-- General datastore settings for all DB types -->\n"
        "\n"
        "      <!-- if this is set to 'yes', SyncML clients can only read\n"
        "           from the database, but make no modifications -->\n"
        "      <readonly>no</readonly>\n"
        "\n"
        "      <!-- conflict strategy: Newer item wins\n"
        "           You can set 'server-wins' or 'client-wins' as well\n"
        "           if you want to give one side precedence\n"
        "      -->\n"
        "      <conflictstrategy>newer-wins</conflictstrategy>\n"
        "\n"
        "      <!-- on slowsync: duplicate items that are not fully equal\n"
        "           You can set this to 'newer-wins' as well to avoid\n"
        "           duplicates as much as possible\n"
        "      -->\n"
        "      <slowsyncstrategy>duplicate</slowsyncstrategy>\n"
        "\n"
        "      <!-- text db plugin is designed for UTF-8, make sure data is passed as UTF-8 (and not the ISO-8859-1 default) -->\n"
        "      <datacharset>UTF-8</datacharset>\n"
        "      <!-- use C-language (unix style) linefeeds (\n, 0x0A) -->\n"
        "      <datalineends>unix</datalineends>\n"
        "\n"
        "      <!-- set this to 'UTC' if time values should be stored in UTC into the database\n"
        "           rather than local time. 'SYSTEM' denotes local server time zone. -->\n"
        "      <datatimezone>SYSTEM</datatimezone>\n"
        "\n"
        "      <!-- plugin DB may have its own identifiers to determine the point in time of changes, so\n"
        "           we must make sure this identifier is stored (and not only the sync time) -->\n"
        "      <storesyncidentifiers>yes</storesyncidentifiers>\n"
        "\n";
    
    xmlstream <<
        "      <!-- Mapping of the fields to the fieldlist -->\n"
        "      <fieldmap fieldlist='" << info.m_fieldlist << "'>\n";
    if (!info.m_profile.empty()) {
        xmlstream <<
            "        <initscript><![CDATA[\n"
            "           string itemdata;\n"
            "        ]]></initscript>\n"
            "        <beforewritescript><![CDATA[\n";
        if(!info.m_incomingScript.empty()) {
            xmlstream << 
                "           " << info.m_incomingScript << "\n";
        }
        xmlstream <<
            "           itemdata = MAKETEXTWITHPROFILE(" << info.m_profile << ", \"EVOLUTION\");\n"
            "        ]]></beforewritescript>\n"
            "        <afterreadscript><![CDATA[\n"
            "           PARSETEXTWITHPROFILE(itemdata, " << info.m_profile << ", \"EVOLUTION\");\n";
        if(!info.m_outgoingScript.empty()) {
            xmlstream << 
                "           " << info.m_outgoingScript<< "\n";
        }
        xmlstream <<
            "        ]]></afterreadscript>\n"
            "        <map name='data' references='itemdata' type='string'/>\n";
    }
    xmlstream << 
        "        <automap/>\n"
        "      </fieldmap>\n"
        "\n";

    xmlstream <<
        "      <!-- datatypes supported by this datastore -->\n"
        "      <typesupport>\n" <<
        info.m_datatypes <<
        "      </typesupport>\n";

    xml = xmlstream.str();
}

string SyncSourceBase::getNativeDatatypeName()
{
    SynthesisInfo info;
    XMLConfigFragments fragments;
    getSynthesisInfo(info, fragments);
    return info.m_native;
}

SDKInterface *SyncSource::getSynthesisAPI() const
{
    return m_synthesisAPI.empty() ?
        NULL :
        static_cast<SDKInterface *>(m_synthesisAPI[m_synthesisAPI.size() - 1]);
}

void SyncSource::pushSynthesisAPI(sysync::SDK_InterfaceType *synthesisAPI)
{
    m_synthesisAPI.push_back(synthesisAPI);
}

void SyncSource::popSynthesisAPI() {
    m_synthesisAPI.pop_back();
}

SourceRegistry &SyncSource::getSourceRegistry()
{
    static SourceRegistry sourceRegistry;
    return sourceRegistry;
}

RegisterSyncSource::RegisterSyncSource(const string &shortDescr,
                                       bool enabled,
                                       Create_t create,
                                       const string &typeDescr,
                                       const Values &typeValues) :
    m_shortDescr(shortDescr),
    m_enabled(enabled),
    m_create(create),
    m_typeDescr(typeDescr),
    m_typeValues(typeValues)
{
    SourceRegistry &registry(SyncSource::getSourceRegistry());

    // insert sorted by description to have deterministic ordering
    for(SourceRegistry::iterator it = registry.begin();
        it != registry.end();
        ++it) {
        if ((*it)->m_shortDescr > shortDescr) {
            registry.insert(it, this);
            return;
        }
    }
    registry.push_back(this);
}

SyncSource *const RegisterSyncSource::InactiveSource = (SyncSource *)1;

TestRegistry &SyncSource::getTestRegistry()
{
    static TestRegistry testRegistry;
    return testRegistry;
}

RegisterSyncSourceTest::RegisterSyncSourceTest(const string &configName, const string &testCaseName) :
    m_configName(configName),
    m_testCaseName(testCaseName)
{
    SyncSource::getTestRegistry().push_back(this);
}

static class ScannedModules {
public:
    ScannedModules() {
#ifdef ENABLE_MODULES
        DIR *dir = opendir (SYNCEVO_BACKEND);
        if(!dir) {
            info<<"Backend directory open failed"<<endl;
            return;
        }
        struct dirent *entry;
        list<pair <string,DIR *> > dirs;
        DIR *subdir = NULL;
        string dirpath (SYNCEVO_BACKEND);
        // possible extension: scan directories for matching module names instead of hard-coding known names

        do {
            info<<"Scanning backend libraries in " <<dirpath <<endl;
            while ((entry = readdir(dir))!=NULL){
                void *dlhandle;
                if (!strcmp (entry->d_name, ".") || !strcmp(entry->d_name, "..")){
                    continue;
                } else if (entry->d_type == DT_DIR) {
                    /* This is a 2-level dir, this corresponds to loading
                     * backends from current building directory. The library
                     * should reside in .libs sub directory.*/
                    string path = dirpath + entry->d_name +"/.libs/";
                    subdir = opendir (path.c_str());
                    if (subdir) {
                        dirs.push_back (make_pair(path, subdir));
                    }
                    continue;
                }
                char *p = strrchr(entry->d_name, 's');
                if (p && *(p+1)=='o' && *(p+2)=='\0'){

                    // Open the shared object so that backend can register
                    // itself. We keep that pointer, so never close the
                    // module!
                    string fullpath = dirpath + entry->d_name;
                    dlhandle = dlopen(fullpath.c_str(), RTLD_NOW|RTLD_GLOBAL);
                    // remember which modules were found and which were not
                    if (dlhandle) {
                        debug<<"Loading backend library "<<entry->d_name<<endl;
                        m_available.push_back(entry->d_name);
                    } else {
                        debug<<"Loading backend library "<<entry->d_name<<"failed "<< dlerror()<<endl;
                    }
                }
            }
            closedir (dir);
            if (!dirs.empty()){
                dirpath = dirs.front().first;
                dir = dirs.front().second;
                dirs.pop_front();
            } else {
                break;
            }
        } while (true);
#endif
    }
    list<string> m_available;
    std::ostringstream debug, info;
} scannedModules;

const char* SyncSourceBackendsInfo() {
    return scannedModules.info.str().c_str();
}
const char* SyncSourceBackendsDebug() {
    return scannedModules.debug.str().c_str();
}

SyncSource *SyncSource::createSource(const SyncSourceParams &params, bool error)
{
    string sourceTypeString = getSourceTypeString(params.m_nodes);
    SourceType sourceType = getSourceType(params.m_nodes);

    const SourceRegistry &registry(getSourceRegistry());
    BOOST_FOREACH(const RegisterSyncSource *sourceInfos, registry) {
        SyncSource *source = sourceInfos->m_create(params);
        if (source) {
            if (source == RegisterSyncSource::InactiveSource) {
                EvolutionSyncClient::throwError(params.m_name + ": access to " + sourceInfos->m_shortDescr +
                                                " not enabled, therefore type = " + sourceTypeString + " not supported");
            }
            return source;
        }
    }

    if (error) {
        string problem = params.m_name + ": type '" + sourceTypeString + "' not supported";
        if (scannedModules.m_available.size()) {
            problem += " by any of the backends (";
            problem += boost::join(scannedModules.m_available, ", ");
            problem += ")";
        }
        EvolutionSyncClient::throwError(problem);
    }

    return NULL;
}

SyncSource *SyncSource::createTestingSource(const string &name, const string &type, bool error,
                                            const char *prefix)
{
    EvolutionSyncConfig config("testing");
    SyncSourceNodes nodes = config.getSyncSourceNodes(name);
    SyncSourceParams params(name, nodes, "");
    PersistentSyncSourceConfig sourceconfig(name, nodes);
    sourceconfig.setSourceType(type);
    if (prefix) {
        sourceconfig.setDatabaseID(string(prefix) + name + "_1");
    }
    return createSource(params, error);
}

void SyncSourceSession::init(SyncSource::Operations &ops)
{
    ops.m_startDataRead = boost::bind(&SyncSourceSession::startDataRead, this, _1, _2);
    ops.m_endDataWrite = boost::bind(&SyncSourceSession::endDataWrite, this, _1, _2);
}

sysync::TSyError SyncSourceSession::startDataRead(const char *lastToken, const char *resumeToken)
{
    beginSync(lastToken ? lastToken : "",
              resumeToken ? resumeToken : "");
    return sysync::LOCERR_OK;
}

sysync::TSyError SyncSourceSession::endDataWrite(bool success, char **newToken)
{
    std::string token = endSync(success);
    *newToken = StrAlloc(token.c_str());
    return sysync::LOCERR_OK;
}

void SyncSourceChanges::init(SyncSource::Operations &ops)
{
    ops.m_readNextItem = boost::bind(&SyncSourceChanges::iterate, this, _1, _2, _3);
}

SyncSourceChanges::SyncSourceChanges() :
    m_first(true)
{
}

bool SyncSourceChanges::addItem(const string &luid, State state)
{
    pair<Items_t::iterator, bool> res = m_items[state].insert(luid);
    return res.second;
}

sysync::TSyError SyncSourceChanges::iterate(sysync::ItemID aID,
                                            sysync::sInt32 *aStatus,
                                            bool aFirst)
{
    if (m_first || aFirst) {
        m_it = m_items[ANY].begin();
        m_first = false;
    }

    if (m_it == m_items[ANY].end()) {
        *aStatus = sysync::ReadNextItem_EOF;
    } else {
        const string &luid = *m_it;

        if (m_items[NEW].find(luid) != m_items[NEW].end() ||
            m_items[UPDATED].find(luid) != m_items[UPDATED].end()) {
            *aStatus = sysync::ReadNextItem_Changed;
        } else {
            *aStatus = sysync::ReadNextItem_Unchanged;
        }
        aID->item = StrAlloc(luid.c_str());
        ++m_it;
    }

    return sysync::LOCERR_OK;
}

void SyncSourceDelete::init(SyncSource::Operations &ops)
{
    ops.m_deleteItem = boost::bind(&SyncSourceDelete::deleteItemSynthesis, this, _1);
}

sysync::TSyError SyncSourceDelete::deleteItemSynthesis(sysync::cItemID aID)
{
    deleteItem(aID->item);
    incrementNumDeleted();
    return sysync::LOCERR_OK;
}


void SyncSourceSerialize::getSynthesisInfo(SynthesisInfo &info,
                                           XMLConfigFragments &fragments)
{
    string type = getMimeType();

    if (type == "text/x-vcard") {
        info.m_native = "vCard21";
        info.m_fieldlist = "contacts";
        info.m_profile = "\"vCard\", 1";
        info.m_datatypes =
            "        <use datatype='vCard21' mode='rw' preferred='yes'/>\n"
            "        <use datatype='vCard30' mode='rw'/>\n";
    } else if (type == "text/vcard") {
        info.m_native = "vCard30";
        info.m_fieldlist = "contacts";
        info.m_profile = "\"vCard\", 2";
        info.m_datatypes =
            "        <use datatype='vCard21' mode='rw'/>\n"
            "        <use datatype='vCard30' mode='rw' preferred='yes'/>\n";
    } else if (type == "text/x-calendar") {
        info.m_native = "vCalendar10";
        info.m_fieldlist = "calendar";
        info.m_profile = "\"vCalendar\", 1";
        info.m_datatypes =
            "        <use datatype='vCalendar10' mode='rw' preferred='yes'/>\n"
            "        <use datatype='iCalendar20' mode='rw'/>\n";
    } else if (type == "text/calendar") {
        info.m_native = "iCalendar20";
        info.m_fieldlist = "calendar";
        info.m_profile = "\"vCalendar\", 2";
        info.m_datatypes =
            "        <use datatype='vCalendar10' mode='rw'/>\n"
            "        <use datatype='iCalendar20' mode='rw' preferred='yes'/>\n";
    } else if (type == "text/plain") {
        info.m_fieldlist = "Note";
        info.m_profile = "\"Note\", 2";
    } else {
        throwError(string("default MIME type not supported: ") + type);
    }

    SourceType sourceType = getSourceType();
    if (!sourceType.m_format.empty()) {
        type = sourceType.m_format;
    }

    if (type == "text/x-vcard:2.1" || type == "text/x-vcard") {
        info.m_datatypes =
            "        <use datatype='vCard21' mode='rw' preferred='yes'/>\n";
        if (!sourceType.m_forceFormat) {
            info.m_datatypes +=
                "        <use datatype='vCard30' mode='rw'/>\n";
        }
    } else if (type == "text/vcard:3.0" || type == "text/vcard") {
        info.m_datatypes =
            "        <use datatype='vCard30' mode='rw' preferred='yes'/>\n";
        if (!sourceType.m_forceFormat) {
            info.m_datatypes +=
                "        <use datatype='vCard21' mode='rw'/>\n";
        }
    } else if (type == "text/x-vcalendar:2.0" || type == "text/x-vcalendar") {
        info.m_datatypes =
            "        <use datatype='vcalendar10' mode='rw' preferred='yes'/>\n";
        if (!sourceType.m_forceFormat) {
            info.m_datatypes +=
                "        <use datatype='icalendar20' mode='rw'/>\n";
        }
    } else if (type == "text/calendar:2.0" || type == "text/calendar") {
        info.m_datatypes =
            "        <use datatype='icalendar20' mode='rw' preferred='yes'/>\n";
        if (!sourceType.m_forceFormat) {
            info.m_datatypes +=
                "        <use datatype='vcalendar10' mode='rw'/>\n";
        }
    } else if (type == "text/plain:1.0" || type == "text/plain") {
        // note10 are the same as note11, so ignore force format
        info.m_datatypes =
            "        <use datatype='note10' mode='rw' preferred='yes'/>\n"
            "        <use datatype='note11' mode='rw'/>\n";
    } else {
        throwError(string("configured MIME type not supported: ") + type);
    }
}

sysync::TSyError SyncSourceSerialize::readItemAsKey(sysync::cItemID aID, sysync::KeyH aItemKey)
{
    std::string item;

    readItem(aID->item, item);
    TSyError res = getSynthesisAPI()->setValue(aItemKey, "data", item.c_str(), item.size());
    return res;
}

sysync::TSyError SyncSourceSerialize::insertItemAsKey(sysync::KeyH aItemKey, sysync::cItemID aID, sysync::ItemID newID)
{
    SharedBuffer data;
    TSyError res = getSynthesisAPI()->getValue(aItemKey, "data", data);

    if (!res) {
        InsertItemResult inserted =
            insertItem(!aID ? "" : aID->item, data.get());
        newID->item = StrAlloc(inserted.m_luid.c_str());
    }

    return res;
}

void SyncSourceSerialize::init(SyncSource::Operations &ops)
{
    ops.m_readItemAsKey = boost::bind(&SyncSourceSerialize::readItemAsKey,
                                      this, _1, _2);
    ops.m_insertItemAsKey = boost::bind(&SyncSourceSerialize::insertItemAsKey,
                                        this, _1, (sysync::cItemID)NULL, _2);
    ops.m_updateItemAsKey = boost::bind(&SyncSourceSerialize::insertItemAsKey,
                                        this, _1, _2, _3);
}


void SyncSourceRevisions::backupData(const string &dir, ConfigNode &node, BackupReport &report)
{
    RevisionMap_t revisions;
    listAllItems(revisions);

    unsigned long counter = 1;
    string item;
    errno = 0;
    BOOST_FOREACH(const StringPair &mapping, revisions) {
        const string &uid = mapping.first;
        const string &rev = mapping.second;
        m_raw->readItemRaw(uid, item);

        stringstream filename;
        filename << dir << "/" << counter;

        ofstream out(filename.str().c_str());
        out.write(item.c_str(), item.size());
        out.close();
        if (out.fail()) {
            throwError(string("error writing ") + filename.str() + ": " + strerror(errno));
        }

        stringstream key;
        key << counter << "-uid";
        node.setProperty(key.str(), uid);
        key.clear();
        key << counter << "-rev";
        node.setProperty(key.str(), rev);

        counter++;
    }

    stringstream value;
    value << counter - 1;
    node.setProperty("numitems", value.str());
    node.flush();

    report.setNumItems(counter - 1);
}

void SyncSourceRevisions::restoreData(const string &dir, const ConfigNode &node, bool dryrun, SyncSourceReport &report)
{
    RevisionMap_t revisions;
    listAllItems(revisions);

    long numitems;
    string strval;
    strval = node.readProperty("numitems");
    stringstream stream(strval);
    stream >> numitems;

    for (long counter = 1; counter <= numitems; counter++) {
        stringstream key;
        key << counter << "-uid";
        string uid = node.readProperty(key.str());
        key.clear();
        key << counter << "-rev";
        string rev = node.readProperty(key.str());
        RevisionMap_t::iterator it = revisions.find(uid);
        report.incrementItemStat(report.ITEM_LOCAL,
                                 report.ITEM_ANY,
                                 report.ITEM_TOTAL);
        if (it != revisions.end() &&
            it->second == rev) {
            // item exists in backup and database with same revision:
            // nothing to do
        } else {
            // add or update, so need item
            stringstream filename;
            filename << dir << "/" << counter;
            string data;
            if (!ReadFile(filename.str(), data)) {
                throwError(StringPrintf("restoring %s from %s failed: could not read file",
                                        uid.c_str(),
                                        filename.str().c_str()));
            }
            // TODO: it would be nicer to recreate the item
            // with the original revision. If multiple peers
            // synchronize against us, then some of them
            // might still be in sync with that revision. By
            // updating the revision here we force them to
            // needlessly receive an update.
            //
            // For the current peer for which we restore this is
            // avoided by the revision check above: unchanged
            // items aren't touched.
            SyncSourceReport::ItemState state =
                it == revisions.end() ?
                SyncSourceReport::ITEM_ADDED :   // not found in database, create anew
                SyncSourceReport::ITEM_UPDATED;  // found, update existing item
            try {
                report.incrementItemStat(report.ITEM_LOCAL,
                                         state,
                                         report.ITEM_TOTAL);
                if (!dryrun) {
                    m_raw->insertItemRaw(it == revisions.end() ? "" : uid,
                                         data);
                }
            } catch (...) {
                report.incrementItemStat(report.ITEM_LOCAL,
                                         state,
                                         report.ITEM_REJECT);
                throw;
            }
        }

        // remove handled item from revision list so
        // that when we are done, the only remaining
        // items listed there are the ones which did
        // no exist in the backup
        if (it != revisions.end()) {
            revisions.erase(it);
        }
    }

    // now remove items that were not in the backup
    BOOST_FOREACH(const StringPair &mapping, revisions) {
        try {
            report.incrementItemStat(report.ITEM_LOCAL,
                                     report.ITEM_REMOVED,
                                     report.ITEM_TOTAL);
            if (!dryrun) {
                m_del->deleteItem(mapping.first);
            }
        } catch(...) {
            report.incrementItemStat(report.ITEM_LOCAL,
                                     report.ITEM_REMOVED,
                                     report.ITEM_REJECT);
            throw;
        }
    }
}

void SyncSourceRevisions::detectChanges(ConfigNode &trackingNode)
{
    RevisionMap_t revisions;
    listAllItems(revisions);

    BOOST_FOREACH(const StringPair &mapping, revisions) {
        const string &uid = mapping.first;
        const string &revision = mapping.second;

        // always remember the item, need full list
        addItem(uid);

        string serverRevision(trackingNode.readProperty(uid));
        if (!serverRevision.size()) {
            addItem(uid, NEW);
            trackingNode.setProperty(uid, revision);
        } else {
            if (revision != serverRevision) {
                addItem(uid, UPDATED);
                trackingNode.setProperty(uid, revision);
            }
        }
    }

    // clear information about all items that we recognized as deleted
    map<string, string> props;
    trackingNode.readProperties(props);

    BOOST_FOREACH(const StringPair &mapping, props) {
        const string &uid(mapping.first);
        if (getAllItems().find(uid) == getAllItems().end()) {
            addItem(uid, DELETED);
            trackingNode.removeProperty(uid);
        }
    }
}

void SyncSourceRevisions::updateRevision(ConfigNode &trackingNode,
                                         const std::string &old_luid,
                                         const std::string &new_luid,
                                         const std::string &revision)
{
    databaseModified();
    if (old_luid != new_luid) {
        trackingNode.removeProperty(old_luid);
    }
    if (new_luid.empty() || revision.empty()) {
        throwError("need non-empty LUID and revision string");
    }
    trackingNode.setProperty(new_luid, revision);
}

void SyncSourceRevisions::deleteRevision(ConfigNode &trackingNode,
                                         const std::string &luid)
{
    databaseModified();
    trackingNode.removeProperty(luid);
}

void SyncSourceRevisions::sleepSinceModification()
{
    time_t current = time(NULL);
    while (current - m_modTimeStamp < m_revisionAccuracySeconds) {
        sleep(m_revisionAccuracySeconds - (current - m_modTimeStamp));
        current = time(NULL);
    }
}

void SyncSourceRevisions::databaseModified()
{
    m_modTimeStamp = time(NULL);
}

void SyncSourceRevisions::init(SyncSourceRaw *raw,
                               SyncSourceDelete *del,
                               int granularity,
                               SyncSource::Operations &ops)
{
    m_raw = raw;
    m_del = del;
    m_modTimeStamp = 0;
    m_revisionAccuracySeconds = granularity;
    if (raw) {
        ops.m_backupData = boost::bind(&SyncSourceRevisions::backupData,
                                       this, _1, _2, _3);
    }
    if (raw && del) {
        ops.m_restoreData = boost::bind(&SyncSourceRevisions::restoreData,
                                        this, _1, _2, _3, _4);
    }
    ops.m_endSession.push_back(boost::bind(&SyncSourceRevisions::sleepSinceModification,
                                           this));
}

std::string SyncSourceLogging::getDescription(sysync::KeyH aItemKey)
{
    try {
        std::list<std::string> values;

        BOOST_FOREACH(const std::string &field, m_fields) {
            SharedBuffer value;
            if (!getSynthesisAPI()->getValue(aItemKey, field, value) &&
                value.size()) {
                values.push_back(std::string(value.get()));
            }
        }

        std::string description = boost::join(values, m_sep);
        return description;
    } catch (...) {
        // Instead of failing we log the error and ask
        // the caller to log the UID. That way transient
        // errors or errors in the logging code don't
        // prevent syncs.
        handleException();
        return "";
    }
}

std::string SyncSourceLogging::getDescription(const string &luid)
{
    return "";
}

sysync::TSyError SyncSourceLogging::insertItemAsKey(sysync::KeyH aItemKey, sysync::ItemID newID, const boost::function<SyncSource::Operations::InsertItemAsKey_t> &parent)
{
    std::string description = getDescription(aItemKey);
    SE_LOG_INFO(this, NULL,
                description.empty() ? "%s <%s>" : "%s \"%s\"",
                "adding",
                !description.empty() ? description.c_str() : "???");
    if (parent) {
        return parent(aItemKey, newID);
    } else {
        return sysync::LOCERR_NOTIMP;
    }
}

sysync::TSyError SyncSourceLogging::updateItemAsKey(sysync::KeyH aItemKey, sysync::cItemID aID, sysync::ItemID newID, const boost::function<SyncSource::Operations::UpdateItemAsKey_t> &parent)
{
    std::string description = getDescription(aItemKey);
    SE_LOG_INFO(this, NULL,
                description.empty() ? "%s <%s>" : "%s \"%s\"",
                "updating",
                !description.empty() ? description.c_str() : aID ? aID->item : "???");
    if (parent) {
        return parent(aItemKey, aID, newID);
    } else {
        return sysync::LOCERR_NOTIMP;
    }
}

sysync::TSyError SyncSourceLogging::deleteItem(sysync::cItemID aID, const boost::function<SyncSource::Operations::DeleteItem_t> &parent)
{
    std::string description = getDescription(aID->item);
    SE_LOG_INFO(this, NULL,
                description.empty() ? "%s <%s>" : "%s \"%s\"",
                "deleting",
                !description.empty() ? description.c_str() : aID->item);
    if (parent) {
        return parent(aID);
    } else {
        return sysync::LOCERR_NOTIMP;
    }
}

void SyncSourceLogging::init(const std::list<std::string> &fields,
                             const std::string &sep,
                             SyncSource::Operations &ops)
{
    m_fields = fields;
    m_sep = sep;

    ops.m_insertItemAsKey = boost::bind(&SyncSourceLogging::insertItemAsKey,
                                        this, _1, _2, ops.m_insertItemAsKey);
    ops.m_updateItemAsKey = boost::bind(&SyncSourceLogging::updateItemAsKey,
                                        this, _1, _2, _3, ops.m_updateItemAsKey);
    ops.m_deleteItem = boost::bind(&SyncSourceLogging::deleteItem,
                                   this, _1, ops.m_deleteItem);
}
