/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerAudio.h"

#include "DVDCodecs/Audio/DVDAudioCodec.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/SingleLock.h"
#include "utils/MathUtils.h"
#include "utils/log.h"

#include "system.h"
#ifdef TARGET_RASPBERRY_PI
#include "platform/linux/RBP.h"
#endif

#include <sstream>
#include <iomanip>
#include <math.h>

class CDVDMsgAudioCodecChange : public CDVDMsg
{
public:
  CDVDMsgAudioCodecChange(const CDVDStreamInfo &hints, CDVDAudioCodec* codec, CDVDAudioCodec* codec2)
    : CDVDMsg(GENERAL_STREAMCHANGE)
    , m_codec(codec)
    , m_codec2(codec2)
    , m_hints(hints)
  {}
 ~CDVDMsgAudioCodecChange() override
  {
    if (m_codec)
      delete m_codec;
    if (m_codec2)
      delete m_codec2;
  }
  CDVDAudioCodec* m_codec;
  CDVDAudioCodec* m_codec2;
  CDVDStreamInfo  m_hints;
};


CVideoPlayerAudio::CVideoPlayerAudio(CDVDClock* pClock, CDVDMessageQueue& parent, CProcessInfo &processInfo)
: CThread("VideoPlayerAudio"), IDVDStreamPlayerAudio(processInfo)
, m_messageQueue("audio")
, m_messageParent(parent)
, m_audioSink(pClock)
, m_audioSink2(pClock, true)
{
  m_pClock = pClock;
  m_pAudioCodec = NULL;
  m_pAudioCodec2 = NULL;
  m_bAudio2 = false;
  m_bAudio2Skip = false;
  m_audioClock = 0;
  m_speed = DVD_PLAYSPEED_NORMAL;
  m_stalled = true;
  m_paused = false;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_synctype = SYNC_DISCON;
  m_prevsynctype = -1;
  m_prevskipped = false;
  m_maxspeedadjust = 0.0;

  m_messageQueue.SetMaxDataSize(6 * 1024 * 1024);
  m_messageQueue.SetMaxTimeSize(8.0);
}

CVideoPlayerAudio::~CVideoPlayerAudio()
{
  StopThread();

  // close the stream, and don't wait for the audio to be finished
  // CloseStream(true);
}

bool CVideoPlayerAudio::OpenStream(CDVDStreamInfo hints)
{
  m_bAudio2 = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_AUDIOOUTPUT2_ENABLED) ? true : false;

  CLog::Log(LOGINFO, "Finding audio codec for: %i", hints.codec);
  bool allowpassthrough = !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (m_processInfo.IsRealtimeStream())
    allowpassthrough = false;

  CAEStreamInfo::DataType streamType =
      m_audioSink.GetPassthroughStreamType(hints.codec, hints.samplerate, hints.profile);
  CDVDAudioCodec* codec = CDVDFactoryCodec::CreateAudioCodec(hints, m_processInfo,
                                                             allowpassthrough, m_processInfo.AllowDTSHDDecode(),
                                                             streamType);
  if(!codec)
  {
    CLog::Log(LOGERROR, "Unsupported audio codec");
    return false;
  }
  CDVDAudioCodec* codec2 = NULL;
  if (m_bAudio2)
  {
    CAEStreamInfo::DataType streamType2 =
        m_audioSink2.GetPassthroughStreamType(hints.codec, hints.samplerate, hints.profile);
    codec2 = CDVDFactoryCodec::CreateAudioCodec(hints, m_processInfo,
                                                allowpassthrough, m_processInfo.AllowDTSHDDecode(),
                                                streamType2, m_bAudio2);
    if( !codec2 )
    {
      CLog::Log(LOGERROR, "Unsupported 2nd audio codec");
      m_audioSink2.Destroy(true);
      m_bAudio2 = false;
    }
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(new CDVDMsgAudioCodecChange(hints, codec, codec2), 0);
  else
  {
    OpenStream(hints, codec, codec2);
    m_messageQueue.Init();
    CLog::Log(LOGINFO, "Creating audio thread");
    Create();
  }
  return true;
}

void CVideoPlayerAudio::OpenStream(CDVDStreamInfo &hints, CDVDAudioCodec* codec, CDVDAudioCodec* codec2)
{
  if (m_pAudioCodec)
    SAFE_DELETE(m_pAudioCodec);
  m_pAudioCodec = codec;

  if (m_pAudioCodec2)
    SAFE_DELETE(m_pAudioCodec2);
  m_pAudioCodec2 = codec2;

  m_processInfo.ResetAudioCodecInfo();

  /* store our stream hints */
  m_streaminfo = hints;

  /* update codec information from what codec gave out, if any */
  int channelsFromCodec   = m_pAudioCodec->GetFormat().m_channelLayout.Count();
  int samplerateFromCodec = m_pAudioCodec->GetFormat().m_sampleRate;

  if (channelsFromCodec > 0)
    m_streaminfo.channels = channelsFromCodec;
  if (samplerateFromCodec > 0)
    m_streaminfo.samplerate = samplerateFromCodec;

  /* check if we only just got sample rate, in which case the previous call
   * to CreateAudioCodec() couldn't have started passthrough */
  if (hints.samplerate != m_streaminfo.samplerate)
    SwitchCodecIfNeeded();

  m_audioClock = 0;
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;

  m_prevsynctype = -1;
  m_synctype = SYNC_DISCON;
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK))
    m_synctype = SYNC_RESAMPLE;
  else if (m_processInfo.IsRealtimeStream())
    m_synctype = SYNC_RESAMPLE;

  m_prevskipped = false;

  m_maxspeedadjust = 5.0;

  m_messageParent.Put(new CDVDMsg(CDVDMsg::PLAYER_AVCHANGE));
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
}

void CVideoPlayerAudio::CloseStream(bool bWaitForBuffers)
{
  bool bWait = bWaitForBuffers && m_speed > 0 && !CServiceBroker::GetActiveAE()->IsSuspended();

  // wait until buffers are empty
  if (bWait)
    m_messageQueue.WaitUntilEmpty();

  // send abort message to the audio queue
  m_messageQueue.Abort();

  CLog::Log(LOGINFO, "Waiting for audio thread to exit");

  // shut down the adio_decode thread and wait for it
  StopThread(); // will set this->m_bStop to true

  // destroy audio device
  CLog::Log(LOGINFO, "Closing audio device");
  if (bWait)
  {
    m_bStop = false;
    m_audioSink.Drain();
    if (m_bAudio2)
      m_audioSink2.Drain();
    m_bStop = true;
  }
  else
  {
    m_audioSink.Flush();
    if (m_bAudio2)
      m_audioSink2.Flush();
  }

  m_audioSink.Destroy(true);
  if (m_bAudio2)
    m_audioSink2.Destroy(true);

  // uninit queue
  m_messageQueue.End();

  CLog::Log(LOGINFO, "Deleting audio codec");
  if (m_pAudioCodec)
  {
    m_pAudioCodec->Dispose();
    delete m_pAudioCodec;
    m_pAudioCodec = NULL;
  }
  if (m_pAudioCodec2)
  {
    m_pAudioCodec2->Dispose();
    delete m_pAudioCodec2;
    m_pAudioCodec2 = NULL;
  }

  m_bAudio2 = false;
}

void CVideoPlayerAudio::OnStartup()
{
}

void CVideoPlayerAudio::UpdatePlayerInfo()
{
  std::ostringstream s;
  s << "aq:"     << std::setw(2) << std::min(99,m_messageQueue.GetLevel()) << "%";
  s << ", Kb/s:" << std::fixed << std::setprecision(2) << m_audioStats.GetBitrate() / 1024.0;

  //print the inverse of the resample ratio, since that makes more sense
  //if the resample ratio is 0.5, then we're playing twice as fast
  if (m_synctype == SYNC_RESAMPLE)
    s << ", rr:" << std::fixed << std::setprecision(5) << 1.0 / m_audioSink.GetResampleRatio();

  if (m_bAudio2)
    s << ", a1/a2:" << std::fixed << std::setprecision(3) << m_audiodiff;

  SInfo info;
  info.info        = s.str();
  info.pts         = m_audioSink.GetPlayingPts();
  info.passthrough = m_pAudioCodec && m_pAudioCodec->NeedPassthrough() && (!m_bAudio2 || (m_pAudioCodec2 && m_pAudioCodec2->NeedPassthrough()));

  { CSingleLock lock(m_info_section);
    m_info = info;
  }
}

void CVideoPlayerAudio::Process()
{
  CLog::Log(LOGINFO, "running thread: CVideoPlayerAudio::Process()");

  DVDAudioFrame audioframe;
  audioframe.nb_frames = 0;
  audioframe.framesOut = 0;
  DVDAudioFrame audioframe2;
  audioframe2.nb_frames = 0;
  audioframe2.framesOut = 0;
  m_audioStats.Start();
  m_audiodiff = 0.0;
  m_bAudio2Skip = false;

  bool onlyPrioMsgs = false;

  while (!m_bStop)
  {
    CDVDMsg* pMsg;
    int timeout = (int)(1000 * m_audioSink.GetCacheTime());

    // read next packet and return -1 on error
    int priority = 1;
    //Do we want a new audio frame?
    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING ||              /* when not started */
        m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) ||
        m_speed <  DVD_PLAYSPEED_PAUSE  || /* when rewinding */
        (m_speed >  DVD_PLAYSPEED_NORMAL && m_audioClock < m_pClock->GetClock())) /* when behind clock in ff */
      priority = 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      priority = 1;

    if (m_paused)
      priority = 1;

    if (onlyPrioMsgs)
    {
      priority = 1;
      timeout = 0;
    }

    MsgQueueReturnCode ret = m_messageQueue.Get(&pMsg, timeout, priority);

    onlyPrioMsgs = false;

    if (MSGQ_IS_ERROR(ret))
    {
      CLog::Log(LOGERROR, "Got MSGQ_ABORT or MSGO_IS_ERROR return true");
      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      if (ProcessDecoderOutput(audioframe, audioframe2))
      {
        onlyPrioMsgs = true;
        continue;
      }

      // if we only wanted priority messages, this isn't a stall
      if (priority)
        continue;

      if (m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) &&
          !m_stalled && m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        // while AE sync is active, we still have time to fill buffers
        if (m_syncTimer.IsTimePast())
        {
          CLog::Log(LOGINFO, "CVideoPlayerAudio::Process - stream stalled");
          m_stalled = true;
        }
      }
      if (timeout == 0)
        CThread::Sleep(10);

      continue;
    }

    // handle messages
    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (static_cast<CDVDMsgGeneralSynchronize*>(pMsg)->Wait(100, SYNCSOURCE_AUDIO))
        CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_SYNCHRONIZE");
      else
        m_messageQueue.Put(pMsg->Acquire(), 1);  // push back as prio message, to process other prio messages
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    { //player asked us to set internal clock
      double pts = static_cast<CDVDMsgDouble*>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_RESYNC(%f), level: %d, cache: %f",
                pts, m_messageQueue.GetLevel(), m_audioSink.GetDelay());

      double delay = m_audioSink.GetDelay();
      if (pts > m_audioClock - delay + 0.5 * DVD_TIME_BASE)
      {
        m_audioSink.Flush();
        if (m_bAudio2)
          m_audioSink2.Flush();
      }
      m_audioClock = pts + delay;
      if (m_speed != DVD_PLAYSPEED_PAUSE)
      {
        m_audioSink.Resume();
        if (m_bAudio2)
          m_audioSink2.Resume();
      }
      m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_syncTimer.Set(3000);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      if (m_pAudioCodec2)
        m_pAudioCodec2->Reset();
      m_audioSink.Flush();
      if (m_bAudio2)
        m_audioSink2.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;
      audioframe2.nb_frames = 0;
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      bool sync = static_cast<CDVDMsgBool*>(pMsg)->m_value;
      m_audioSink.Flush();
      if (m_bAudio2)
        m_audioSink2.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;
      audioframe2.nb_frames = 0;

      if (sync)
      {
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        m_audioSink.Pause();
        if (m_bAudio2)
          m_audioSink2.Pause();
      }

      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      if (m_pAudioCodec2)
        m_pAudioCodec2->Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_EOF))
    {
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_EOF");
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      double speed = static_cast<CDVDMsgInt*>(pMsg)->m_value;

      if (m_processInfo.IsTempoAllowed(static_cast<float>(speed)/DVD_PLAYSPEED_NORMAL))
      {
        if (speed != m_speed)
        {
          if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
          {
            m_audioSink.Resume();
            if (m_bAudio2)
              m_audioSink2.Resume();
            m_stalled = false;
          }
        }
      }
      else
      {
        m_audioSink.Pause();
        if (m_bAudio2)
          m_audioSink2.Pause();
      }
      m_speed = (int)speed;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      CDVDMsgAudioCodecChange* msg(static_cast<CDVDMsgAudioCodecChange*>(pMsg));
      OpenStream(msg->m_hints, msg->m_codec, msg->m_codec2);
      msg->m_codec = NULL;
      msg->m_codec2 = NULL;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_PAUSE))
    {
      m_paused = static_cast<CDVDMsgBool*>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_PAUSE: %d", m_paused);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_REQUEST_STATE))
    {
      SStateMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.syncState = m_syncState;
      m_messageParent.Put(new CDVDMsgType<SStateMsg>(CDVDMsg::PLAYER_REPORT_STATE, msg));
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = static_cast<CDVDMsgDemuxerPacket*>(pMsg)->GetPacket();
      bool bPacketDrop  = static_cast<CDVDMsgDemuxerPacket*>(pMsg)->GetPacketDrop();

      if (bPacketDrop)
      {
        pMsg->Release();
        if (m_syncState != IDVDStreamPlayer::SYNC_STARTING)
        {
          m_audioSink.Drain();
          m_audioSink.Flush();
          audioframe.nb_frames = 0;
          if (m_bAudio2)
          {
            m_audioSink2.Drain();
            m_audioSink2.Flush();
            audioframe2.nb_frames = 0;
          }
        }
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        continue;
      }

      if (!m_processInfo.IsTempoAllowed(static_cast<float>(m_speed) / DVD_PLAYSPEED_NORMAL) &&
          m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        pMsg->Release();
        continue;
      }

      if (!m_pAudioCodec->AddData(*pPacket))
      {
        m_messageQueue.PutBack(pMsg->Acquire());
        onlyPrioMsgs = true;
        pMsg->Release();
        continue;
      }

      if (m_bAudio2)
        m_pAudioCodec2->AddData(*pPacket);

      m_audioStats.AddSampleBytes(pPacket->iSize);
      UpdatePlayerInfo();

      if (ProcessDecoderOutput(audioframe, audioframe2))
      {
        onlyPrioMsgs = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_DISPLAY_RESET))
    {
      m_displayReset = true;
    }

    pMsg->Release();
  }
}

bool CVideoPlayerAudio::ProcessDecoderOutput(DVDAudioFrame &audioframe, DVDAudioFrame &audioframe2)
{
  if (audioframe.nb_frames <= audioframe.framesOut)
  {
    audioframe.hasDownmix = false;

    m_pAudioCodec->GetData(audioframe);

    if (audioframe.nb_frames == 0)
    {
      if(m_bAudio2)
        return ProcessDecoderOutput2(audioframe2);
      return false;
    }

    audioframe.hasTimestamp = true;
    if (audioframe.pts == DVD_NOPTS_VALUE)
    {
      audioframe.pts = m_audioClock;
      audioframe.hasTimestamp = false;
    }
    else
    {
      m_audioClock = audioframe.pts;
    }

    if (audioframe.format.m_sampleRate && m_streaminfo.samplerate != (int) audioframe.format.m_sampleRate)
    {
      // The sample rate has changed or we just got it for the first time
      // for this stream. See if we should enable/disable passthrough due
      // to it.
      m_streaminfo.samplerate = audioframe.format.m_sampleRate;
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // if stream switches to realtime, disable pass through
    // or switch to resample
    if (m_processInfo.IsRealtimeStream() && m_synctype != SYNC_RESAMPLE)
    {
      m_synctype = SYNC_RESAMPLE;
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // Display reset event has occurred
    // See if we should enable passthrough
    if (m_displayReset)
    {
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // demuxer reads metatags that influence channel layout
    if (m_streaminfo.codec == AV_CODEC_ID_FLAC && m_streaminfo.channellayout)
      audioframe.format.m_channelLayout = CAEUtil::GetAEChannelLayout(m_streaminfo.channellayout);

    // we have successfully decoded an audio frame, setup renderer to match
    if (!m_audioSink.IsValidFormat(audioframe))
    {
      if (m_speed)
        m_audioSink.Drain();

      m_audioSink.Destroy(false);

      if (!m_audioSink.Create(audioframe, m_streaminfo.codec, m_synctype == SYNC_RESAMPLE))
        CLog::Log(LOGERROR, "%s - failed to create audio renderer", __FUNCTION__);

      if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
        m_audioSink.Resume();
    }

    // Apply VolumeAmplification from settings on playback
    m_audioSink.SetDynamicRangeCompression(
        static_cast<long>(m_processInfo.GetVideoSettings().m_VolumeAmplification * 100));

    SetSyncType(audioframe.passthrough);

    // downmix
    double clev = audioframe.hasDownmix ? audioframe.centerMixLevel : M_SQRT1_2;
    double curDB = 20 * log10(clev);
    audioframe.centerMixLevel = pow(10, (curDB + m_processInfo.GetVideoSettings().m_CenterMixLevel) / 20);
    audioframe.hasDownmix = true;
  }


  {
    double syncerror = m_audioSink.GetSyncError();
    if (m_synctype == SYNC_DISCON && fabs(syncerror) > DVD_MSEC_TO_TIME(10))
    {
      double correction = m_pClock->ErrorAdjust(syncerror, "CVideoPlayerAudio::OutputPacket");
      if (correction != 0)
      {
        m_audioSink.SetSyncErrorCorrection(-correction);
      }
    }
  }

  int framesOutput = m_audioSink.AddPackets(audioframe);

  // guess next pts
  m_audioClock += audioframe.duration * ((double)framesOutput / audioframe.nb_frames);

  audioframe.framesOut += framesOutput;

  if(m_bAudio2)
    ProcessDecoderOutput2(audioframe2);

  // signal to our parent that we have initialized
  if (m_syncState == IDVDStreamPlayer::SYNC_STARTING)
  {
    double cachetotal = m_audioSink.GetCacheTotal();
    double cachetime = m_audioSink.GetCacheTime();
    if (cachetime >= cachetotal * 0.75)
    {
      m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
      m_stalled = false;
      SStartMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.cachetotal = m_audioSink.GetMaxDelay() * DVD_TIME_BASE;
      msg.cachetime = m_audioSink.GetDelay();
      msg.timestamp = audioframe.hasTimestamp ? audioframe.pts : DVD_NOPTS_VALUE;
      m_messageParent.Put(new CDVDMsgType<SStartMsg>(CDVDMsg::PLAYER_STARTED, msg));

      m_streaminfo.channels = audioframe.format.m_channelLayout.Count();
      m_processInfo.SetAudioChannels(audioframe.format.m_channelLayout);
      m_processInfo.SetAudioSampleRate(audioframe.format.m_sampleRate);
      m_processInfo.SetAudioBitsPerSample(audioframe.bits_per_sample);
      m_processInfo.SetAudioDecoderName(m_pAudioCodec->GetName());
      if(m_bAudio2)
      {
        m_processInfo.SetAudioChannels(audioframe2.format.m_channelLayout, true);
        m_processInfo.SetAudioSampleRate(audioframe2.format.m_sampleRate, true);
        m_processInfo.SetAudioBitsPerSample(audioframe2.bits_per_sample, true);
        m_processInfo.SetAudioDecoderName(m_pAudioCodec2->GetName(), true);
      }
      m_messageParent.Put(new CDVDMsg(CDVDMsg::PLAYER_AVCHANGE));
    }
  }

  return true;
}

bool CVideoPlayerAudio::ProcessDecoderOutput2(DVDAudioFrame &audioframe2)
{
  if (audioframe2.nb_frames <= audioframe2.framesOut)
  {
    audioframe2.hasDownmix = false;

    m_pAudioCodec2->GetData(audioframe2);

    if (audioframe2.nb_frames == 0)
    {
      return false;
    }

    audioframe2.hasTimestamp = true;
    if (audioframe2.pts == DVD_NOPTS_VALUE)
    {
      audioframe2.pts = m_audioClock;
      audioframe2.hasTimestamp = false;
    }

    // demuxer reads metatags that influence channel layout
    if (m_streaminfo.codec == AV_CODEC_ID_FLAC && m_streaminfo.channellayout)
      audioframe2.format.m_channelLayout = CAEUtil::GetAEChannelLayout(m_streaminfo.channellayout);

    // we have successfully decoded an audio frame, setup renderer to match
    if (!m_audioSink2.IsValidFormat(audioframe2))
    {
      if (m_speed)
        m_audioSink2.Drain();

      m_audioSink2.Destroy(false);

      if (!m_audioSink2.Create(audioframe2, m_streaminfo.codec, m_synctype == SYNC_RESAMPLE))
        CLog::Log(LOGERROR, "%s - failed to create 2nd audio renderer", __FUNCTION__);

      if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
        m_audioSink2.Resume();
    }

    // Apply VolumeAmplification from settings on playback
    m_audioSink2.SetDynamicRangeCompression(
        static_cast<long>(m_processInfo.GetVideoSettings().m_VolumeAmplification * 100));

    // downmix
    double clev = audioframe2.hasDownmix ? audioframe2.centerMixLevel : M_SQRT1_2;
    double curDB = 20 * log10(clev);
    audioframe2.centerMixLevel = pow(10, (curDB + m_processInfo.GetVideoSettings().m_CenterMixLevel) / 20);
    audioframe2.hasDownmix = true;
  }

  bool bAudio2Dumb = CServiceBroker::GetActiveAE(true)->IsDumb();
  bool bAudio2Disabled = CServiceBroker::GetActiveAE(true)->IsDisabled();

  if(!bAudio2Disabled && !bAudio2Dumb && !m_bAudio2Skip && audioframe2.nb_frames > 0)
  {
    int framesOutput = m_audioSink2.AddPackets(audioframe2);
    audioframe2.framesOut += framesOutput;
	if(framesOutput == 0) audioframe2.framesOut = audioframe2.nb_frames;
  }
  else
    audioframe2.framesOut = audioframe2.nb_frames;

  if(bAudio2Disabled || bAudio2Dumb)
  {
    m_audiodiff = 0.0;
  	return false;
  }
  else
    HandleSyncAudio2(audioframe2);

  return true;
}

void CVideoPlayerAudio::SetSyncType(bool passthrough)
{
  if (passthrough && m_synctype == SYNC_RESAMPLE)
    m_synctype = SYNC_DISCON;

  //if SetMaxSpeedAdjust returns false, it means no video is played and we need to use clock feedback
  double maxspeedadjust = 0.0;
  if (m_synctype == SYNC_RESAMPLE)
    maxspeedadjust = m_maxspeedadjust;

  m_pClock->SetMaxSpeedAdjust(maxspeedadjust);

  if (m_synctype != m_prevsynctype)
  {
    const char *synctypes[] = {"clock feedback", "resample", "invalid"};
    int synctype = (m_synctype >= 0 && m_synctype <= 1) ? m_synctype : 2;
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio:: synctype set to %i: %s", m_synctype, synctypes[synctype]);
    m_prevsynctype = m_synctype;
    if (m_synctype == SYNC_RESAMPLE)
      m_audioSink.SetResampleMode(1);
    else
      m_audioSink.SetResampleMode(0);
  }
}

void CVideoPlayerAudio::HandleSyncAudio2(DVDAudioFrame &audioframe2)
{
  if(audioframe2.nb_frames == 0 || audioframe2.planes == 0)
    return;

  double threshold = 50000.0;
  threshold = threshold > audioframe2.duration ? threshold : audioframe2.duration;

  double dtm1 = m_audioSink.GetDelay();
  double dtm2 = m_audioSink2.GetDelay();
  double ddiff = (dtm1 - dtm2);

  m_audiodiff = ddiff / DVD_TIME_BASE;

  if (ddiff > threshold)
  {
    int framesize = audioframe2.passthrough ? 1 : audioframe2.framesize;
    int size2 = audioframe2.nb_frames * framesize / audioframe2.planes;
    for (unsigned int i=0; i<audioframe2.planes; i++)
      memset(audioframe2.data[i], 0, size2);
    m_audioSink2.AddPackets(audioframe2);
  }

  if (ddiff < -threshold)
  {
    m_bAudio2Skip = true;
  }
  else if (m_bAudio2Skip && ddiff > 0.0)
  {
    m_bAudio2Skip = false;
  }
}

void CVideoPlayerAudio::OnExit()
{
#ifdef TARGET_WINDOWS
  CoUninitialize();
#endif

  CLog::Log(LOGINFO, "thread end: CVideoPlayerAudio::OnExit()");
}

void CVideoPlayerAudio::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put( new CDVDMsgInt(CDVDMsg::PLAYER_SETSPEED, speed), 1 );
  else
    m_speed = speed;
}

void CVideoPlayerAudio::Flush(bool sync)
{
  m_messageQueue.Flush();
  m_messageQueue.Put( new CDVDMsgBool(CDVDMsg::GENERAL_FLUSH, sync), 1);

  m_audioSink.AbortAddPackets();
  if(m_bAudio2)
    m_audioSink2.AbortAddPackets();
}

bool CVideoPlayerAudio::AcceptsData() const
{
  bool full = m_messageQueue.IsFull();
  return !full;
}

bool CVideoPlayerAudio::SwitchCodecIfNeeded()
{
  if (m_displayReset)
    CLog::Log(LOGINFO, "CVideoPlayerAudio: display reset occurred, checking for passthrough");
  else
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio: stream props changed, checking for passthrough");

  m_displayReset = false;

  bool bSwitched = false;
  bool allowpassthrough = !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (m_processInfo.IsRealtimeStream() || m_synctype == SYNC_RESAMPLE)
    allowpassthrough = false;

  CAEStreamInfo::DataType streamType = m_audioSink.GetPassthroughStreamType(
      m_streaminfo.codec, m_streaminfo.samplerate, m_streaminfo.profile);
  CDVDAudioCodec *codec = CDVDFactoryCodec::CreateAudioCodec(m_streaminfo, m_processInfo,
                                                             allowpassthrough, m_processInfo.AllowDTSHDDecode(),
                                                             streamType);

  if (!codec || codec->NeedPassthrough() == m_pAudioCodec->NeedPassthrough())
  {
    // passthrough state has not changed
    delete codec;
    bSwitched = false;
  } else {
    delete m_pAudioCodec;
    m_pAudioCodec = codec;
    bSwitched = true;
  }

  if (m_bAudio2)
  {
    CAEStreamInfo::DataType streamType2 = m_audioSink2.GetPassthroughStreamType(
        m_streaminfo.codec, m_streaminfo.samplerate, m_streaminfo.profile);
    CDVDAudioCodec *codec2 = CDVDFactoryCodec::CreateAudioCodec(m_streaminfo, m_processInfo,
                                                                allowpassthrough, m_processInfo.AllowDTSHDDecode(),
                                                                streamType2, true);
    if (codec2 != NULL)
    {
      if (!codec2 || codec2->NeedPassthrough() == m_pAudioCodec2->NeedPassthrough()) {
        // passthrough state has not changed
        delete codec2;
      } else {
        delete m_pAudioCodec2;
        m_pAudioCodec2 = codec2;
      }
    }
  }

  return bSwitched;
}

std::string CVideoPlayerAudio::GetPlayerInfo()
{
  CSingleLock lock(m_info_section);
  return m_info.info;
}

int CVideoPlayerAudio::GetAudioChannels()
{
  return m_streaminfo.channels;
}

bool CVideoPlayerAudio::IsPassthrough() const
{
  CSingleLock lock(m_info_section);
  return m_info.passthrough;
}
