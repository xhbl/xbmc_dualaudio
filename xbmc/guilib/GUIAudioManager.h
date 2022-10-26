/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "GUIComponent.h"
#include "cores/AudioEngine/Interfaces/AESound.h"
#include "settings/lib/ISettingCallback.h"
#include "threads/CriticalSection.h"

#include <map>
#include <string>

// forward definitions
class CAction;
class CSettings;
class TiXmlNode;
class IAESound;

enum WINDOW_SOUND { SOUND_INIT = 0, SOUND_DEINIT };

class CGUIAudioManager : public ISettingCallback
{
  class CWindowSounds
  {
  public:
    IAESound *initSound;
    IAESound *deInitSound;
    IAESound *initSound2;
    IAESound *deInitSound2;
  };

  class CAPSounds
  {
  public:
    IAESound *sound;      
    IAESound *sound2;
  };

  class CSoundInfo
  {
  public:
    int usage;
    IAESound *sound;
    IAESound *sound2;
  };

public:
  CGUIAudioManager();
  ~CGUIAudioManager() override;

  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;
  bool OnSettingUpdate(const std::shared_ptr<CSetting>& setting,
                       const char* oldSettingId,
                       const TiXmlNode* oldSettingNode) override;

  void Initialize();
  void DeInitialize();

  bool Load();
  void UnLoad();


  void PlayActionSound(const CAction& action);
  void PlayWindowSound(int id, WINDOW_SOUND event);
  void PlayPythonSound(const std::string& strFileName, bool useCached = true);

  void CheckAudio2();
  void Enable(bool bEnable);
  void SetVolume(float level);
  void Stop();

private:
  // Construction parameters
  std::shared_ptr<CSettings> m_settings;

  typedef std::map<const std::string, CSoundInfo> soundCache;
  typedef std::map<int, CAPSounds              > actionSoundMap;
  typedef std::map<int, CWindowSounds          > windowSoundMap;
  typedef std::map<const std::string, CAPSounds > pythonSoundsMap;

  soundCache          m_soundCache;
  actionSoundMap      m_actionSoundMap;
  windowSoundMap      m_windowSoundMap;
  pythonSoundsMap     m_pythonSounds;

  std::string          m_strMediaDir;
  bool                m_bEnabled;
  bool                m_bAudio2;

  CCriticalSection    m_cs;

  CAPSounds LoadSound(const std::string &filename);
  void      FreeSound(IAESound *sound);
  void      FreeSoundAllUsage(IAESound *sound);
  CAPSounds LoadWindowSound(TiXmlNode* pWindowNode, const std::string& strIdentifier);
};

