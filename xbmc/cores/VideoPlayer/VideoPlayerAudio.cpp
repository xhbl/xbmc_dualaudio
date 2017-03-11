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

#include "threads/SingleLock.h"
#include "VideoPlayerAudio.h"
#include "DVDCodecs/Audio/DVDAudioCodec.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDDemuxers/DVDDemuxPacket.h"
#include "settings/Settings.h"
#include "utils/log.h"
#include "utils/MathUtils.h"
#include "cores/AudioEngine/AEFactory.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#ifdef TARGET_RASPBERRY_PI
#include "linux/RBP.h"
#endif

#include <sstream>
#include <iomanip>
#include <math.h>

// allow audio for slow and fast speeds (but not rewind/fastforward)
#define ALLOW_AUDIO(speed) ((speed) > 5*DVD_PLAYSPEED_NORMAL/10 && (speed) <= 15*DVD_PLAYSPEED_NORMAL/10)

class CDVDMsgAudioCodecChange : public CDVDMsg
{
public:
  CDVDMsgAudioCodecChange(const CDVDStreamInfo &hints, CDVDAudioCodec* codec, CDVDAudioCodec* codec2)
    : CDVDMsg(GENERAL_STREAMCHANGE)
    , m_codec(codec)
    , m_codec2(codec2)
    , m_hints(hints)
  {}
 ~CDVDMsgAudioCodecChange()
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

CAudio2Frames::CAudio2Frames()
{
  pcap = sizeof(data)/sizeof(uint8_t*);
  incr = 64*1024;
  for (int i=0; i < (int)pcap; i++)
  {
    data[i] = NULL;
    capa[i] = 0;
    size[i] = 0;
  }
  plns = 0;
}

CAudio2Frames::~CAudio2Frames()
{
  for (int i=0; i < (int)pcap; i++)
  {
    if(data[i]) free(data[i]);
  }
}

void CAudio2Frames::Add(DVDAudioFrame af)
{
  if(!af.data[0] || !af.nb_frames || !af.planes)
    return;

  if(plns == 0)
  {
	plns = af.planes < pcap ? af.planes : pcap;
  }

  int af_size = af.nb_frames * af.framesize / af.planes;
  for (int i=0; i < (int)af.planes; i++)
  {
    if(size[i] + af_size > capa[i])
    {
      capa[i] = ((size[i] + af_size) / incr + 1) * incr;
      data[i] = (uint8_t*)realloc(data[i], capa[i]);
    }
    if(af.data[i])
    {
      memcpy(data[i]+size[i], af.data[i], af_size);
    }
    else
    {
      memset(data[i]+size[i], 0, af_size);
    }
    af.data[i] = data[i] + size[i];
    size[i] += af_size;
  }
  afs.push_back(af);
}

bool CAudio2Frames::Merge(DVDAudioFrame& af)
{
  if (!afs.size())
    return false;
  af = afs.front();
  for (int i=0; i < (int)plns; i++)
  {
    af.data[i] = data[i];
  }
  af.duration = 0;
  af.nb_frames = 0;
  for (std::list<DVDAudioFrame>::iterator it = afs.begin(); it != afs.end(); ++it)
  {
    af.duration += it->duration;
    af.nb_frames += it->nb_frames;
  }
  return true;
}

void CAudio2Frames::Clear()
{
  afs.clear();
  for (int i=0; i < (int)pcap; i++)
  {
    size[i] = 0;
  }
  plns = 0;
}


CVideoPlayerAudio::CVideoPlayerAudio(CDVDClock* pClock, CDVDMessageQueue& parent, CProcessInfo &processInfo)
: CThread("VideoPlayerAudio"), IDVDStreamPlayerAudio(processInfo)
, m_messageQueue("audio")
, m_messageParent(parent)
, m_dvdAudio(pClock)
, m_dvdAudio2(pClock)
{
  m_pClock = pClock;
  m_pAudioCodec = NULL;
  m_pAudioCodec2 = NULL;
  m_bAudio2 = false;
  m_bAudio2Skip = false;
  m_bAudio2Dumb = false;
  m_audioClock = 0;
  m_speed = DVD_PLAYSPEED_NORMAL;
  m_stalled = true;
  m_paused = false;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_synctype = SYNC_DISCON;
  m_setsynctype = SYNC_DISCON;
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

bool CVideoPlayerAudio::OpenStream(CDVDStreamInfo &hints)
{
  m_bAudio2 = CSettings::GetInstance().GetBool(CSettings::SETTING_AUDIOOUTPUT2_ENABLED) ? true : false;

  m_processInfo.ResetAudioCodecInfo();

  CLog::Log(LOGNOTICE, "Finding audio codec for: %i", hints.codec);
  bool allowpassthrough = !CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (hints.realtime)
    allowpassthrough = false;
  CDVDAudioCodec* codec = CDVDFactoryCodec::CreateAudioCodec(hints, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode());
  if(!codec)
  {
    CLog::Log(LOGERROR, "Unsupported audio codec");
    return false;
  }
  CDVDAudioCodec* codec2 = NULL;
  if (m_bAudio2)
  {
    codec2 = CDVDFactoryCodec::CreateAudioCodec(hints, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), m_bAudio2);
    if( !codec2 )
    {
      CLog::Log(LOGERROR, "Unsupported 2nd audio codec");
      m_dvdAudio2.Destroy();
      m_bAudio2 = false;
    }
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(new CDVDMsgAudioCodecChange(hints, codec, codec2), 0);
  else
  {
    OpenStream(hints, codec, codec2);
    m_messageQueue.Init();
    CLog::Log(LOGNOTICE, "Creating audio thread");
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

  m_synctype = SYNC_DISCON;
  m_setsynctype = SYNC_DISCON;
  if (CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK))
    m_setsynctype = SYNC_RESAMPLE;
  else if (hints.realtime)
    m_setsynctype = SYNC_RESAMPLE;

  m_prevsynctype = -1;

  m_prevskipped = false;

  m_maxspeedadjust = 5.0;

  m_messageParent.Put(new CDVDMsg(CDVDMsg::PLAYER_AVCHANGE));
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
}

void CVideoPlayerAudio::CloseStream(bool bWaitForBuffers)
{
  bool bWait = bWaitForBuffers && m_speed > 0 && !CAEFactory::IsSuspended();

  // wait until buffers are empty
  if (bWait)
    m_messageQueue.WaitUntilEmpty();

  // send abort message to the audio queue
  m_messageQueue.Abort();

  CLog::Log(LOGNOTICE, "Waiting for audio thread to exit");

  // shut down the adio_decode thread and wait for it
  StopThread(); // will set this->m_bStop to true

  // destroy audio device
  CLog::Log(LOGNOTICE, "Closing audio device");
  if (bWait)
  {
    m_bStop = false;
    m_dvdAudio.Drain();
    if (m_bAudio2)
      m_dvdAudio2.Drain();
    m_bStop = true;
  }
  else
  {
    m_dvdAudio.Flush();
    if (m_bAudio2)
      m_dvdAudio2.Flush();
  }

  m_dvdAudio.Destroy();
  if (m_bAudio2)
    m_dvdAudio2.Destroy();

  // uninit queue
  m_messageQueue.End();

  CLog::Log(LOGNOTICE, "Deleting audio codec");
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
  s << ", Kb/s:" << std::fixed << std::setprecision(2) << (double)GetAudioBitrate() / 1024.0;

  //print the inverse of the resample ratio, since that makes more sense
  //if the resample ratio is 0.5, then we're playing twice as fast
  if (m_synctype == SYNC_RESAMPLE)
    s << ", rr:" << std::fixed << std::setprecision(5) << 1.0 / m_dvdAudio.GetResampleRatio();

  if (m_bAudio2)
    s << ", a1/a2:" << std::fixed << std::setprecision(3) << m_audiodiff;

  s << ", att:" << std::fixed << std::setprecision(1) << log(GetCurrentAttenuation()) * 20.0f << " dB";

  SInfo info;
  info.info        = s.str();
  info.pts         = m_dvdAudio.GetPlayingPts();
  info.passthrough = m_pAudioCodec && m_pAudioCodec->NeedPassthrough() && (!m_bAudio2 || (m_pAudioCodec2 && m_pAudioCodec2->NeedPassthrough()));

  { CSingleLock lock(m_info_section);
    m_info = info;
  }
}

void CVideoPlayerAudio::Process()
{
  CLog::Log(LOGNOTICE, "running thread: CVideoPlayerAudio::Process()");

  DVDAudioFrame audioframe;
  DVDAudioFrame audioframe2;
  m_audioStats.Start();
  m_audiodiff = 0.0;
  m_bAudio2Skip = false;

  while (!m_bStop)
  {
    CDVDMsg* pMsg;
    int timeout  = (int)(1000 * m_dvdAudio.GetCacheTime());

    // read next packet and return -1 on error
    int priority = 1;
    //Do we want a new audio frame?
    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING ||              /* when not started */
        ALLOW_AUDIO(m_speed) || /* when playing normally */
        m_speed <  DVD_PLAYSPEED_PAUSE  || /* when rewinding */
        (m_speed >  DVD_PLAYSPEED_NORMAL && m_audioClock < m_pClock->GetClock())) /* when behind clock in ff */
      priority = 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      priority = 1;

    if (m_paused)
      priority = 1;

    MsgQueueReturnCode ret = m_messageQueue.Get(&pMsg, timeout, priority);

    if (MSGQ_IS_ERROR(ret))
    {
      CLog::Log(LOGERROR, "Got MSGQ_ABORT or MSGO_IS_ERROR return true");
      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      // Flush as the audio output may keep looping if we don't
      if (ALLOW_AUDIO(m_speed) && !m_stalled && m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        // while AE sync is active, we still have time to fill buffers
        if (m_syncTimer.IsTimePast())
        {
          CLog::Log(LOGNOTICE, "CVideoPlayerAudio::Process - stream stalled");
          m_stalled = true;
        }
      }
      if (timeout == 0)
        Sleep(10);
      continue;
    }

    // handle messages
    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if(((CDVDMsgGeneralSynchronize*)pMsg)->Wait(100, SYNCSOURCE_AUDIO))
        CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_SYNCHRONIZE");
      else
        m_messageQueue.Put(pMsg->Acquire(), 1);  // push back as prio message, to process other prio messages
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    { //player asked us to set internal clock
      double pts = static_cast<CDVDMsgDouble*>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_RESYNC(%f)", pts);

      m_audioClock = pts + m_dvdAudio.GetDelay();
      if (m_speed != DVD_PLAYSPEED_PAUSE)
      {
        m_dvdAudio.Resume();
        if (m_bAudio2)
          m_dvdAudio2.Resume();
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
      m_dvdAudio.Flush();
      if (m_bAudio2)
        m_dvdAudio2.Flush();
      m_stalled = true;
      m_audioClock = 0;
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      bool sync = static_cast<CDVDMsgBool*>(pMsg)->m_value;
      m_dvdAudio.Flush();
      if (m_bAudio2)
        m_dvdAudio2.Flush();
      m_stalled = true;
      m_audioClock = 0;

      if (sync)
      {
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        m_dvdAudio.Pause();
        if (m_bAudio2)
          m_dvdAudio2.Pause();
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

      if (ALLOW_AUDIO(speed))
      {
        if (speed != m_speed)
        {
          if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
          {
            m_dvdAudio.Resume();
            if (m_bAudio2)
              m_dvdAudio2.Resume();
          }
        }
      }
      else
      {
        m_dvdAudio.Pause();
        if (m_bAudio2)
          m_dvdAudio2.Pause();
      }
      m_speed = speed;
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
    else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacket();
      bool bPacketDrop  = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacketDrop();

      int consumed = m_pAudioCodec->Decode(pPacket->pData, pPacket->iSize, pPacket->dts, pPacket->pts);
      if (m_bAudio2)
        m_pAudioCodec2->Decode(pPacket->pData, pPacket->iSize, pPacket->dts, pPacket->pts);
      if (consumed < 0)
      {
        CLog::Log(LOGERROR, "CVideoPlayerAudio::DecodeFrame - Decode Error. Skipping audio packet (%d)", consumed);
        m_pAudioCodec->Reset();
        if (m_bAudio2)
          m_pAudioCodec2->Reset();
        pMsg->Release();
        continue;
      }

      m_audioStats.AddSampleBytes(pPacket->iSize);
      UpdatePlayerInfo();

      // make sure the sent frame is clean
      audioframe.nb_frames = 0;
      audioframe2.nb_frames = 0;
      m_audio2frames.Clear();
	  
      // loop while no error and decoder produces output
      while (!m_bStop)
      {
        // get decoded data and the size of it
        m_pAudioCodec->GetData(audioframe);
        if (audioframe.format.m_dataFormat == AE_FMT_RAW )
          audioframe.framesize = audioframe.format.m_frameSize;

        if (m_bAudio2)
        {
          m_pAudioCodec2->GetData(audioframe2);
          if (audioframe2.nb_frames > 0)
          {
            if (audioframe2.format.m_dataFormat == AE_FMT_RAW )
              audioframe2.framesize = audioframe2.format.m_frameSize;
            m_audio2frames.Add(audioframe2);
          }
        }

        if (audioframe.nb_frames == 0)
        {
          if (consumed >= pPacket->iSize)
            break;
          int ret = m_pAudioCodec->Decode(pPacket->pData+consumed, pPacket->iSize-consumed, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
          if (m_bAudio2)
            m_pAudioCodec2->Decode(pPacket->pData+consumed, pPacket->iSize-consumed, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
          if (ret < 0)
          {
            CLog::Log(LOGERROR, "CVideoPlayerAudio::DecodeFrame - Decode Error. Skipping audio packet (%d)", ret);
            m_pAudioCodec->Reset();
            if (m_bAudio2)
              m_pAudioCodec2->Reset();
            break;
          }
          consumed += ret;
          continue;
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

        if (m_bAudio2)
        {
          m_audio2frames.Merge(audioframe2);

          if (audioframe2.nb_frames > 0)
          {
            if (audioframe2.pts == DVD_NOPTS_VALUE)
            {
              audioframe2.pts = m_audioClock;
              audioframe2.hasTimestamp = false;
            }
          }
        }

        //Drop when not playing normally
        if (!ALLOW_AUDIO(m_speed) && m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
        {
          break;
        }

        if (audioframe.format.m_sampleRate && m_streaminfo.samplerate != (int) audioframe.format.m_sampleRate)
        {
          // The sample rate has changed or we just got it for the first time
          // for this stream. See if we should enable/disable passthrough due
          // to it.
          m_streaminfo.samplerate = audioframe.format.m_sampleRate;
          if (SwitchCodecIfNeeded())
          {
            break;
          }
        }

        // demuxer reads metatags that influence channel layout
        if (m_streaminfo.codec == AV_CODEC_ID_FLAC && m_streaminfo.channellayout)
          audioframe.format.m_channelLayout = CAEUtil::GetAEChannelLayout(m_streaminfo.channellayout);

        // we have succesfully decoded an audio frame, setup renderer to match
        if (!m_dvdAudio.IsValidFormat(audioframe))
        {
          if(m_speed)
            m_dvdAudio.Drain();

          m_dvdAudio.Destroy();

          if (!m_dvdAudio.Create(audioframe, m_streaminfo.codec, m_setsynctype == SYNC_RESAMPLE))
            CLog::Log(LOGERROR, "%s - failed to create audio renderer", __FUNCTION__);

          if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
            m_dvdAudio.Resume();

          m_streaminfo.channels = audioframe.format.m_channelLayout.Count();


          m_processInfo.SetAudioChannels(audioframe.format.m_channelLayout);
          m_processInfo.SetAudioSampleRate(audioframe.format.m_sampleRate);
          m_processInfo.SetAudioBitsPerSample(audioframe.bits_per_sample);

          m_messageParent.Put(new CDVDMsg(CDVDMsg::PLAYER_AVCHANGE));
        }

        if (m_bAudio2 && audioframe2.nb_frames > 0 && !m_dvdAudio2.IsValidFormat(audioframe2))
        {
          if(m_speed)
            m_dvdAudio2.Drain();

          m_dvdAudio2.Destroy();

          if(!m_dvdAudio2.Create(audioframe2, m_streaminfo.codec, m_setsynctype == SYNC_RESAMPLE, m_bAudio2))
            CLog::Log(LOGERROR, "%s - failed to create 2nd audio renderer", __FUNCTION__);

          if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
            m_dvdAudio2.Resume();
		}
		if (m_bAudio2)
			m_bAudio2Dumb = CAEFactory::IsDumb(true);

        SetSyncType(audioframe.passthrough);

        if (!bPacketDrop)
        {
          OutputPacket(audioframe, audioframe2);

          // signal to our parent that we have initialized
          if(m_syncState == IDVDStreamPlayer::SYNC_STARTING)
          {
            if (m_bAudio2)
              HandleSyncAudio2(audioframe2);
			  
            double cachetotal = DVD_SEC_TO_TIME(m_dvdAudio.GetCacheTotal());
            double cachetime = m_dvdAudio.GetDelay();
            if (cachetime >= cachetotal * 0.5)
            {
              m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
              m_stalled = false;
              SStartMsg msg;
              msg.player = VideoPlayer_AUDIO;
              msg.cachetotal = cachetotal;
              msg.cachetime = cachetime;
              msg.timestamp = audioframe.hasTimestamp ? audioframe.pts : DVD_NOPTS_VALUE;
              m_messageParent.Put(new CDVDMsgType<SStartMsg>(CDVDMsg::PLAYER_STARTED, msg));

              if (consumed < pPacket->iSize)
              {
                pPacket->iSize -= consumed;
                memmove(pPacket->pData, pPacket->pData + consumed, pPacket->iSize);
                m_messageQueue.Put(pMsg, 0, false);
                pMsg->Acquire();
                break;
              }
            }
          }
        }

        // guess next pts
        m_audioClock += audioframe.duration;

        int ret = m_pAudioCodec->Decode(nullptr, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
        if (m_bAudio2)
          m_pAudioCodec2->Decode(nullptr, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
        if (ret < 0)
        {
          CLog::Log(LOGERROR, "CVideoPlayerAudio::DecodeFrame - Decode Error. Skipping audio packet (%d)", ret);
          m_pAudioCodec->Reset();
          if (m_bAudio2)
            m_pAudioCodec2->Reset();
          break;
        }
      } // while decoder produces output

    } // demuxer packet
    
    pMsg->Release();
  }
}

void CVideoPlayerAudio::SetSyncType(bool passthrough)
{
  //set the synctype from the gui
  m_synctype = m_setsynctype;
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
      m_dvdAudio.SetResampleMode(1);
    else
      m_dvdAudio.SetResampleMode(0);
  }
}

void CVideoPlayerAudio::HandleSyncAudio2(DVDAudioFrame &audioframe2)
{
  if(m_bAudio2Dumb)
  {
    m_audiodiff = 0.0;
	return;
  }
  if(audioframe2.nb_frames == 0 || audioframe2.planes == 0)
    return;

  double threshold = 50000.0;
  threshold = threshold > audioframe2.duration ? threshold : audioframe2.duration;

  double dtm1 = m_dvdAudio.GetDelay();
  double dtm2 = m_dvdAudio2.GetDelay();
  double ddiff = (dtm1 - dtm2);

  m_audiodiff = ddiff / DVD_TIME_BASE;

  if (ddiff > threshold)
  {
    int size2 = audioframe2.nb_frames * audioframe2.framesize / audioframe2.planes;
    for (unsigned int i=0; i<audioframe2.planes; i++)
      memset(audioframe2.data[i], 0, size2);
    m_dvdAudio2.AddPackets(audioframe2);
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

bool CVideoPlayerAudio::OutputPacket(DVDAudioFrame &audioframe, DVDAudioFrame &audioframe2)
{
  bool bAddAudio2 = (m_bAudio2 && !m_bAudio2Dumb && !m_bAudio2Skip && audioframe2.nb_frames > 0);
  double syncerror = m_dvdAudio.GetSyncError();

  if (m_synctype == SYNC_DISCON && fabs(syncerror) > DVD_MSEC_TO_TIME(10))
  {
    double correction = m_pClock->ErrorAdjust(syncerror, "CVideoPlayerAudio::OutputPacket");
    if (correction != 0)
    {
      m_dvdAudio.SetSyncErrorCorrection(-correction);
    }
  }
  m_dvdAudio.AddPackets(audioframe);
  if (bAddAudio2)
    m_dvdAudio2.AddPackets(audioframe2);

  return true;
}

void CVideoPlayerAudio::OnExit()
{
#ifdef TARGET_WINDOWS
  CoUninitialize();
#endif

  CLog::Log(LOGNOTICE, "thread end: CVideoPlayerAudio::OnExit()");
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

  m_dvdAudio.AbortAddPackets();
}

bool CVideoPlayerAudio::AcceptsData() const
{
  bool full = m_messageQueue.IsFull();
  return !full;
}

bool CVideoPlayerAudio::SwitchCodecIfNeeded()
{
  CLog::Log(LOGDEBUG, "CVideoPlayerAudio: Sample rate changed, checking for passthrough");
  bool allowpassthrough = !CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (m_streaminfo.realtime)
    allowpassthrough = false;
  bool bSwitched = false;
  CDVDAudioCodec *codec = CDVDFactoryCodec::CreateAudioCodec(m_streaminfo, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode());
  if (!codec || codec->NeedPassthrough() == m_pAudioCodec->NeedPassthrough()) {
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
    CDVDAudioCodec *codec2 = CDVDFactoryCodec::CreateAudioCodec(m_streaminfo, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), true);
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

int CVideoPlayerAudio::GetAudioBitrate()
{
  return (int)m_audioStats.GetBitrate();
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
