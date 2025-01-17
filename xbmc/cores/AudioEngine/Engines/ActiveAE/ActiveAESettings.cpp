/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */


#include "ActiveAESettings.h"

#include "ServiceBroker.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAE.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "guilib/LocalizeStrings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/SettingDefinitions.h"
#include "settings/lib/SettingsManager.h"
#include "utils/StringUtils.h"

#include <mutex>

namespace ActiveAE
{

CActiveAESettings* CActiveAESettings::m_instance = nullptr;
CActiveAESettings* CActiveAESettings::m_instance2 = nullptr;

CActiveAESettings::CActiveAESettings(CActiveAE &ae) : m_audioEngine(ae)
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  std::unique_lock<CCriticalSection> lock(m_cs);
  if(m_audioEngine.IsAudio2())
    m_instance2 = this;
  else
    m_instance = this;

  std::set<std::string> settingSet;
  if(m_audioEngine.IsAudio2())
  {
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_ENABLED);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_CONFIG);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_SAMPLERATE);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_PASSTHROUGH);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_CHANNELS);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_PROCESSQUALITY);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_ATEMPOTHRESHOLD);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_GUISOUNDMODE);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_STEREOUPMIX);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_AC3PASSTHROUGH);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_AC3TRANSCODE);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_EAC3PASSTHROUGH);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_DTSPASSTHROUGH);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_TRUEHDPASSTHROUGH);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_DTSHDPASSTHROUGH);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_AUDIODEVICE);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_PASSTHROUGHDEVICE);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_STREAMSILENCE);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_STREAMNOISE);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_MAINTAINORIGINALVOLUME);
    settingSet.insert(CSettings::SETTING_AUDIOOUTPUT2_DTSHDCOREFALLBACK);
    settings->GetSettingsManager()->RegisterCallback(this, settingSet);
	  
    settings->GetSettingsManager()->RegisterSettingOptionsFiller("aequalitylevels2", SettingOptionsAudioQualityLevelsFiller);
    settings->GetSettingsManager()->RegisterSettingOptionsFiller("audiodevices2", SettingOptionsAudioDevicesFiller);
    settings->GetSettingsManager()->RegisterSettingOptionsFiller("audiodevicespassthrough2", SettingOptionsAudioDevicesPassthroughFiller);
    settings->GetSettingsManager()->RegisterSettingOptionsFiller("audiostreamsilence2", SettingOptionsAudioStreamsilenceFiller);
    return;
  }
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_CONFIG);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_SAMPLERATE);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_CHANNELS);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_PROCESSQUALITY);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_ATEMPOTHRESHOLD);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_GUISOUNDMODE);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_STEREOUPMIX);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_AC3PASSTHROUGH);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_AC3TRANSCODE);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGHDEVICE);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_STREAMSILENCE);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_STREAMNOISE);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_MAINTAINORIGINALVOLUME);
  settingSet.insert(CSettings::SETTING_AUDIOOUTPUT_DTSHDCOREFALLBACK);
  settings->GetSettingsManager()->RegisterCallback(this, settingSet);

  settings->GetSettingsManager()->RegisterSettingOptionsFiller("aequalitylevels", SettingOptionsAudioQualityLevelsFiller);
  settings->GetSettingsManager()->RegisterSettingOptionsFiller("audiodevices", SettingOptionsAudioDevicesFiller);
  settings->GetSettingsManager()->RegisterSettingOptionsFiller("audiodevicespassthrough", SettingOptionsAudioDevicesPassthroughFiller);
  settings->GetSettingsManager()->RegisterSettingOptionsFiller("audiostreamsilence", SettingOptionsAudioStreamsilenceFiller);
}

CActiveAESettings::~CActiveAESettings()
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  std::unique_lock<CCriticalSection> lock(m_cs);
  if(m_audioEngine.IsAudio2())
  {
    settings->GetSettingsManager()->UnregisterSettingOptionsFiller("aequalitylevels2");
    settings->GetSettingsManager()->UnregisterSettingOptionsFiller("audiodevices2");
    settings->GetSettingsManager()->UnregisterSettingOptionsFiller("audiodevicespassthrough2");
    settings->GetSettingsManager()->UnregisterSettingOptionsFiller("audiostreamsilence2");
    settings->GetSettingsManager()->UnregisterCallback(this);
    m_instance2 = nullptr;
	return;
  }
  settings->GetSettingsManager()->UnregisterSettingOptionsFiller("aequalitylevels");
  settings->GetSettingsManager()->UnregisterSettingOptionsFiller("audiodevices");
  settings->GetSettingsManager()->UnregisterSettingOptionsFiller("audiodevicespassthrough");
  settings->GetSettingsManager()->UnregisterSettingOptionsFiller("audiostreamsilence");
  settings->GetSettingsManager()->UnregisterCallback(this);
  m_instance = nullptr;
}

void CActiveAESettings::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  std::unique_lock<CCriticalSection> lock(m_cs);
  m_audioEngine.OnSettingsChange();
}

void CActiveAESettings::SettingOptionsAudioDevicesFiller(const SettingConstPtr& setting,
                                                         std::vector<StringSettingOption>& list,
                                                         std::string& current,
                                                         void* data)
{
  SettingOptionsAudioDevicesFillerGeneral(setting, list, current, false);
}

void CActiveAESettings::SettingOptionsAudioDevicesPassthroughFiller(
    const SettingConstPtr& setting,
    std::vector<StringSettingOption>& list,
    std::string& current,
    void* data)
{
  SettingOptionsAudioDevicesFillerGeneral(setting, list, current, true);
}

void CActiveAESettings::SettingOptionsAudioQualityLevelsFiller(
    const SettingConstPtr& setting,
    std::vector<IntegerSettingOption>& list,
    int& current,
    void* data)
{
  CActiveAESettings* c_instance = IsSettingAudio2(setting) ? m_instance2 : m_instance;

  std::unique_lock<CCriticalSection> lock(m_instance->m_cs);

  if (c_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_LOW))
    list.emplace_back(g_localizeStrings.Get(13506), AE_QUALITY_LOW);
  if (c_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_MID))
    list.emplace_back(g_localizeStrings.Get(13507), AE_QUALITY_MID);
  if (c_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_HIGH))
    list.emplace_back(g_localizeStrings.Get(13508), AE_QUALITY_HIGH);
  if (c_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_REALLYHIGH))
    list.emplace_back(g_localizeStrings.Get(13509), AE_QUALITY_REALLYHIGH);
  if (c_instance->m_audioEngine.SupportsQualityLevel(AE_QUALITY_GPU))
    list.emplace_back(g_localizeStrings.Get(38010), AE_QUALITY_GPU);
}

void CActiveAESettings::SettingOptionsAudioStreamsilenceFiller(
    const SettingConstPtr& setting,
    std::vector<IntegerSettingOption>& list,
    int& current,
    void* data)
{
  CActiveAESettings* c_instance = IsSettingAudio2(setting) ? m_instance2 : m_instance;

  std::unique_lock<CCriticalSection> lock(m_instance->m_cs);

  list.emplace_back(g_localizeStrings.Get(20422),
                    XbmcThreads::EndTime<std::chrono::minutes>::Max().count());
  list.emplace_back(g_localizeStrings.Get(13551), 0);

  if (c_instance->m_audioEngine.SupportsSilenceTimeout())
  {
    list.emplace_back(StringUtils::Format(g_localizeStrings.Get(13554), 1), 1);
    for (int i = 2; i <= 10; i++)
    {
      list.emplace_back(StringUtils::Format(g_localizeStrings.Get(13555), i), i);
    }
  }
}

bool CActiveAESettings::IsSettingVisible(const std::string& condition,
                                         const std::string& value,
                                         const SettingConstPtr& setting,
                                         void* data)
{
  if (setting == NULL || value.empty())
    return false;

  CActiveAESettings* c_instance = IsSettingAudio2(setting) ? m_instance2 : m_instance;

  std::unique_lock<CCriticalSection> lock(m_instance->m_cs);
  if (!c_instance)
    return false;

  return c_instance->m_audioEngine.IsSettingVisible(value);
}

void CActiveAESettings::SettingOptionsAudioDevicesFillerGeneral(
    const SettingConstPtr& setting,
    std::vector<StringSettingOption>& list,
    std::string& current,
    bool passthrough)
{
  current = std::static_pointer_cast<const CSettingString>(setting)->GetValue();
  std::string firstDevice;

  CActiveAESettings* c_instance = IsSettingAudio2(setting) ? m_instance2 : m_instance;

  std::unique_lock<CCriticalSection> lock(m_instance->m_cs);

  bool foundValue = false;
  AEDeviceList sinkList;
  c_instance->m_audioEngine.EnumerateOutputDevices(sinkList, passthrough);
  if (sinkList.empty())
    list.emplace_back("Error - no devices found", "error");
  else
  {
    for (AEDeviceList::const_iterator sink = sinkList.begin(); sink != sinkList.end(); ++sink)
    {
      if (sink == sinkList.begin())
        firstDevice = sink->second;

      list.emplace_back(sink->first, sink->second);

      if (StringUtils::EqualsNoCase(current, sink->second))
        foundValue = true;
    }
  }

  if (!foundValue)
    current = firstDevice;
}

bool CActiveAESettings::IsSettingAudio2(const SettingConstPtr& setting)
{
  return StringUtils::StartsWithNoCase(setting->GetId(), "audiooutput2.");
}
}
