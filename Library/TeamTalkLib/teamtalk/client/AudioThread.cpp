/*
 * Copyright (c) 2005-2018, BearWare.dk
 * 
 * Contact Information:
 *
 * Bjoern D. Rasmussen
 * Kirketoften 5
 * DK-8260 Viby J
 * Denmark
 * Email: contact@bearware.dk
 * Phone: +45 20 20 54 59
 * Web: http://www.bearware.dk
 *
 * This source code is part of the TeamTalk SDK owned by
 * BearWare.dk. Use of this file, or its compiled unit, requires a
 * TeamTalk SDK License Key issued by BearWare.dk.
 *
 * The TeamTalk SDK License Agreement along with its Terms and
 * Conditions are outlined in the file License.txt included with the
 * TeamTalk SDK distribution.
 *
 */

#include "AudioThread.h"

#include "myace/MyACE.h"
#include "teamtalk/CodecCommon.h"
#include "teamtalk/PacketLayout.h"
#include "teamtalk/TTAssert.h"

#if defined(ENABLE_WEBRTC)
#include "avstream/WebRTCPreprocess.h"
#endif

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace teamtalk;

AudioThread::AudioThread()
{
    m_encbuf.resize(MAX_ENC_FRAMESIZE);
}

AudioThread::~AudioThread()
{
    FreeFFmpegFilter();
    MYTRACE(ACE_TEXT("AudioThread\n"));
}

void AudioThread::SetFFmpegFilter(const std::string& filter_str)
{
    std::unique_lock<std::recursive_mutex> const g(m_preprocess_lock);
    m_ffmpeg_filter_str = filter_str;
    m_filter_changed = true;
}

void AudioThread::FreeFFmpegFilter()
{
    if (m_filter_graph) {
        avfilter_graph_free(&m_filter_graph);
        m_filter_graph = nullptr;
        m_buffersrc_ctx = nullptr;
        m_buffersink_ctx = nullptr;
    }
    m_filter_fifo.clear();
}

bool AudioThread::InitFFmpegFilter(const media::AudioFormat& format)
{
    FreeFFmpegFilter();
    
    if (m_ffmpeg_filter_str.empty())
        return true;

    char args[512];
    int ret = 0;
    const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    
    m_filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !m_filter_graph) {
        if (outputs) avfilter_inout_free(&outputs);
        if (inputs) avfilter_inout_free(&inputs);
        FreeFFmpegFilter();
        return false;
    }

    AVChannelLayout in_ch_layout = {};
    av_channel_layout_default(&in_ch_layout, format.channels);
    char ch_layout_str[64];
    av_channel_layout_describe(&in_ch_layout, ch_layout_str, sizeof(ch_layout_str));

    snprintf(args, sizeof(args),
             "time_base=1/%d:sample_rate=%d:sample_fmt=s16:channel_layout=%s",
             format.samplerate, format.samplerate, ch_layout_str);

    ret = avfilter_graph_create_filter(&m_buffersrc_ctx, abuffersrc, "in",
                                       args, nullptr, m_filter_graph);
    if (ret < 0) goto filter_init_error;

    ret = avfilter_graph_create_filter(&m_buffersink_ctx, abuffersink, "out",
                                       nullptr, nullptr, m_filter_graph);
    if (ret < 0) goto filter_init_error;

    // رفع خطای rvalue با تعریف متغیرهای محلی صریح (Explicit Local Variables)
    {
        const enum AVSampleFormat out_sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
        ret = av_opt_set_int_list(m_buffersink_ctx, "sample_fmts", out_sample_fmts, -1, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) goto filter_init_error;

        const int out_sample_rates[] = { format.samplerate, -1 };
        ret = av_opt_set_int_list(m_buffersink_ctx, "sample_rates", out_sample_rates, -1, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) goto filter_init_error;

        AVChannelLayout out_ch_layout = {};
        av_channel_layout_default(&out_ch_layout, format.channels);
        const AVChannelLayout out_ch_layouts[] = { out_ch_layout };
        ret = av_opt_set_array(m_buffersink_ctx, "channel_layouts", AV_OPT_SEARCH_CHILDREN, 0, 1, AV_OPT_TYPE_CHLAYOUT, out_ch_layouts);
        if (ret < 0) goto filter_init_error;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = m_buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = nullptr;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = m_buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = nullptr;

    ret = avfilter_graph_parse_ptr(m_filter_graph, m_ffmpeg_filter_str.c_str(),
                                   &inputs, &outputs, nullptr);
    if (ret < 0) goto filter_init_error;

    ret = avfilter_graph_config(m_filter_graph, nullptr);
    if (ret < 0) goto filter_init_error;

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return true;

filter_init_error:
    if (inputs) avfilter_inout_free(&inputs);
    if (outputs) avfilter_inout_free(&outputs);
    MYTRACE(ACE_TEXT("Failed to init FFmpeg Filter: %s. Error: %d\n"), m_ffmpeg_filter_str.c_str(), ret);
    FreeFFmpegFilter();
    return false;
}

bool AudioThread::StartEncoder(const audioencodercallback_t& callback,
                               const teamtalk::AudioCodec& codec,
                               bool spawn_thread)
{
    if(this->thr_count() != 0)
        return false;

    TTASSERT(this->msg_queue()->is_empty());

    int const callback_samples = GetAudioCodecCbSamples(codec);
    int const sample_rate = GetAudioCodecSampleRate(codec);
    int const channels = GetAudioCodecChannels(codec);

    switch(codec.codec)
    {
    case CODEC_NO_CODEC :
    {
        m_codec = codec;
        m_callback = callback;
        return true;
    }
    break;
    case CODEC_SPEEX :
#if defined(ENABLE_SPEEX)
    {
        TTASSERT(callback_samples);
        TTASSERT(sample_rate);
        TTASSERT(channels);

        m_speex = std::make_unique<SpeexEncoder>();
        if(!m_speex->Initialize(codec.speex.bandmode,
                                DEFAULT_SPEEX_COMPLEXITY,
                                codec.speex.quality))
        {
            StopEncoder();
            return false;
        }
    }
    break;
#else
    return false;
#endif
    case CODEC_SPEEX_VBR :
#if defined(ENABLE_SPEEX)
    {
        TTASSERT(callback_samples);
        TTASSERT(sample_rate);
        TTASSERT(channels);

        m_speex = std::make_unique<SpeexEncoder>();
        if(!m_speex->Initialize(codec.speex_vbr.bandmode,
                                DEFAULT_SPEEX_COMPLEXITY,
                                (float)codec.speex_vbr.vbr_quality,
                                codec.speex_vbr.bitrate,
                                codec.speex_vbr.max_bitrate,
                                codec.speex_vbr.dtx))
        {
            StopEncoder();
            return false;
        }
    }
    break;
#else
    return false;
#endif
#if defined(ENABLE_OPUS)
    case CODEC_OPUS :
    {
        TTASSERT(callback_samples);
        TTASSERT(sample_rate);
        TTASSERT(channels);

        m_opus = std::make_unique<OpusEncode>();
        if(!m_opus->Open(codec.opus.samplerate, codec.opus.channels,
                         codec.opus.application) ||
           !m_opus->SetComplexity(codec.opus.complexity) ||
           !m_opus->SetFEC(codec.opus.fec) ||
           !m_opus->SetDTX(codec.opus.dtx) ||
           !m_opus->SetBitrate(codec.opus.bitrate) ||
           !m_opus->SetVBR(codec.opus.vbr) ||
           !m_opus->SetVBRConstraint(codec.opus.vbr_constraint))
        {
            StopEncoder();
            return false;
        }
    }
    break;
#else
    return false;
#endif
    default:
        TTASSERT(codec.codec == CODEC_SPEEX);
    }

    TTASSERT(sample_rate);

    if((sample_rate == 0) || (callback_samples == 0))
        return false;

    m_codec = codec;
    m_callback = callback;

    int max_queue = PCM16_BYTES(sample_rate, GetAudioCodecChannels(codec));
    max_queue += (1 + (sample_rate / callback_samples)) * sizeof(media::AudioFrame);

    this->msg_queue()->activate();
    this->msg_queue()->high_water_mark(max_queue);
    this->msg_queue()->low_water_mark(max_queue);

    if(spawn_thread && this->activate() < 0)
    {
        StopEncoder();
        return false;
    }

    return true;
}

void AudioThread::StopEncoder()
{
    this->msg_queue()->close();
    wait();

    FreeFFmpegFilter();

#if defined(ENABLE_SPEEXDSP)
    m_preprocess_left.reset();
    m_preprocess_right.reset();
#endif

#if defined(ENABLE_WEBRTC)
    m_apm.release();
    m_aps.reset();
#endif

#if defined(ENABLE_SPEEX)
    m_speex.reset();
#endif

#if defined(ENABLE_OPUS)
    m_opus.reset();
#endif
    m_enc_cleared = true;

    m_echobuf.clear();
    m_callback = {};

    memset(&m_codec, 0, sizeof(m_codec));
    m_codec.codec = teamtalk::CODEC_NO_CODEC;
}

int AudioThread::close(u_long /*flags*/)
{
    return 0;
}

bool AudioThread::UpdatePreprocessor(const teamtalk::AudioPreprocessor& preprocess)
{
    std::unique_lock<std::recursive_mutex> const g(m_preprocess_lock);

    if (preprocess.preprocessor != AUDIOPREPROCESSOR_TEAMTALK)
        MuteSound(false, false);

#if defined(ENABLE_SPEEXDSP)
    if (preprocess.preprocessor != AUDIOPREPROCESSOR_SPEEXDSP)
    {
        m_preprocess_left.reset();
        m_preprocess_right.reset();
    }
#endif

#if defined(ENABLE_WEBRTC)
    if (preprocess.preprocessor != AUDIOPREPROCESSOR_WEBRTC)
    {
        m_apm.release();
        m_aps.reset();
    }
#endif

    if (Codec().codec == CODEC_NO_CODEC)
        return true;

    switch (preprocess.preprocessor)
    {
    case AUDIOPREPROCESSOR_WEBRTC_OBSOLETE_R4332 :
        return false;
    case AUDIOPREPROCESSOR_NONE :
        return true;
    case AUDIOPREPROCESSOR_SPEEXDSP :
        return UpdatePreprocess(preprocess.speexdsp);
    case AUDIOPREPROCESSOR_TEAMTALK :
        MuteSound(preprocess.ttpreprocessor.muteleft, preprocess.ttpreprocessor.muteright);
        m_gainlevel = preprocess.ttpreprocessor.gainlevel;
        return true;
    case AUDIOPREPROCESSOR_WEBRTC :
#if defined(ENABLE_WEBRTC)
        if (GetAudioCodecCbMillis(m_codec) % 10 != 0)
            return false;

        if (!m_apm)
            m_apm = webrtc::AudioProcessingBuilder().Create();
        m_apm->ApplyConfig(preprocess.webrtc);
        if (m_apm->Initialize() != webrtc::AudioProcessing::kNoError)
        {
            m_apm.release();
            return false;
        }
        
        m_aps = std::make_unique<webrtc::AudioProcessingStats>();
        return true;
#else
        return false;
#endif
    }
    return false;
}

bool AudioThread::UpdatePreprocess(const teamtalk::SpeexDSP& speexdsp)
{
#if defined(ENABLE_SPEEXDSP)
    assert(Codec().codec != CODEC_NO_CODEC);

    int const callback_samples = GetAudioCodecCbSamples(Codec());
    int const sample_rate = GetAudioCodecSampleRate(Codec());
    int const channels = GetAudioCodecChannels(Codec());

    if (!m_preprocess_left)
    {
        if (channels == 2)
        {
            m_preprocess_left = std::make_unique<SpeexPreprocess>();
            m_preprocess_right = std::make_unique<SpeexPreprocess>();

            if (!m_preprocess_left->Initialize(sample_rate, callback_samples) ||
                !m_preprocess_right->Initialize(sample_rate, callback_samples))
            {
                m_preprocess_left.reset();
                m_preprocess_right.reset();
                return false;
            }

            m_preprocess_left->EnableDenoise(false);
            m_preprocess_right->EnableDenoise(false);
        }
        else
        {
            m_preprocess_left = std::make_unique<SpeexPreprocess>();
            if (!m_preprocess_left->Initialize(sample_rate, callback_samples))
            {
                m_preprocess_left.reset();
                return false;
            }
            m_preprocess_left->EnableDenoise(false);
        }
    }

    SpeexAGC agc;
    agc.gain_level = (float)speexdsp.agc_gainlevel;
    agc.max_increment = speexdsp.agc_maxincdbsec;
    agc.max_decrement = speexdsp.agc_maxdecdbsec;
    agc.max_gain = speexdsp.agc_maxgaindb;

    bool agc_success = true;
    agc_success &= m_preprocess_left->EnableAGC(speexdsp.enable_agc);
    agc_success &= (channels == 1 || m_preprocess_right->EnableAGC(speexdsp.enable_agc));
    agc_success &= m_preprocess_left->SetAGCSettings(agc);
    agc_success &= (channels == 1 || m_preprocess_right->SetAGCSettings(agc));

    bool denoise_success = true;
    denoise_success &= m_preprocess_left->EnableDenoise(speexdsp.enable_denoise);
    denoise_success &= (channels == 1 || m_preprocess_right->EnableDenoise(speexdsp.enable_denoise));
    denoise_success &= m_preprocess_left->SetDenoiseLevel(speexdsp.maxnoisesuppressdb);
    denoise_success &= (channels == 1 || m_preprocess_right->SetDenoiseLevel(speexdsp.maxnoisesuppressdb));

    bool aec_success = true;
    aec_success &= m_preprocess_left->EnableEchoCancel(speexdsp.enable_aec);
    aec_success &= (channels == 1 || m_preprocess_right->EnableEchoCancel(speexdsp.enable_aec));
    aec_success &= m_preprocess_left->SetEchoSuppressLevel(speexdsp.aec_suppress_level);
    aec_success &= (channels == 1 || m_preprocess_right->SetEchoSuppressLevel(speexdsp.aec_suppress_level));
    aec_success &= m_preprocess_left->SetEchoSuppressActive(speexdsp.aec_suppress_active);
    aec_success &= (channels == 1 || m_preprocess_right->SetEchoSuppressActive(speexdsp.aec_suppress_active));

    bool const dereverb = true;
    m_preprocess_left->EnableDereverb(dereverb);
    if(channels == 2)
        m_preprocess_right->EnableDereverb(dereverb);

    if ((speexdsp.enable_agc && !agc_success) ||
        (speexdsp.enable_denoise && !denoise_success) ||
        (speexdsp.enable_aec && !aec_success))
        return false;

    return true;
#else
    return false;
#endif
}

void AudioThread::MuteSound(bool leftchannel, bool rightchannel)
{
    m_stereo = ToStereoMask(leftchannel, rightchannel);
}

void AudioThread::QueueAudio(const media::AudioFrame& audframe)
{
    TTASSERT(m_codec.codec != CODEC_NO_CODEC);
    ACE_Message_Block* mb = AudioFrameToMsgBlock(audframe);
    if (mb != nullptr)
        QueueAudio(mb);
}

void AudioThread::QueueAudio(ACE_Message_Block* mb_audio)
{
    ACE_Time_Value tv;
    if(putq(mb_audio, &tv)<0)
    {
        mb_audio->release();
    }
}

bool AudioThread::IsVoiceActive()
{
#if defined(ENABLE_WEBRTC)
    std::unique_lock<std::recursive_mutex> const g(m_preprocess_lock);
    if (m_apm)
    {
        assert(m_aps);
        return m_aps->voice_detected.value_or(false) ||
            m_lastActive + m_voiceact_delay > ACE_OS::gettimeofday();
    }
#endif
    return m_voicelevel >= m_voiceactlevel ||
        m_lastActive + m_voiceact_delay > ACE_OS::gettimeofday();
}

int AudioThread::GetCurrentVoiceLevel() const
{
    return m_voicelevel;
}

void AudioThread::ProcessQueue(ACE_Time_Value* tm)
{
    TTASSERT(m_codec.codec != CODEC_NO_CODEC);
    TTASSERT(m_callback);
    ACE_Message_Block* mb = nullptr;
    while (getq(mb, tm) >= 0)
    {
        MBGuard const g(mb);
        media::AudioFrame af(mb);
        ProcessAudioFrame(af);
    }
}

int AudioThread::svc()
{
    ProcessQueue(nullptr);
    return 0;
}

void AudioThread::ProcessAudioFrame(media::AudioFrame& audblock)
{
    if(m_tone_frequency != 0u)
         m_tone_sample_index = GenerateTone(audblock, m_tone_sample_index, m_tone_frequency);

    SOFTGAIN(audblock.input_buffer, audblock.input_samples,
             audblock.inputfmt.channels, m_gainlevel, GAIN_NORMAL);

#if defined(ENABLE_SPEEXDSP)
    PreprocessSpeex(audblock);
#endif

#if defined(ENABLE_WEBRTC)
    if (m_gainlevel > 0)
    {
        PreprocessWebRTC(audblock);
    }
#endif

    // اعمال فیلترهای صوتی FFmpeg
    {
        std::unique_lock<std::recursive_mutex> const g(m_preprocess_lock);
        
        if (m_filter_changed) {
            InitFFmpegFilter(audblock.inputfmt);
            m_filter_changed = false;
        }

        if (m_filter_graph && m_buffersrc_ctx && m_buffersink_ctx) {
            AVFrame* frame = av_frame_alloc();
            if (frame) {
                frame->nb_samples = audblock.input_samples;
                frame->format = AV_SAMPLE_FMT_S16;
                frame->sample_rate = audblock.inputfmt.samplerate;
                av_channel_layout_default(&frame->ch_layout, audblock.inputfmt.channels);
                
                if (av_frame_get_buffer(frame, 0) >= 0) {
                    memcpy(frame->data[0], audblock.input_buffer, audblock.input_samples * audblock.inputfmt.channels * sizeof(short));

                    if (av_buffersrc_add_frame(m_buffersrc_ctx, frame) >= 0) {
                        while (true) {
                            AVFrame* out_frame = av_frame_alloc();
                            if (av_buffersink_get_frame(m_buffersink_ctx, out_frame) < 0) {
                                av_frame_free(&out_frame);
                                break;
                            }
                            
                            int out_elements = out_frame->nb_samples * audblock.inputfmt.channels;
                            short* out_data = (short*)out_frame->data[0];
                            m_filter_fifo.insert(m_filter_fifo.end(), out_data, out_data + out_elements);
                            av_frame_free(&out_frame);
                        }
                    }
                }
                av_frame_free(&frame);
            }

            int required_elements = audblock.input_samples * audblock.inputfmt.channels;
            if (m_filter_fifo.size() >= (size_t)required_elements) {
                memcpy(audblock.input_buffer, m_filter_fifo.data(), required_elements * sizeof(short));
                m_filter_fifo.erase(m_filter_fifo.begin(), m_filter_fifo.begin() + required_elements);
            } else {
                return;
            }
        }
    }

    MeasureVoiceLevel(audblock);

    if(audblock.inputfmt.channels == 2)
        SelectStereo(m_stereo, audblock.input_buffer, audblock.input_samples);

    if ((IsVoiceActive() && audblock.voiceact_enc) || audblock.force_enc)
    {
        const char* enc_data = nullptr;
        std::vector<int> enc_frame_sizes;
        switch(m_codec.codec)
        {
        case CODEC_SPEEX :
        case CODEC_SPEEX_VBR :
#if defined(ENABLE_SPEEX)
            enc_data = ProcessSpeex(audblock, enc_frame_sizes);
#endif
            break;
        case CODEC_OPUS :
#if defined(ENABLE_OPUS)
            enc_data = ProcessOPUS(audblock, enc_frame_sizes);
#endif
            break;
        default: break;
        }
        if(enc_data != nullptr)
        {
            int nbBytes = 0;
            for(int enc_frame_size : enc_frame_sizes)
                nbBytes += enc_frame_size;

            m_callback(m_codec, enc_data, nbBytes,
                       enc_frame_sizes, audblock);
        }
        m_enc_cleared = false;
    }
    else
    {
        if(!m_enc_cleared)
        {
#if defined(ENABLE_SPEEX)
            if(m_speex) m_speex->Reset();
#endif
#if defined(ENABLE_OPUS)
            if (m_opus) m_opus->Reset();
#endif
            m_enc_cleared = true;
        }
        m_callback(m_codec, nullptr, 0, std::vector<int>(), audblock);
    }
}

void AudioThread::MeasureVoiceLevel(const media::AudioFrame& audblock)
{
    const int VU_MAX_VOLUME = 8000;
    int lsum = 0, rsum = 0, sum = 0;
    int const samples_total = audblock.input_samples * audblock.inputfmt.channels;
    
    if (audblock.inputfmt.channels == 2)
    {
        for (int i = 0; i < samples_total; i += 2)
        {
            lsum += abs(audblock.input_buffer[i]);
            rsum += abs(audblock.input_buffer[i + 1]);
        }
        switch (m_stereo)
        {
        case STEREO_BOTH: sum = (lsum + rsum) / 2; break;
        case STEREO_LEFT: sum = lsum; break;
        case STEREO_RIGHT: sum = rsum; break;
        case STEREO_NONE: sum = 0; break;
        }
    }
    else
    {
        for (int i = 0; i < samples_total; ++i)
            sum += abs(audblock.input_buffer[i]);
    }
    
    int avg = sum / audblock.input_samples;
    avg = 100 * avg / VU_MAX_VOLUME;
    this->m_voicelevel = avg > VU_METER_MAX ? VU_METER_MAX : avg;

    if (this->m_voicelevel >= this->m_voiceactlevel)
        m_lastActive = ACE_OS::gettimeofday();
}

#if defined(ENABLE_SPEEXDSP)
void AudioThread::PreprocessSpeex(media::AudioFrame& audblock)
{
    std::unique_lock<std::recursive_mutex> const g(m_preprocess_lock);
    if (!m_preprocess_left) return;

    bool preprocess = m_preprocess_left->IsEchoCancel() || m_preprocess_left->IsDenoising() || m_preprocess_left->IsAGC();
    if(!preprocess) return;

    if(audblock.inputfmt.channels == 1)
    {
        if (m_preprocess_left->IsEchoCancel() && audblock.outputfmt.channels == 1 && (audblock.output_buffer != nullptr))
        {
            if(m_echobuf.size() < (size_t)audblock.input_samples)
                m_echobuf.resize(audblock.input_samples);

            m_preprocess_left->EchoCancel(audblock.input_buffer, audblock.output_buffer, m_echobuf.data());
            audblock.input_buffer = m_echobuf.data();
        }
        m_preprocess_left->Preprocess(audblock.input_buffer); 
    }
    else if(audblock.inputfmt.channels == 2)
    {
        assert(m_preprocess_right);
        std::vector<short> in_left(audblock.input_samples), in_right(audblock.input_samples);
        SplitStereo(audblock.input_buffer, audblock.input_samples, in_left, in_right);

        if(m_preprocess_left->IsEchoCancel() && m_preprocess_right->IsEchoCancel() && audblock.outputfmt.channels == 2 && (audblock.output_buffer != nullptr))
        {
            std::vector<short> out_l(audblock.output_samples), out_r(audblock.output_samples);
            std::vector<short> echo_l(audblock.output_samples), echo_r(audblock.output_samples);
            SplitStereo(audblock.output_buffer, audblock.output_samples, out_l, out_r);
            m_preprocess_left->EchoCancel(in_left.data(), out_l.data(), echo_l.data());
            m_preprocess_right->EchoCancel(in_right.data(), out_r.data(), echo_r.data());
            in_left.swap(echo_l); in_right.swap(echo_r);
        }

        m_preprocess_left->Preprocess(in_left.data());
        m_preprocess_right->Preprocess(in_right.data());
        MergeStereo(in_left, in_right, audblock.input_buffer, audblock.input_samples);
    }
}
#endif

#if defined(ENABLE_WEBRTC)
void AudioThread::PreprocessWebRTC(media::AudioFrame& audblock)
{
    std::unique_lock<std::recursive_mutex> const g(m_preprocess_lock);
    if (m_apm) {
        if (WebRTCPreprocess(*m_apm, audblock, audblock, m_aps.get()) != audblock.input_samples) {
            // Error handling
        }
    }
}
#endif

#if defined(ENABLE_SPEEX)
const char* AudioThread::ProcessSpeex(const media::AudioFrame& audblock, std::vector<int>& enc_frame_sizes)
{
    int const framesize = GetAudioCodecFrameSize(m_codec);
    int nbBytes = 0, n_processed = 0;
    int const fpp = GetAudioCodecFramesPerPacket(m_codec);
    if (framesize <= 0 || fpp <= 0) return nullptr;
    int enc_frm_size = int(m_encbuf.size()) / fpp;

    while(n_processed < audblock.input_samples)
    {
        int ret = m_speex->Encode(&audblock.input_buffer[n_processed], &m_encbuf[nbBytes], enc_frm_size);
        if(ret <= 0) return nullptr;
        enc_frame_sizes.push_back(ret);
        n_processed += framesize;
        nbBytes += ret;
    }
    return m_encbuf.data();
}
#endif

#if defined(ENABLE_OPUS)
const char* AudioThread::ProcessOPUS(const media::AudioFrame& audblock, std::vector<int>& enc_frame_sizes)
{
    int const framesize = GetAudioCodecFrameSize(m_codec);
    int const channels = GetAudioCodecChannels(m_codec);
    int const fpp = GetAudioCodecFramesPerPacket(m_codec);
    int nbBytes = 0, n_processed = 0;
    if (framesize <= 0 || fpp <= 0) return nullptr;
    int enc_frm_size = int(m_encbuf.size()) / fpp;

    while(n_processed < audblock.input_samples)
    {
        int ret = m_opus->Encode(&audblock.input_buffer[n_processed*channels], framesize, &m_encbuf[nbBytes], enc_frm_size);
        if(ret <= 0) return nullptr;
        enc_frame_sizes.push_back(ret);
        n_processed += framesize;
        nbBytes += ret;
    }
    return m_encbuf.data();
}
#endif