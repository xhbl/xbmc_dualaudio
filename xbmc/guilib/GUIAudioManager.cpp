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

#include "system.h"
#include "GUIAudioManager.h"
#include "Key.h"
#include "input/ButtonTranslator.h"
#include "settings/lib/Setting.h"
#include "threads/SingleLock.h"
#include "utils/URIUtils.h"
#include "utils/XBMCTinyXML.h"
#include "filesystem/Directory.h"
#include "addons/Skin.h"
#include "cores/AudioEngine/AEFactory.h"

using namespace std;

CGUIAudioManager g_audioManager;

CGUIAudioManager::CGUIAudioManager()
{
  m_bEnabled = false;
  m_bAudio2 = false;
}

CGUIAudioManager::~CGUIAudioManager()
{
}

void CGUIAudioManager::OnSettingChanged(const CSetting *setting)
{
  if (setting == NULL)
    return;

  const std::string &settingId = setting->GetId();
  if (settingId == "lookandfeel.soundskin")
  {
    Enable(true);
    Load();
  }
}

void CGUIAudioManager::Initialize()
{
}

void CGUIAudioManager::DeInitialize()
{
  CSingleLock lock(m_cs);
  UnLoad();
}

void CGUIAudioManager::Stop()
{
  CSingleLock lock(m_cs);
  for (windowSoundMap::iterator it = m_windowSoundMap.begin(); it != m_windowSoundMap.end(); ++it)
  {
    if (it->second.initSound  ) it->second.initSound  ->Stop();
    if (it->second.deInitSound) it->second.deInitSound->Stop();
    if (it->second.initSound2  ) it->second.initSound2  ->Stop();
    if (it->second.deInitSound2) it->second.deInitSound2->Stop();
  }

  for (pythonSoundsMap::iterator it = m_pythonSounds.begin(); it != m_pythonSounds.end(); ++it)
  {
    IAESound* sound = it->second.sound;
    sound->Stop();
    sound = it->second.sound2;
    if (sound) sound->Stop();
  }
}

// \brief Play a sound associated with a CAction
void CGUIAudioManager::PlayActionSound(const CAction& action)
{
  CSingleLock lock(m_cs);

  // it's not possible to play gui sounds when passthrough is active
  if (!m_bEnabled)
    return;

  actionSoundMap::iterator it = m_actionSoundMap.find(action.GetID());
  if (it == m_actionSoundMap.end())
    return;

  if (it->second.sound)
    it->second.sound->Play();
  if (m_bAudio2 && it->second.sound2)
    it->second.sound2->Play();
}

// \brief Play a sound associated with a window and its event
// Events: SOUND_INIT, SOUND_DEINIT
void CGUIAudioManager::PlayWindowSound(int id, WINDOW_SOUND event)
{
  CSingleLock lock(m_cs);

  // it's not possible to play gui sounds when passthrough is active
  if (!m_bEnabled)
    return;

  windowSoundMap::iterator it=m_windowSoundMap.find(id);
  if (it==m_windowSoundMap.end())
    return;

  CWindowSounds sounds=it->second;
  IAESound *sound = NULL;
  IAESound *sound2 = NULL;
  switch (event)
  {
  case SOUND_INIT:
    sound = sounds.initSound;
    sound2 = sounds.initSound2;
    break;
  case SOUND_DEINIT:
    sound = sounds.deInitSound;
    sound2 = sounds.deInitSound2;
    break;
  }

  if (!sound)
    return;

  sound->Play();

  if (m_bAudio2 && sound2)
    sound2->Play();
}

// \brief Play a sound given by filename
void CGUIAudioManager::PlayPythonSound(const CStdString& strFileName, bool useCached /*= true*/)
{
  CSingleLock lock(m_cs);

  // it's not possible to play gui sounds when passthrough is active
  if (!m_bEnabled)
    return;

  // If we already loaded the sound, just play it
  pythonSoundsMap::iterator itsb=m_pythonSounds.find(strFileName);
  if (itsb != m_pythonSounds.end())
  {
    IAESound* sound = itsb->second.sound;
    if (useCached)
    {
      sound->Play();
      sound = itsb->second.sound2;
      if (m_bAudio2 && sound) sound->Play();
      return;
    }
    else
    {
      FreeSoundAllUsage(sound);
      sound = itsb->second.sound2;
      if (m_bAudio2 && sound) FreeSoundAllUsage(sound);
      m_pythonSounds.erase(itsb);
    }
  }

  CAPSounds aps = LoadSound(strFileName);
  if (!aps.sound)
    return;

  m_pythonSounds.insert(pair<const CStdString, CAPSounds>(strFileName, aps));
  aps.sound->Play();
  if (m_bAudio2 && aps.sound2)
    aps.sound2->Play();
}

void CGUIAudioManager::UnLoad()
{
  //  Free sounds from windows
  {
    windowSoundMap::iterator it = m_windowSoundMap.begin();
    while (it != m_windowSoundMap.end())
    {
      if (it->second.initSound  ) FreeSound(it->second.initSound  );
      if (it->second.deInitSound) FreeSound(it->second.deInitSound);
      m_windowSoundMap.erase(it++);
    }
  }

  // Free sounds from python
  {
    pythonSoundsMap::iterator it = m_pythonSounds.begin();
    while (it != m_pythonSounds.end())
    {
      IAESound* sound = it->second.sound;
      FreeSound(sound);
      m_pythonSounds.erase(it++);
    }
  }

  // free action sounds
  {
    actionSoundMap::iterator it = m_actionSoundMap.begin();
    while (it != m_actionSoundMap.end())
    {
      IAESound* sound = it->second.sound;
      FreeSound(sound);
      m_actionSoundMap.erase(it++);
    }
  }
}

// \brief Load the config file (sounds.xml) for nav sounds
// Can be located in a folder "sounds" in the skin or from a
// subfolder of the folder "sounds" in the root directory of
// xbmc
bool CGUIAudioManager::Load()
{
  CSingleLock lock(m_cs);

  UnLoad();
  CheckAudio2();

  if (CSettings::Get().GetString("lookandfeel.soundskin")=="OFF")
    return true;
  else
    Enable(true);

  CStdString soundSkin = CSettings::Get().GetString("lookandfeel.soundskin");

  if (soundSkin == "SKINDEFAULT")
  {
    m_strMediaDir = URIUtils::AddFileToFolder(g_SkinInfo->Path(), "sounds");
  }
  else
  {
    //check if sound skin is located in home, otherwise fallback to built-ins
    m_strMediaDir = URIUtils::AddFileToFolder("special://home/sounds", soundSkin);
    if (!XFILE::CDirectory::Exists(m_strMediaDir))
      m_strMediaDir = URIUtils::AddFileToFolder("special://xbmc/sounds", soundSkin);
  }

  CStdString strSoundsXml = URIUtils::AddFileToFolder(m_strMediaDir, "sounds.xml");

  //  Load our xml file
  CXBMCTinyXML xmlDoc;

  CLog::Log(LOGINFO, "Loading %s", strSoundsXml.c_str());

  //  Load the config file
  if (!xmlDoc.LoadFile(strSoundsXml))
  {
    CLog::Log(LOGNOTICE, "%s, Line %d\n%s", strSoundsXml.c_str(), xmlDoc.ErrorRow(), xmlDoc.ErrorDesc());
    return false;
  }

  TiXmlElement* pRoot = xmlDoc.RootElement();
  CStdString strValue = pRoot->Value();
  if ( strValue != "sounds")
  {
    CLog::Log(LOGNOTICE, "%s Doesn't contain <sounds>", strSoundsXml.c_str());
    return false;
  }

  //  Load sounds for actions
  TiXmlElement* pActions = pRoot->FirstChildElement("actions");
  if (pActions)
  {
    TiXmlNode* pAction = pActions->FirstChild("action");

    while (pAction)
    {
      TiXmlNode* pIdNode = pAction->FirstChild("name");
      int id = 0;    // action identity
      if (pIdNode && pIdNode->FirstChild())
      {
        CButtonTranslator::TranslateActionString(pIdNode->FirstChild()->Value(), id);
      }

      TiXmlNode* pFileNode = pAction->FirstChild("file");
      CStdString strFile;
      if (pFileNode && pFileNode->FirstChild())
        strFile += pFileNode->FirstChild()->Value();

      if (id > 0 && !strFile.empty())
      {
        CStdString filename = URIUtils::AddFileToFolder(m_strMediaDir, strFile);
        CAPSounds aps = LoadSound(filename);
        if (aps.sound)
          m_actionSoundMap.insert(pair<int, CAPSounds>(id, aps));
      }

      pAction = pAction->NextSibling();
    }
  }

  //  Load window specific sounds
  TiXmlElement* pWindows = pRoot->FirstChildElement("windows");
  if (pWindows)
  {
    TiXmlNode* pWindow = pWindows->FirstChild("window");

    while (pWindow)
    {
      int id = 0;

      TiXmlNode* pIdNode = pWindow->FirstChild("name");
      if (pIdNode)
      {
        if (pIdNode->FirstChild())
          id = CButtonTranslator::TranslateWindow(pIdNode->FirstChild()->Value());
      }

      CWindowSounds sounds;
	  CAPSounds aps;
	  aps = LoadWindowSound(pWindow, "activate"  );
	  sounds.initSound   = aps.sound;
	  sounds.initSound2   = aps.sound2;
	  aps = LoadWindowSound(pWindow, "deactivate"  );
      sounds.deInitSound = aps.sound;
      sounds.deInitSound2 = aps.sound2;

      if (id > 0)
        m_windowSoundMap.insert(pair<int, CWindowSounds>(id, sounds));

      pWindow = pWindow->NextSibling();
    }
  }

  return true;
}

CGUIAudioManager::CAPSounds CGUIAudioManager::LoadSound(const CStdString &filename)
{
  CSingleLock lock(m_cs);
  CAPSounds aps;
  aps.sound = aps.sound2 = NULL;
  soundCache::iterator it = m_soundCache.find(filename);
  if (it != m_soundCache.end())
  {
    ++it->second.usage;
	aps.sound = it->second.sound;
	aps.sound2 = it->second.sound2;
    return aps;
  }

  IAESound *sound = CAEFactory::MakeSound(filename);
  if (!sound)
    return aps;
  IAESound *sound2 = CAEFactory::MakeSound(filename,true);

  CSoundInfo info;
  info.usage = 1;
  info.sound = sound;
  info.sound2 = sound2;
  m_soundCache[filename] = info;

  aps.sound = info.sound;
  aps.sound2 = info.sound2;
  return aps;
}

void CGUIAudioManager::FreeSound(IAESound *sound)
{
  CSingleLock lock(m_cs);
  for(soundCache::iterator it = m_soundCache.begin(); it != m_soundCache.end(); ++it) {
    if (it->second.sound == sound) {
      if (--it->second.usage == 0) {     
        CAEFactory::FreeSound(sound);
        if(it->second.sound2) CAEFactory::FreeSound(it->second.sound2);
        m_soundCache.erase(it);
      }
      return;
    }
  }
}

void CGUIAudioManager::FreeSoundAllUsage(IAESound *sound)
{
  CSingleLock lock(m_cs);
  for(soundCache::iterator it = m_soundCache.begin(); it != m_soundCache.end(); ++it) {
    if (it->second.sound == sound) {   
      CAEFactory::FreeSound(sound);
      if(it->second.sound2) CAEFactory::FreeSound(it->second.sound2);
      m_soundCache.erase(it);
      return;
    }
  }
}

// \brief Load a window node of the config file (sounds.xml)
CGUIAudioManager::CAPSounds CGUIAudioManager::LoadWindowSound(TiXmlNode* pWindowNode, const CStdString& strIdentifier)
{
  CAPSounds aps;
  aps.sound = aps.sound2 = NULL;

  if (!pWindowNode)
    return aps;

  TiXmlNode* pFileNode = pWindowNode->FirstChild(strIdentifier);
  if (pFileNode && pFileNode->FirstChild())
    return LoadSound(URIUtils::AddFileToFolder(m_strMediaDir, pFileNode->FirstChild()->Value()));

  return aps;
}

// \brief Enable/Disable nav sounds
void CGUIAudioManager::Enable(bool bEnable)
{
  // always deinit audio when we don't want gui sounds
  if (CSettings::Get().GetString("lookandfeel.soundskin")=="OFF")
    bEnable = false;

  CSingleLock lock(m_cs);
  m_bEnabled = bEnable;
}

// \brief Sets the volume of all playing sounds
void CGUIAudioManager::SetVolume(float level)
{
  CSingleLock lock(m_cs);

  {
    actionSoundMap::iterator it = m_actionSoundMap.begin();
    while (it!=m_actionSoundMap.end())
    {
      if (it->second.sound)
        it->second.sound->SetVolume(level);
      if (it->second.sound2)
        it->second.sound2->SetVolume(level);
      ++it;
    }
  }

  for(windowSoundMap::iterator it = m_windowSoundMap.begin(); it != m_windowSoundMap.end(); ++it)
  {
    if (it->second.initSound  ) it->second.initSound  ->SetVolume(level);
    if (it->second.deInitSound) it->second.deInitSound->SetVolume(level);
    if (it->second.initSound2  ) it->second.initSound2  ->SetVolume(level);
    if (it->second.deInitSound2) it->second.deInitSound2->SetVolume(level);
  }

  {
    pythonSoundsMap::iterator it = m_pythonSounds.begin();
    while (it != m_pythonSounds.end())
    {
      if (it->second.sound)
        it->second.sound->SetVolume(level);
      if (it->second.sound2)
        it->second.sound2->SetVolume(level);

      ++it;
    }
  }
}

void CGUIAudioManager::CheckAudio2()
{
  m_bAudio2 = CSettings::Get().GetBool("audiooutput2.enabled");
}
