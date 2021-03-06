/*
 */

#ifndef CLIENTENGINE_CUSTOM_H
#define CLIENTENGINE_CUSTOM_H

/* Headers that might be available in precompiled form
 * (standard libraries, SyncML toolkit...)
 */
#include "clientengine_custom_precomp.h"

/* headers not suitable for / entirely included in precompilation */

// DB interface related includes
#ifdef SQL_SUPPORT
  #include "odbcdb.h"
#endif
#ifdef SDK_SUPPORT
  #include "plugindb.h"
#endif
#ifdef OUTLOOK_SUPPORT
  #include "outlookdb.h"
#endif


#endif // CLIENTENGINE_CUSTOM_H
