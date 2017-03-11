#pragma once

/*
*      Copyright (C) 2005-2014 Team XBMC
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

#include <atomic>
#include <string>
#include "threads/CriticalSection.h"

class CDataCacheCore
{
public:
  CDataCacheCore();
  static CDataCacheCore& GetInstance();
  bool HasAVInfoChanges();
  void SignalVideoInfoChange();
  void SignalAudioInfoChange();

  // player video info
  void SetVideoDecoderName(std::string name, bool isHw);
  std::string GetVideoDecoderName();
  bool IsVideoHwDecoder();
  void SetVideoDeintMethod(std::string method);
  std::string GetVideoDeintMethod();
  void SetVideoPixelFormat(std::string pixFormat);
  std::string GetVideoPixelFormat();
  void SetVideoDimensions(int width, int height);
  int GetVideoWidth();
  int GetVideoHeight();
  void SetVideoFps(float fps);
  float GetVideoFps();
  void SetVideoDAR(float dar);
  float GetVideoDAR();

  // player audio info
  void SetAudioDecoderName(std::string name, bool bAudio2 = false);
  std::string GetAudioDecoderName(bool bAudio2 = false);
  void SetAudioChannels(std::string channels, bool bAudio2 = false);
  std::string GetAudioChannels(bool bAudio2 = false);
  void SetAudioSampleRate(int sampleRate, bool bAudio2 = false);
  int GetAudioSampleRate(bool bAudio2 = false);
  void SetAudioBitsPerSample(int bitsPerSample, bool bAudio2 = false);
  int GetAudioBitsPerSample(bool bAudio2 = false);

  // render info
  void SetRenderClockSync(bool enabled);
  bool IsRenderClockSync();

  // player states
  void SetStateSeeking(bool active);
  bool IsSeeking();

protected:
  std::atomic_bool m_hasAVInfoChanges;

  CCriticalSection m_videoPlayerSection;
  struct SPlayerVideoInfo
  {
    std::string decoderName;
    bool isHwDecoder;
    std::string deintMethod;
    std::string pixFormat;
    int width;
    int height;
    float fps;
    float dar;
  } m_playerVideoInfo;

  CCriticalSection m_audioPlayerSection, m_audio2PlayerSection;
  struct SPlayerAudioInfo
  {
    std::string decoderName;
    std::string channels;
    int sampleRate;
    int bitsPerSample;
  } m_playerAudioInfo, m_playerAudio2Info;

  CCriticalSection m_renderSection;
  struct SRenderInfo
  {
    bool m_isClockSync;
  } m_renderInfo;

  CCriticalSection m_stateSection;
  struct SStateInfo
  {
    bool m_stateSeeking;
  } m_stateInfo;
};
