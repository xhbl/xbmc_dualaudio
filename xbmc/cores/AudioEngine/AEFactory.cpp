/*
 *      Copyright (C) 2010-2012 Team XBMC
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

#include "AEFactory.h"

#if defined(TARGET_DARWIN)
  #include "Engines/CoreAudio/CoreAudioAE.h"
#else
  #include "Engines/SoftAE/SoftAE.h"
#endif

#if defined(HAS_PULSEAUDIO)
  #include "Engines/PulseAE/PulseAE.h"
#endif

IAE* CAEFactory::AE = NULL;
IAE* CAEFactory::AE2 = NULL;
static float  g_fVolume = 1.0f;
static bool   g_bMute = false;

IAE *CAEFactory::GetEngine(bool bAudio2)
{
  if(!bAudio2)
    return AE;
  else
    return AE2;
}

bool CAEFactory::LoadEngine()
{
#if defined(TARGET_RASPBERRY_PI)
  return true;
#endif

  bool loaded = false;

  std::string engine;

#if defined(TARGET_LINUX)
  if (getenv("AE_ENGINE"))
  {
    engine = (std::string)getenv("AE_ENGINE");
    std::transform(engine.begin(), engine.end(), engine.begin(), ::toupper);

    #if defined(HAS_PULSEAUDIO)
    if (!loaded && engine == "PULSE")
      loaded = CAEFactory::LoadEngine(AE_ENGINE_PULSE);
    #endif
    if (!loaded && engine == "SOFT" )
      loaded = CAEFactory::LoadEngine(AE_ENGINE_SOFT);
  }
#endif

#if defined(HAS_PULSEAUDIO)
  if (!loaded)
    loaded = CAEFactory::LoadEngine(AE_ENGINE_PULSE);
#endif

#if defined(TARGET_DARWIN)
  if (!loaded)
    loaded = CAEFactory::LoadEngine(AE_ENGINE_COREAUDIO);
#else
  if (!loaded)
    loaded = CAEFactory::LoadEngine(AE_ENGINE_SOFT);
#endif

  return loaded;
}

bool CAEFactory::LoadEngine(enum AEEngine engine)
{
  /* can only load the engine once, XBMC restart is required to change it */
  if (AE)
    return false;

  switch(engine)
  {
    case AE_ENGINE_NULL     :
#if defined(TARGET_DARWIN)
    case AE_ENGINE_COREAUDIO: AE = new CCoreAudioAE(); break;
#else
    case AE_ENGINE_SOFT     : AE = new CSoftAE(); break;
#endif
#if defined(HAS_PULSEAUDIO)
    case AE_ENGINE_PULSE    : AE = new CPulseAE(); break;
#endif

    default:
      return false;
  }

  if (AE && !AE->CanInit())
  {
    delete AE;
    AE = NULL;
  }

  if (!AE2)
  {
    switch(engine)
    {
      case AE_ENGINE_NULL	  :
#if defined(TARGET_DARWIN)
      case AE_ENGINE_COREAUDIO: AE2 = new CCoreAudioAE(); break;
#else
      case AE_ENGINE_SOFT	  : AE2 = new CSoftAE(); break;
#endif
#if defined(HAS_PULSEAUDIO)
      case AE_ENGINE_PULSE	  : AE2 = new CPulseAE(); break;
#endif
  
      default: break;
    }
  
    if (AE2)
        AE2->SetAudio2(true);
    if (AE2 && !AE2->CanInit())
    {
      delete AE2;
      AE2 = NULL;
    }
  }

  return AE != NULL;
}

void CAEFactory::UnLoadEngine()
{
  if(AE)
  {
    AE->Shutdown();
    delete AE;
    AE = NULL;
  }
  if(AE2)
  {
    AE2->Shutdown();
    delete AE2;
    AE2 = NULL;
  }
}

bool CAEFactory::StartEngine()
{
#if defined(TARGET_RASPBERRY_PI)
  return true;
#endif

  if (!AE)
    return false;

  if (AE->Initialize())
  {
    if (AE2)
    {
      if(!AE2->Initialize())
      {
        delete AE2;
        AE2 = NULL;
      }
    }
    return true;
  }

  delete AE;
  AE = NULL;
  return false;
}

bool CAEFactory::Suspend()
{
  bool bRet = false;
  if(AE)
    bRet = AE->Suspend();
  if (AE2)
    AE2->Suspend();

  return bRet;
}

bool CAEFactory::Resume()
{
  bool bRet = false;
  if(AE)
    bRet = AE->Resume();
  if (AE2)
    AE2->Resume();

  return bRet;
}

bool CAEFactory::IsSuspended()
{
  if(AE)
    return AE->IsSuspended();

  /* No engine to process audio */
  return true;
}

/* engine wrapping */
IAESound *CAEFactory::MakeSound(const std::string &file, bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->MakeSound(file);
  if(bAudio2 && AE2)
    return AE2->MakeSound(file);
  
  return NULL;
}

void CAEFactory::FreeSound(IAESound *sound)
{
  if(!sound)
    return;
  bool bAudio2 = sound->IsAudio2();

  if(!bAudio2 && AE)
    AE->FreeSound(sound);
  if(bAudio2 && AE2)
    AE2->FreeSound(sound);
}

void CAEFactory::SetSoundMode(const int mode, bool bAudio2)
{
  if(!bAudio2 && AE)
    AE->SetSoundMode(mode);
  if(bAudio2 && AE2)
    AE2->SetSoundMode(mode);
}

void CAEFactory::OnSettingsChange(std::string setting, bool bAudio2)
{
  if(!bAudio2 && AE)
    AE->OnSettingsChange(setting);
  if(bAudio2 && AE2)
    AE2->OnSettingsChange(setting);
}

void CAEFactory::EnumerateOutputDevices(AEDeviceList &devices, bool passthrough, bool bAudio2)
{
  if(!bAudio2 && AE)
    AE->EnumerateOutputDevices(devices, passthrough);
  if(bAudio2 && AE2)
    AE2->EnumerateOutputDevices(devices, passthrough);
}

void CAEFactory::VerifyOutputDevice(std::string &device, bool passthrough)
{
  AEDeviceList devices;
  EnumerateOutputDevices(devices, passthrough);
  std::string firstDevice;

  for (AEDeviceList::const_iterator deviceIt = devices.begin(); deviceIt != devices.end(); deviceIt++)
  {
    std::string currentDevice = deviceIt->second;
    /* remember the first device so we can default to it if required */
    if (firstDevice.empty())
      firstDevice = deviceIt->second;

    if (deviceIt->second == device)
      return;
    else if (deviceIt->first == device)
    {
      device = deviceIt->second;
      return;
    }
  }

  /* if the device wasnt found, set it to the first viable output */
  device = firstDevice;
}

std::string CAEFactory::GetDefaultDevice(bool passthrough, bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->GetDefaultDevice(passthrough);
  if(bAudio2 && AE2)
    return AE2->GetDefaultDevice(passthrough);

  return "default";
}

std::string CAEFactory::GetCreateDevice(bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->GetCreateDevice();
  if(bAudio2 && AE2)
    return AE2->GetCreateDevice();

  return "";
}

bool CAEFactory::SupportsRaw(bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->SupportsRaw();
  if(bAudio2 && AE2)
    return AE2->SupportsRaw();

  return false;
}

void CAEFactory::SetMute(const bool enabled)
{
  if(AE)
    AE->SetMute(enabled);
  if(AE2)
    AE2->SetMute(enabled);

  g_bMute = enabled;
}

bool CAEFactory::IsMuted()
{
  if(AE)
    return AE->IsMuted();

  return g_bMute || (g_fVolume == 0.0f);
}

bool CAEFactory::IsDumb(bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->IsDumb();
  if(bAudio2 && AE2)
    return AE2->IsDumb();

  return true;
}

float CAEFactory::GetVolume()
{
  if(AE)
    return AE->GetVolume();

  return g_fVolume;
}

void CAEFactory::SetVolume(const float volume)
{
  if(AE)
  {
    AE->SetVolume(volume);
    AE2->SetVolume(volume);
  }
  else
    g_fVolume = volume;
}

void CAEFactory::Shutdown()
{
  if(AE)
    AE->Shutdown();
  if(AE2)
    AE2->Shutdown();
}

IAEStream *CAEFactory::MakeStream(enum AEDataFormat dataFormat, unsigned int sampleRate, 
  unsigned int encodedSampleRate, CAEChannelInfo channelLayout, unsigned int options, bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->MakeStream(dataFormat, sampleRate, encodedSampleRate, channelLayout, options);
  if(bAudio2 && AE2)
    return AE2->MakeStream(dataFormat, sampleRate, encodedSampleRate, channelLayout, options);

  return NULL;
}

IAEStream *CAEFactory::FreeStream(IAEStream *stream)
{
  if(!stream)
    return NULL;
  bool bAudio2 = stream->IsAudio2();

  if(!bAudio2 && AE)
    return AE->FreeStream(stream);
  if(bAudio2 && AE2)
    return AE2->FreeStream(stream);

  return NULL;
}

void CAEFactory::GarbageCollect()
{
  if(AE)
    AE->GarbageCollect();
  if(AE2)
    AE2->GarbageCollect();
}

bool CAEFactory::IsDualAudioBetaExpired()
{
#if 1
  struct tm expired_date;
  memset(&expired_date, 0, sizeof(expired_date));

  expired_date.tm_year = 2014; // beta test expired date set here
  expired_date.tm_mon = 5;
  expired_date.tm_mday = 31;

  expired_date.tm_year-=1900;
  expired_date.tm_mon--;
  time_t expired_time = mktime(&expired_date);
  time_t current_time = time(NULL);
  if(current_time > expired_time)
  	return true;
#endif
  return false;
}
