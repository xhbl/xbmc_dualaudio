/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RetroPlayerAudio.h"

#include "ServiceBroker.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Interfaces/AEStream.h"
#include "cores/AudioEngine/Utils/AEChannelInfo.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/RetroPlayer/audio/AudioTranslator.h"
#include "cores/RetroPlayer/process/RPProcessInfo.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <cmath>

using namespace KODI;
using namespace RETRO;

const double MAX_DELAY = 0.3; // seconds

CRetroPlayerAudio::CRetroPlayerAudio(CRPProcessInfo& processInfo)
  : m_processInfo(processInfo), m_pAudioStream(nullptr), m_pAudioStream2(nullptr), m_bAudioEnabled(true)
{
  m_bAudio2 = false;
  CLog::Log(LOGDEBUG, "RetroPlayer[AUDIO]: Initializing audio");
}

CRetroPlayerAudio::~CRetroPlayerAudio()
{
  CLog::Log(LOGDEBUG, "RetroPlayer[AUDIO]: Deinitializing audio");

  CloseStream();
}

bool CRetroPlayerAudio::OpenStream(const StreamProperties& properties)
{
  m_bAudio2 = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_AUDIOOUTPUT2_ENABLED) ? true : false;

  const AudioStreamProperties& audioProperties =
      static_cast<const AudioStreamProperties&>(properties);

  const AEDataFormat pcmFormat = CAudioTranslator::TranslatePCMFormat(audioProperties.format);
  if (pcmFormat == AE_FMT_INVALID)
  {
    CLog::Log(LOGERROR, "RetroPlayer[AUDIO]: Unknown PCM format: {}",
              static_cast<int>(audioProperties.format));
    return false;
  }

  unsigned int iSampleRate = static_cast<unsigned int>(std::round(audioProperties.sampleRate));
  if (iSampleRate == 0)
  {
    CLog::Log(LOGERROR, "RetroPlayer[AUDIO]: Invalid samplerate: {:f}", audioProperties.sampleRate);
    return false;
  }

  CAEChannelInfo channelLayout;
  for (auto it = audioProperties.channelMap.begin(); it != audioProperties.channelMap.end(); ++it)
  {
    AEChannel channel = CAudioTranslator::TranslateAudioChannel(*it);
    if (channel == AE_CH_NULL)
      break;

    channelLayout += channel;
  }

  if (!channelLayout.IsLayoutValid())
  {
    CLog::Log(LOGERROR, "RetroPlayer[AUDIO]: Empty channel layout");
    return false;
  }

  if (m_pAudioStream != nullptr || m_pAudioStream2 != nullptr)
    CloseStream();

  IAE* audioEngine = CServiceBroker::GetActiveAE();
  if (audioEngine == nullptr)
    return false;
  IAE* audioEngine2 = CServiceBroker::GetActiveAE(true);

  CLog::Log(
      LOGINFO,
      "RetroPlayer[AUDIO]: Creating audio stream, format = {}, sample rate = {}, channels = {}",
      CAEUtil::DataFormatToStr(pcmFormat), iSampleRate, channelLayout.Count());

  AEAudioFormat audioFormat;
  audioFormat.m_dataFormat = pcmFormat;
  audioFormat.m_sampleRate = iSampleRate;
  audioFormat.m_channelLayout = channelLayout;
  m_pAudioStream = audioEngine->MakeStream(audioFormat);

  if (m_pAudioStream == nullptr)
  {
    CLog::Log(LOGERROR, "RetroPlayer[AUDIO]: Failed to create audio stream");
    return false;
  }

  if (m_bAudio2 && audioEngine2)
  {
    m_pAudioStream2 = audioEngine2->MakeStream(audioFormat);
    if (m_pAudioStream2 == nullptr)
      CLog::Log(LOGERROR, "RetroPlayer[AUDIO]: Failed to create 2nd audio stream");
  }

  m_processInfo.SetAudioChannels(audioFormat.m_channelLayout);
  m_processInfo.SetAudioSampleRate(audioFormat.m_sampleRate);
  m_processInfo.SetAudioBitsPerSample(CAEUtil::DataFormatToUsedBits(audioFormat.m_dataFormat));

  return true;
}

void CRetroPlayerAudio::AddStreamData(const StreamPacket& packet)
{
  const AudioStreamPacket& audioPacket = static_cast<const AudioStreamPacket&>(packet);

  if (m_bAudioEnabled)
  {
    if (m_pAudioStream)
    {
      const double delaySecs = m_pAudioStream->GetDelay();

      const size_t frameSize = m_pAudioStream->GetChannelCount() *
                               (CAEUtil::DataFormatToBits(m_pAudioStream->GetDataFormat()) >> 3);

      const unsigned int frameCount = static_cast<unsigned int>(audioPacket.size / frameSize);

      if (delaySecs > MAX_DELAY)
      {
        m_pAudioStream->Flush();
        CLog::Log(LOGDEBUG, "RetroPlayer[AUDIO]: Audio delay ({:0.2f} ms) is too high - flushing",
                  delaySecs * 1000);
      }

      m_pAudioStream->AddData(&audioPacket.data, 0, frameCount, nullptr);
    }
    if (m_bAudio2 && m_pAudioStream2)
    {
      const double delaySecs = m_pAudioStream2->GetDelay();

      const size_t frameSize = m_pAudioStream2->GetChannelCount() *
                               (CAEUtil::DataFormatToBits(m_pAudioStream2->GetDataFormat()) >> 3);

      const unsigned int frameCount = static_cast<unsigned int>(audioPacket.size / frameSize);

      if (delaySecs > MAX_DELAY)
      {
        m_pAudioStream2->Flush();
        CLog::Log(LOGDEBUG, "RetroPlayer[AUDIO]: Audio2 delay ({:0.2f} ms) is too high - flushing",
                  delaySecs * 1000);
      }

      if(!CServiceBroker::GetActiveAE(true)->IsDisabled())
        m_pAudioStream2->AddData(&audioPacket.data, 0, frameCount, nullptr);
    }
  }
}

void CRetroPlayerAudio::CloseStream()
{
  if (m_pAudioStream)
  {
    CLog::Log(LOGDEBUG, "RetroPlayer[AUDIO]: Closing audio stream");

    m_pAudioStream.reset();
    m_pAudioStream2.reset();
  }
}
