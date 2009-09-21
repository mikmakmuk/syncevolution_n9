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

#ifndef INCL_CMDLINESYNCCLIENT
#define INCL_CMDLINESYNCCLIENT

#include "EvolutionSyncClient.h"

/**
 * a command line sync client for the purpose of
 * supporting a mechanism to save and retrieve password
 * in keyring.
 */
class CmdlineSyncClient : public EvolutionSyncClient {
 public:
    CmdlineSyncClient(const string &server,
                      bool doLogging = false,
                      const set<string> &sources = set<string>(),
                      bool useKeyring = false);

    using EvolutionSyncConfig::savePassword;

    /**
     * These 2 functions are from ConfigUserInterface and implement it
     * to use keyring to retrieve and save password in the keyring.
     */
    virtual string askPassword(const string &passwordName, const string &descr, const ConfigPasswordKey &key);
    virtual bool savePassword(const string &passwordName, const string &password, const ConfigPasswordKey &key); 

    void setKeyring(bool keyring) { m_keyring = keyring; }
    bool getKeyring() const { return m_keyring; }
 private:
    /** a bool flag used to indicate whether to use keyring to store password */
    bool m_keyring;
};

#endif // INCL_CMDLINESYNCCLIENT
