/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
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
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "system.h"
#include "AudioRendererFactory.h"
#include "GUISettings.h"
#include "log.h"
#include "NullDirectSound.h"

#ifdef HAS_PULSEAUDIO
#include "PulseAudioDirectSound.h"
#endif

#ifdef _WIN32
#include "Win32WASAPI.h"
#include "Win32DirectSound.h"
#endif
#ifdef __APPLE__
#include "CoreAudioRenderer.h"
#elif defined(_LINUX)
#include "ALSADirectSound.h"
#endif

#define ReturnOnValidInitialize(rendererName)    \
{                                                \
  if (audioSink->Initialize(pCallback, device, iChannels, channelMap, uiSamplesPerSec, uiBitsPerSample, bResample, bIsMusic, bPassthrough)) \
  {                                              \
    CLog::Log(LOGDEBUG, "%s::Initialize"         \
      " - Channels: %i"                          \
      " - SampleRate: %i"                        \
      " - SampleBit: %i"                         \
      " - Resample %s"                           \
      " - IsMusic %s"                            \
      " - IsPassthrough %s"                      \
      " - audioDevice: %s",                      \
      rendererName,                              \
      iChannels,                                 \
      uiSamplesPerSec,                           \
      uiBitsPerSample,                           \
      bResample ? "true" : "false",              \
      bIsMusic ? "true" : "false",               \
      bPassthrough ? "true" : "false",           \
      device.c_str()                             \
    ); \
    return audioSink;                      \
  }                                        \
  else                                     \
  {                                        \
    audioSink->Deinitialize();             \
    delete audioSink;                      \
    audioSink = NULL;                      \
  }                                        \
}

#define CreateAndReturnOnValidInitialize(rendererClass, bAudio2) \
{ \
  audioSink = new rendererClass(bAudio2); \
  ReturnOnValidInitialize(#rendererClass); \
}

#define ReturnNewRenderer(rendererClass, bAudio2) \
{ \
  renderer = #rendererClass; \
  return new rendererClass(bAudio2); \
}

IAudioRenderer* CAudioRendererFactory::Create(IAudioCallback* pCallback, int iChannels, enum PCMChannels *channelMap, unsigned int uiSamplesPerSec, unsigned int uiBitsPerSample, bool bResample, bool bIsMusic, bool bPassthrough, bool bAudio2)
{
  IAudioRenderer* audioSink = NULL;
  CStdString renderer;

  CStdString deviceString, device;
  if (!bAudio2)
  {
    if (bPassthrough)
    {
  #if defined(_LINUX) && !defined(__APPLE__)
      deviceString = g_guiSettings.GetString("audiooutput.passthroughdevice");
      if (deviceString.Equals("custom"))
        deviceString = g_guiSettings.GetString("audiooutput.custompassthrough");
  #else
      // osx/win platforms do not have an "audiooutput.passthroughdevice" setting but can do passthrough
      deviceString = g_guiSettings.GetString("audiooutput.audiodevice");
  #endif
    }
    else
    {
      deviceString = g_guiSettings.GetString("audiooutput.audiodevice");
      if (deviceString.Equals("custom"))
        deviceString = g_guiSettings.GetString("audiooutput.customdevice");
    }
  }
  else
  {
    if (bPassthrough)
    {
#if defined(_LINUX) && !defined(__APPLE__)
      deviceString = g_guiSettings.GetString("audiooutput2.passthroughdevice");
      if (deviceString.Equals("custom"))
        deviceString = g_guiSettings.GetString("audiooutput2.custompassthrough");
#else
      // osx/win platforms do not have an "audiooutput.passthroughdevice" setting but can do passthrough
      deviceString = g_guiSettings.GetString("audiooutput2.audiodevice");
#endif
    }
    else
    {
      deviceString = g_guiSettings.GetString("audiooutput2.audiodevice");
      if (deviceString.Equals("custom"))
        deviceString = g_guiSettings.GetString("audiooutput2.customdevice");
    }
  }
  int iPos = deviceString.Find(":");
  if (iPos > 0)
  {
    audioSink = CreateFromUri(deviceString.Left(iPos), renderer, bAudio2);
    if (audioSink)
    {
      device = deviceString.Right(deviceString.length() - iPos - 1);
      ReturnOnValidInitialize(renderer.c_str());

#ifdef _WIN32
      //If WASAPI failed try DirectSound.
      if(deviceString.Left(iPos).Equals("wasapi"))
      {
        audioSink = CreateFromUri("directsound", renderer, bAudio2);
        ReturnOnValidInitialize(renderer.c_str());
      }
#endif

      CreateAndReturnOnValidInitialize(CNullDirectSound, bAudio2);
      /* should never get here */
      assert(false);
    }
  }
  CLog::Log(LOGINFO, "AudioRendererFactory: %s not a explicit device, trying to autodetect.", device.c_str());

  device = deviceString;

/* First pass creation */
#ifdef HAS_PULSEAUDIO
  CreateAndReturnOnValidInitialize(CPulseAudioDirectSound, bAudio2);
#endif

/* incase none in the first pass was able to be created, fall back to os specific */
#ifdef WIN32
  CreateAndReturnOnValidInitialize(CWin32DirectSound, bAudio2);
#endif
#ifdef __APPLE__
  CreateAndReturnOnValidInitialize(CCoreAudioRenderer, bAudio2);
#elif defined(_LINUX)
  CreateAndReturnOnValidInitialize(CALSADirectSound, bAudio2);
#endif

  CreateAndReturnOnValidInitialize(CNullDirectSound, bAudio2);
  /* should never get here */
  assert(false);
  return NULL;
}

void CAudioRendererFactory::EnumerateAudioSinks(AudioSinkList& vAudioSinks, bool passthrough)
{
#ifdef HAS_PULSEAUDIO
  CPulseAudioDirectSound::EnumerateAudioSinks(vAudioSinks, passthrough);
  if (vAudioSinks.size() > 0)
    return;
#endif

#ifdef WIN32
  CWin32DirectSound::EnumerateAudioSinks(vAudioSinks, passthrough);
  CWin32WASAPI::EnumerateAudioSinks(vAudioSinks, passthrough);
#endif

#ifdef __APPLE__
  CCoreAudioRenderer::EnumerateAudioSinks(vAudioSinks, passthrough);
#elif defined(_LINUX)
  CALSADirectSound::EnumerateAudioSinks(vAudioSinks, passthrough);
#endif
}

IAudioRenderer *CAudioRendererFactory::CreateFromUri(const CStdString &soundsystem, CStdString &renderer, bool bAudio2)
{
#ifdef HAS_PULSEAUDIO
  if (soundsystem.Equals("pulse"))
    ReturnNewRenderer(CPulseAudioDirectSound, bAudio2);
#endif

#ifdef WIN32
  if (soundsystem.Equals("wasapi"))
    ReturnNewRenderer(CWin32WASAPI, bAudio2)
  else if (soundsystem.Equals("directsound"))
    ReturnNewRenderer(CWin32DirectSound, bAudio2);
#endif

#ifdef __APPLE__
  if (soundsystem.Equals("coreaudio"))
    ReturnNewRenderer(CCoreAudioRenderer, bAudio2);
#elif defined(_LINUX)
  if (soundsystem.Equals("alsa"))
    ReturnNewRenderer(CALSADirectSound, bAudio2);
#endif

  if (soundsystem.Equals("null"))
    ReturnNewRenderer(CNullDirectSound, bAudio2);

  return NULL;
}
