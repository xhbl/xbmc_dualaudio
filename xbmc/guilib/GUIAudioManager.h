#pragma once

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

#include <map>
#include <string>

#include "cores/AudioEngine/Interfaces/AESound.h"
#include "settings/lib/ISettingCallback.h"
#include "threads/CriticalSection.h"

// forward definitions
class CAction;
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
  ~CGUIAudioManager();

  virtual void OnSettingChanged(const CSetting *setting);
  virtual bool OnSettingUpdate(CSetting* &setting, const char *oldSettingId, const TiXmlNode *oldSettingNode);

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

extern CGUIAudioManager g_audioManager;
