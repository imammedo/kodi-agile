/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "AddonDll.h"

#include "AddonStatusHandler.h"
#include "GUIUserMessages.h"
#include "addons/GUIDialogAddonSettings.h"
#include "events/EventLog.h"
#include "events/NotificationEvent.h"
#include "guilib/GUIWindowManager.h"
#include "dialogs/GUIDialogOK.h"
#include "utils/URIUtils.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "filesystem/Directory.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "Util.h"

// Global addon callback handle classes
#include "addons/interfaces/AudioEngine.h"
#include "addons/interfaces/Filesystem.h"
#include "addons/interfaces/General.h"
#include "addons/interfaces/Network.h"
#include "addons/interfaces/kodi/gui/General.h"

namespace ADDON
{

// IAddonInstanceHandler::IAddonInstanceHandler(TYPE type, const std::string& instanceID/* = ""*/)
//   : m_type(type),
//     m_parentInstance(nullptr)
// {
//   m_instanceId = !instanceID.empty() ? instanceID : StringUtils::Format("%p", static_cast<void*>(this));
// }
// 
// IAddonInstanceHandler::IAddonInstanceHandler(TYPE type, const AddonInfoPtr& addonInfo, kodi::addon::IAddonInstance* parentInstance/* = nullptr*/, const std::string& instanceID/* = ""*/)
//   : m_type(type),
//     m_parentInstance(parentInstance),
//     m_addonInfo(addonInfo)
// {
//   m_instanceId = !instanceID.empty() ? instanceID : StringUtils::Format("%p", static_cast<void*>(this));
// 
//   m_addon = CAddonMgr::GetInstance().GetAddon(addonInfo, this);
//   if (!m_addon)
//     CLog::Log(LOGFATAL, "ADDON::IAddonInstanceHandler: Tried to get add-on '%s' who not available!", addonInfo->ID().c_str());
// }
// 
// IAddonInstanceHandler::~IAddonInstanceHandler()
// {
//   if (m_addon)
//     CAddonMgr::GetInstance().ReleaseAddon(m_addon, this);
// }
// 
// bool IAddonInstanceHandler::CreateInstance(ADDON_TYPE instanceType, KODI_HANDLE instance)
// {
//   return m_addon->CreateInstance(instanceType, m_instanceId, instance, m_parentInstance) == ADDON_STATUS_OK;
// }
// 
// void IAddonInstanceHandler::DestroyInstance()
// {
//   if (m_addon)
//     m_addon->DestroyInstance(m_instanceId);
// }

CAddonDll::CAddonDll(AddonInfoPtr props)
  : CAddon(props),
    m_bIsChild(false)
{
  m_initialized = false;
  m_pDll        = NULL;
  m_pHelpers    = NULL;
  m_needsavedsettings = false;
  m_parentLib.clear();
  m_interface = {0};
}

CAddonDll::CAddonDll(const CAddonDll &rhs)
  : CAddon(rhs),
    m_bIsChild(true)
{
  m_initialized       = rhs.m_initialized;
  m_pDll              = rhs.m_pDll;
  m_pHelpers          = rhs.m_pHelpers;
  m_needsavedsettings = rhs.m_needsavedsettings;
  m_parentLib = rhs.m_parentLib;
  m_interface = rhs.m_interface;
}

CAddonDll::~CAddonDll()
{
  if (m_initialized)
    Destroy();
}

bool CAddonDll::LoadDll()
{
  if (m_pDll)
    return true;

  std::string strFileName;
  std::string strAltFileName;
  if (!m_bIsChild)
  {
    strFileName = MainLibPath();
  }
  else
  {
    std::string libPath = MainLibPath();
    if (!XFILE::CFile::Exists(libPath))
    {
      std::string temp = CSpecialProtocol::TranslatePath("special://xbmc/");
      std::string tempbin = CSpecialProtocol::TranslatePath("special://xbmcbin/");
      libPath.erase(0, temp.size());
      libPath = tempbin + libPath;
      if (!XFILE::CFile::Exists(libPath))
      {
        CLog::Log(LOGERROR, "ADDON: Could not locate %s", AddonInfo()->Libname().c_str());
        return false;
      }
    }

    std::stringstream childcount;
    childcount << GetChildCount();
    std::string extension = URIUtils::GetExtension(libPath);
    strFileName = "special://temp/" + ID() + "-" + childcount.str() + extension;

    XFILE::CFile::Copy(libPath, strFileName);

    m_parentLib = libPath;
    CLog::Log(LOGNOTICE, "ADDON: Loaded virtual child addon %s", strFileName.c_str());
  }

  /* Check if lib being loaded exists, else check in XBMC binary location */
#if defined(TARGET_ANDROID)
  // Android libs MUST live in this path, else multi-arch will break.
  // The usual soname requirements apply. no subdirs, and filename is ^lib.*\.so$
  if (!XFILE::CFile::Exists(strFileName))
  {
    std::string tempbin = getenv("XBMC_ANDROID_LIBS");
    strFileName = tempbin + "/" + AddonInfo()->Libname().c_str();
  }
#endif
  if (!XFILE::CFile::Exists(strFileName))
  {
    std::string altbin = CSpecialProtocol::TranslatePath("special://xbmcaltbinaddons/");
    if (!altbin.empty())
    {
      strAltFileName = altbin + AddonInfo()->Libname().c_str();
      if (!XFILE::CFile::Exists(strAltFileName))
      {
        std::string temp = CSpecialProtocol::TranslatePath("special://xbmc/addons/");
        strAltFileName = strFileName;
        strAltFileName.erase(0, temp.size());
        strAltFileName = altbin + strAltFileName;
      }
      CLog::Log(LOGDEBUG, "ADDON: Trying to load %s", strAltFileName.c_str());
    }

    if (XFILE::CFile::Exists(strAltFileName))
      strFileName = strAltFileName;
    else
    {
      std::string temp = CSpecialProtocol::TranslatePath("special://xbmc/");
      std::string tempbin = CSpecialProtocol::TranslatePath("special://xbmcbin/");
      strFileName.erase(0, temp.size());
      strFileName = tempbin + strFileName;
      if (!XFILE::CFile::Exists(strFileName))
      {
        CLog::Log(LOGERROR, "ADDON: Could not locate %s", AddonInfo()->Libname().c_str());
        return false;
      }
    }
  }

  /* Load the Dll */
  m_pDll = new DllAddon;
  m_pDll->SetFile(strFileName);
  m_pDll->EnableDelayedUnload(false);
  if (!m_pDll->Load())
  {
    delete m_pDll;
    m_pDll = NULL;

    CGUIDialogOK* pDialog = g_windowManager.GetWindow<CGUIDialogOK>(WINDOW_DIALOG_OK);
    if (pDialog)
    {
      std::string heading = StringUtils::Format("%s: %s", CAddonInfo::TranslateType(MainType(), true).c_str(), Name().c_str());
      pDialog->SetHeading(CVariant{heading});
      pDialog->SetLine(1, CVariant{24070});
      pDialog->SetLine(2, CVariant{24071});
      pDialog->SetLine(2, CVariant{"Can't load shared library"});
      pDialog->Open();
    }

    return false;
  }

  return true;
}

ADDON_STATUS CAddonDll::Create(ADDON_TYPE type, void* funcTable, void* info)
{
  /* ensure that a previous instance is destroyed */
  Destroy();

  if (!funcTable)
    return ADDON_STATUS_PERMANENT_FAILURE;

  CLog::Log(LOGDEBUG, "ADDON: Dll Initializing - %s", Name().c_str());
  m_initialized = false;

  if (!LoadDll())
    return ADDON_STATUS_PERMANENT_FAILURE;

  /* Check requested instance version on add-on */
  if (!CheckAPIVersion(type))
    return ADDON_STATUS_PERMANENT_FAILURE;

  /* Check versions about global parts on add-on (parts used on all types) */
  for (unsigned int id = ADDON_GLOBAL_MAIN; id <= ADDON_GLOBAL_MAX; ++id)
  {
    if (!CheckAPIVersion(id))
      return ADDON_STATUS_PERMANENT_FAILURE;
  }

  /* Load add-on function table (written by add-on itself) */
  m_pDll->GetAddon(funcTable);

  /* Allocate the helper function class to allow crosstalk over
     helper libraries */
  m_pHelpers = new CAddonInterfaces(this);

  /* Call Create to make connections, initializing data or whatever is
     needed to become the AddOn running */
  ADDON_STATUS status = m_pDll->Create(m_pHelpers->GetCallbacks(), info);
  if (status == ADDON_STATUS_OK)
  {
    m_initialized = true;
  }
  else if ((status == ADDON_STATUS_NEED_SETTINGS) || (status == ADDON_STATUS_NEED_SAVEDSETTINGS))
  {
    m_needsavedsettings = (status == ADDON_STATUS_NEED_SAVEDSETTINGS) ? true : false;
    status = TransferSettings();
    if (status == ADDON_STATUS_OK)
      m_initialized = true;
    else
      new CAddonStatusHandler(ID(), status, "", false);
  }
  else
  { // Addon failed initialization
    CLog::Log(LOGERROR, "ADDON: Dll %s - Client returned bad status (%i) from Create and is not usable", Name().c_str(), status);
    
    CGUIDialogOK* pDialog = g_windowManager.GetWindow<CGUIDialogOK>(WINDOW_DIALOG_OK);
    if (pDialog)
    {
      std::string heading = StringUtils::Format("%s: %s", CAddonInfo::TranslateType(MainType(), true).c_str(), Name().c_str());
      pDialog->SetHeading(CVariant{heading});
      pDialog->SetLine(1, CVariant{24070});
      pDialog->SetLine(2, CVariant{24071});
      pDialog->Open();
    }
  }

  return status;
}

ADDON_STATUS CAddonDll::Create(KODI_HANDLE firstKodiInstance)
{
  CLog::Log(LOGDEBUG, "ADDON: Dll Initializing - %s", Name().c_str());
  m_initialized = false;

  if (!LoadDll())
  {
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  /* Check versions about global parts on add-on (parts used on all types) */
  for (unsigned int id = ADDON_GLOBAL_MAIN; id <= ADDON_GLOBAL_MAX; ++id)
  {
    if (!CheckAPIVersion(id))
      return ADDON_STATUS_PERMANENT_FAILURE;
  }

  /* Allocate the helper function class to allow crosstalk over
     helper add-on headers */
  if (!InitInterface(firstKodiInstance))
    return ADDON_STATUS_PERMANENT_FAILURE;

  /* Call Create to make connections, initializing data or whatever is
     needed to become the AddOn running */
  ADDON_STATUS status = m_pDll->Create(&m_interface, nullptr);
  if (status == ADDON_STATUS_OK)
  {
    m_initialized = true;
  }
  else if ((status == ADDON_STATUS_NEED_SETTINGS) || (status == ADDON_STATUS_NEED_SAVEDSETTINGS))
  {
    m_needsavedsettings = (status == ADDON_STATUS_NEED_SAVEDSETTINGS);
    if ((status = TransferSettings()) == ADDON_STATUS_OK)
      m_initialized = true;
    else
      new CAddonStatusHandler(ID(), status, "", false);
  }
  else
  { // Addon failed initialization
    CLog::Log(LOGERROR, "ADDON: Dll %s - Client returned bad status (%i) from Create and is not usable", Name().c_str(), status);

    // @todo currently a copy and paste from other function and becomes improved.
    CGUIDialogOK* pDialog = g_windowManager.GetWindow<CGUIDialogOK>(WINDOW_DIALOG_OK);
    if (pDialog)
    {
      std::string heading = StringUtils::Format("%s: %s", CAddonInfo::TranslateType(MainType(), true).c_str(), Name().c_str());
      pDialog->SetHeading(CVariant{heading});
      pDialog->SetLine(1, CVariant{24070});
      pDialog->SetLine(2, CVariant{24071});
      pDialog->Open();
    }
  }

  return status;
}

void CAddonDll::Destroy()
{
  /* Unload library file */
  if (m_pDll)
  {
    /* Inform dll to stop all activities */
    if (m_needsavedsettings)  // If the addon supports it we save some settings to settings.xml before stop
    {
      char   str_id[64] = "";
      char   str_value[1024];
      CAddon::LoadUserSettings();
      for (unsigned int i=0; (strcmp(str_id,"###End") != 0); i++)
      {
        strcpy(str_id, "###GetSavedSettings");
        sprintf (str_value, "%i", i);
        ADDON_STATUS status = m_pDll->SetSetting((const char*)&str_id, (void*)&str_value);

        if (status == ADDON_STATUS_UNKNOWN)
          break;

        if (strcmp(str_id,"###End") != 0) UpdateSetting(str_id, str_value);
      }
      CAddon::SaveSettings();
    }

    /* Make sure all instances are destroyed if interface becomes stopped. */
    for (auto it = m_usedInstances.begin(); it != m_usedInstances.end(); ++it)
    {
      m_interface.toAddon->destroy_instance(it->second.first, it->second.second);
      m_usedInstances.erase(it);
    }

    /* Unload library file */
    m_pDll->Destroy();
    m_pDll->Unload();
  }

  DeInitInterface();

  delete m_pHelpers;
  m_pHelpers = NULL;
  if (m_pDll)
  {
    if (m_bIsChild)
      XFILE::CFile::Delete(m_pDll->GetFile());
    delete m_pDll;
    m_pDll = NULL;
    CLog::Log(LOGINFO, "ADDON: Dll Destroyed - %s", Name().c_str());
  }
  m_initialized = false;
}

ADDON_STATUS CAddonDll::CreateInstance(ADDON_TYPE instanceType, const std::string& instanceID, KODI_HANDLE instance, KODI_HANDLE parentInstance)
{
  ADDON_STATUS status = ADDON_STATUS_OK;

  if (!m_initialized)
    status = Create(instance);
  if (status != ADDON_STATUS_OK)
    return status;

  /* Check version of requested instance type */
  if (!CheckAPIVersion(instanceType))
    return ADDON_STATUS_PERMANENT_FAILURE;

  KODI_HANDLE addonInstance;
  status = m_interface.toAddon->create_instance(instanceType, instanceID.c_str(), instance, &addonInstance, parentInstance);
  if (status == ADDON_STATUS_OK)
  {
    m_usedInstances[instanceID] = std::make_pair(instanceType, addonInstance);
  }
  return status;
}

void CAddonDll::DestroyInstance(const std::string& instanceID)
{
  auto it = m_usedInstances.find(instanceID);
  if (it != m_usedInstances.end())
  {
    m_interface.toAddon->destroy_instance(it->second.first, it->second.second);
    m_usedInstances.erase(it);
  }

  if (m_usedInstances.empty())
    Destroy();
}

bool CAddonDll::DllLoaded(void) const
{
  return m_pDll != NULL;
}

ADDON_STATUS CAddonDll::GetStatus()
{
  return m_pDll->GetStatus();
}

bool CAddonDll::IsInUse() const
{
  return !m_usedInstances.empty();
}

void CAddonDll::SaveSettings()
{
  // must save first, as TransferSettings() reloads saved settings!
  CAddon::SaveSettings();
  if (m_initialized)
    TransferSettings();
}

ADDON_STATUS CAddonDll::TransferSettings()
{
  bool restart = false;
  ADDON_STATUS reportStatus = ADDON_STATUS_OK;

  CLog::Log(LOGDEBUG, "Calling TransferSettings for: %s", Name().c_str());

  LoadSettings();

  const TiXmlElement *category = m_addonXmlDoc.RootElement() ? m_addonXmlDoc.RootElement()->FirstChildElement("category") : NULL;
  if (!category)
    category = m_addonXmlDoc.RootElement(); // no categories

  while (category)
  {
    const TiXmlElement *setting = category->FirstChildElement("setting");
    while (setting)
    {
      ADDON_STATUS status = ADDON_STATUS_OK;
      const char *id = setting->Attribute("id");
      const std::string type = XMLUtils::GetAttribute(setting, "type");
      const char *option = setting->Attribute("option");

      if (id && !type.empty())
      {
        if (type == "sep" || type == "lsep")
        {
          /* Don't propagate separators */
        }
        else if (type == "text"       || type == "ipaddress" ||
                 type == "video"      || type == "audio"     ||
                 type == "image"      || type == "folder"    ||
                 type == "executable" || type == "file"      ||
                 type == "action"     || type == "date"      ||
                 type == "time"       || type == "select"    ||
                 type == "addon"      || type == "labelenum" ||
                 type == "fileenum" )
        {
          status = m_pDll->SetSetting(id, (const char*) GetSetting(id).c_str());
        }
        else if (type == "enum"      || type =="integer" ||
                 type == "labelenum" || type == "rangeofnum")
        {
          int tmp = atoi(GetSetting(id).c_str());
          status = m_pDll->SetSetting(id, (int*) &tmp);
        }
        else if (type == "bool")
        {
          bool tmp = (GetSetting(id) == "true") ? true : false;
          status = m_pDll->SetSetting(id, (bool*) &tmp);
        }
        else if (type == "rangeofnum" || type == "slider" ||
                 type == "number")
        {
          float tmpf = (float)atof(GetSetting(id).c_str());
          int   tmpi;

          if (option && strcmpi(option,"int") == 0)
          {
            tmpi = (int)floor(tmpf);
            status = m_pDll->SetSetting(id, (int*) &tmpi);
          }
          else
          {
            status = m_pDll->SetSetting(id, (float*) &tmpf);
          }
        }
        else
        {
          /* Log unknowns as an error, but go ahead and transfer the string */
          CLog::Log(LOGERROR, "Unknown setting type '%s' for %s", type.c_str(), Name().c_str());
          status = m_pDll->SetSetting(id, (const char*) GetSetting(id).c_str());
        }

        if (status == ADDON_STATUS_NEED_RESTART)
          restart = true;
        else if (status != ADDON_STATUS_OK)
          reportStatus = status;
      }
      setting = setting->NextSiblingElement("setting");
    }
    category = category->NextSiblingElement("category");
  }

  if (restart || reportStatus != ADDON_STATUS_OK)
  {
    new CAddonStatusHandler(ID(), restart ? ADDON_STATUS_NEED_RESTART : reportStatus, "", true);
  }

  return ADDON_STATUS_OK;
}

void CAddonDll::UpdateSettings(std::map<std::string, std::string>& settings)
{
  if (m_interface.toAddon &&
      m_interface.toAddon->set_setting)
  {
    for (auto it = settings.begin(); it != settings.end(); ++it)
      m_interface.toAddon->set_setting(it->first.c_str(), it->second.c_str()/*, (std::next(it) == settings.end()) ? true : false*/);
  }

  CAddon::UpdateSettings(settings);
}

bool CAddonDll::CheckAPIVersion(int type)
{
  /* check the API version */
  AddonVersion kodiMinVersion(kodi::addon::GetTypeMinVersion(type));
  AddonVersion addonVersion(m_pDll->GetAddonTypeVersion(type));

  /* Check the global usage from addon
   * if not used from addon becomes "0.0.0" returned
   */
  if (type <= ADDON_GLOBAL_MAX && addonVersion == AddonVersion("0.0.0"))
    return true;

  /* If a instance (not global) version becomes checked must be the version
   * present.
   */
  if (kodiMinVersion > addonVersion || 
      addonVersion > AddonVersion(kodi::addon::GetTypeVersion(type)))
  {
    CLog::Log(LOGERROR, "Add-on '%s' is using an incompatible API version for type '%s'. Kodi API min version = '%s', add-on API version '%s'",
                            Name().c_str(),
                            kodi::addon::GetTypeName(type),
                            kodiMinVersion.asString().c_str(),
                            addonVersion.asString().c_str());

    CEventLog::GetInstance().AddWithNotification(EventPtr(new CNotificationEvent(Name(), 24152, EventLevel::Error)));

    return false;
  }

  return true;
}

/*!
 * @brief Addon to Kodi basic callbacks below
 *
 * The amount of functions here are hold so minimal as possible. Only parts
 * where needed on nearly every add-on (e.g. addon_log_msg) are to add there.
 *
 * More specific parts like e.g. to open files should be added to a separate
 * part.
 */
//@{

bool CAddonDll::InitInterface(KODI_HANDLE firstKodiInstance)
{
  m_interface = {0};

  m_interface.libBasePath = strdup(CSpecialProtocol::TranslatePath("special://xbmcbinaddons").c_str());
  m_interface.addonBase = nullptr;
  m_interface.globalSingleInstance = nullptr;
  m_interface.firstKodiInstance = firstKodiInstance;

  // Create function list from kodi to addon, generated with malloc to have
  // compatible with other versions
  m_interface.toKodi = (AddonToKodiFuncTable_Addon*) malloc(sizeof(AddonToKodiFuncTable_Addon));
  m_interface.toKodi->kodiBase = this;
  m_interface.toKodi->get_addon_path = get_addon_path;
  m_interface.toKodi->get_base_user_path = get_base_user_path;
  m_interface.toKodi->addon_log_msg = addon_log_msg;
  m_interface.toKodi->get_setting = get_setting;
  m_interface.toKodi->set_setting = set_setting;
  m_interface.toKodi->free_string = free_string;

  // Create function list from addon to kodi, generated with calloc to have
  // compatible with other versions and everything with "0"
  // Related parts becomes set from addon headers.
  m_interface.toAddon = (KodiToAddonFuncTable_Addon*) calloc(1, sizeof(KodiToAddonFuncTable_Addon));

  Interface_General::Init(&m_interface);
  Interface_AudioEngine::Init(&m_interface);
  Interface_Filesystem::Init(&m_interface);
  Interface_Network::Init(&m_interface);
  Interface_GUIGeneral::Init(&m_interface);

  return true;
}

void CAddonDll::DeInitInterface()
{
  Interface_Network::DeInit(&m_interface);
  Interface_Filesystem::DeInit(&m_interface);
  Interface_AudioEngine::DeInit(&m_interface);
  Interface_General::DeInit(&m_interface);
  Interface_GUIGeneral::DeInit(&m_interface);

  if (m_interface.libBasePath)
    free((char*)m_interface.libBasePath);
  if (m_interface.toKodi)
    free((char*)m_interface.toKodi);
  if (m_interface.toAddon)
    free((char*)m_interface.toAddon);
  m_interface = {0};
}

char* CAddonDll::get_addon_path(void* kodiBase)
{ 
  CAddonDll* addon = static_cast<CAddonDll*>(kodiBase);
  if (addon == nullptr)
  {
    CLog::Log(LOGERROR, "get_addon_path(...) called with empty kodi instance pointer");
    return nullptr;
  }

  return strdup(CSpecialProtocol::TranslatePath(addon->Path()).c_str());
}

char* CAddonDll::get_base_user_path(void* kodiBase)
{ 
  CAddonDll* addon = static_cast<CAddonDll*>(kodiBase);
  if (addon == nullptr)
  {
    CLog::Log(LOGERROR, "get_base_user_path(...) called with empty kodi instance pointer");
    return nullptr;
  }

  return strdup(CSpecialProtocol::TranslatePath(addon->Profile()).c_str());
}

void CAddonDll::addon_log_msg(void* kodiBase, const int addonLogLevel, const char* strMessage)
{
  CAddonDll* addon = static_cast<CAddonDll*>(kodiBase);
  if (addon == nullptr)
  {
    CLog::Log(LOGERROR, "addon_log_msg(...) called with empty kodi instance pointer");
    return;
  }

  int logLevel = LOGNONE;
  switch (addonLogLevel)
  {
    case ADDON_LOG_FATAL:
      logLevel = LOGFATAL;
      break;
    case ADDON_LOG_SEVERE:
      logLevel = LOGSEVERE;
      break;
    case ADDON_LOG_ERROR:
      logLevel = LOGERROR;
      break;
    case ADDON_LOG_WARNING:
      logLevel = LOGWARNING;
      break;
    case ADDON_LOG_NOTICE:
      logLevel = LOGNOTICE;
      break;
    case ADDON_LOG_INFO:
      logLevel = LOGINFO;
      break;
    case ADDON_LOG_DEBUG:
      logLevel = LOGDEBUG;
      break;
    default:
      break;
  }

  CLog::Log(logLevel, "AddOnLog: %s: %s", addon->Name().c_str(), strMessage);
}

bool CAddonDll::get_setting(void* kodiBase, const char* settingName, void* settingValue)
{ 
  CAddonDll* addon = static_cast<CAddonDll*>(kodiBase);
  if (addon == nullptr || settingName == nullptr || settingValue == nullptr)
  {
    CLog::Log(LOGERROR, "kodi::General::%s - invalid data (addon='%p', settingName='%p', settingValue='%p')",
                                        __FUNCTION__, addon, settingName, settingValue);

    return false;
  }

  CLog::Log(LOGDEBUG, "kodi::General::%s - add-on '%s' requests setting '%s'",
                        __FUNCTION__, addon->Name().c_str(), settingName);

  if (!addon->ReloadSettings())
  {
    CLog::Log(LOGERROR, "kodi::General::%s - could't get settings for add-on '%s'", __FUNCTION__, addon->Name().c_str());
    return false;
  }

  const TiXmlElement *category = addon->GetSettingsXML()->FirstChildElement("category");
  if (!category) // add a default one...
    category = addon->GetSettingsXML();

  while (category)
  {
    const TiXmlElement *setting = category->FirstChildElement("setting");
    while (setting)
    {
      const std::string   id = XMLUtils::GetAttribute(setting, "id");
      const std::string type = XMLUtils::GetAttribute(setting, "type");

      if (id == settingName && !type.empty())
      {
        if (type == "text"     || type == "ipaddress" ||
            type == "folder"   || type == "action"    ||
            type == "music"    || type == "pictures"  ||
            type == "programs" || type == "fileenum"  ||
            type == "file"     || type == "labelenum")
        {
          *(char**) settingValue = strdup(addon->GetSetting(id).c_str());
          return true;
        }
        else if (type == "number" || type == "enum")
        {
          *(int*) settingValue = (int) atoi(addon->GetSetting(id).c_str());
          return true;
        }
        else if (type == "bool")
        {
          *(bool*) settingValue = (bool) (addon->GetSetting(id) == "true" ? true : false);
          return true;
        }
        else if (type == "slider")
        {
          const char *option = setting->Attribute("option");
          if (option && strcmpi(option, "int") == 0)
          {
            *(int*) settingValue = (int) atoi(addon->GetSetting(id).c_str());
            return true;
          }
          else
          {
            *(float*) settingValue = (float) atof(addon->GetSetting(id).c_str());
            return true;
          }
        }
      }
      setting = setting->NextSiblingElement("setting");
    }
    category = category->NextSiblingElement("category");
  }
  CLog::Log(LOGERROR, "kodi::General::%s - can't find setting '%s' in '%s'", __FUNCTION__, settingName, addon->Name().c_str());

  return false;
}
  
bool CAddonDll::set_setting(void* kodiBase, const char* settingName, const char* settingValue)
{
  CAddonDll* addon = static_cast<CAddonDll*>(kodiBase);
  if (addon == nullptr || settingName == nullptr || settingValue == nullptr)
  {
    CLog::Log(LOGERROR, "kodi::General::%s - invalid data (addon='%p', settingName='%p', settingValue='%p')",
                                        __FUNCTION__, addon, settingName, settingValue);

    return false;
  }

  bool save = true;
  if (g_windowManager.IsWindowActive(WINDOW_DIALOG_ADDON_SETTINGS))
  {
    CGUIDialogAddonSettings* dialog = g_windowManager.GetWindow<CGUIDialogAddonSettings>(WINDOW_DIALOG_ADDON_SETTINGS);
    if (dialog->GetCurrentID() == addon->ID())
    {
      CGUIMessage message(GUI_MSG_SETTING_UPDATED, 0, 0);
      std::vector<std::string> params;
      params.push_back(settingName);
      params.push_back(settingValue);
      message.SetStringParams(params);
      g_windowManager.SendThreadMessage(message, WINDOW_DIALOG_ADDON_SETTINGS);
      save=false;
    }
  }
  if (save)
  {
    addon->UpdateSetting(settingName, settingValue);
    addon->SaveSettings();
  }
  
  return true;
}

void CAddonDll::free_string(void* kodiBase, char* str)
{
  if (str)
    free(str);
}

//@}

}; /* namespace ADDON */

