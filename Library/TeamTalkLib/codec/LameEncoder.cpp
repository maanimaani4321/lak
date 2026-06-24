#include "LameEncoder.h"
#include "teamtalk/Log.h"
#include <cassert>

namespace teamtalk {

    LameEncoder::LameEncoder()
    {
    }

    LameEncoder::~LameEncoder()
    {
        Close();
    }

    lame_encoder_t LameEncoder::CreateMP3(const media::AudioFormat& format, int bitrate, const ACE_TString& filename)
    {
        std::unique_ptr<LameEncoder> encoder = std::make_unique<LameEncoder>();
        if (encoder->Init(format, bitrate, filename))
        {
            return encoder;
        }
        return nullptr;
    }

    bool LameEncoder::Init(const media::AudioFormat& format, int bitrate, const ACE_TString& filename)
    {
        if (m_lame != nullptr || m_active)
            return false;

        if (!m_file.NewFile(filename))
            return false;

        m_lame = lame_init();
        if (m_lame == nullptr)
        {
            m_file.Close();
            return false;
        }

        lame_set_in_samplerate(m_lame, format.samplerate);
        lame_set_num_channels(m_lame, format.channels);
        lame_set_mode(m_lame, format.channels == 2 ? JOINT_STEREO : MONO);
        lame_set_brate(m_lame, bitrate / 1000); // LAME بیت‌ریت بر حسب kbps می‌خواهد
        lame_set_quality(m_lame, 2); // کیفیت بالا

        if (lame_init_params(m_lame) < 0)
        {
            Close();
            return false;
        }

        m_format = format;
        m_active = true;

        // حداکثر حجم احتمالی برای بافر خروجی فریم بر اساس فرکانس و متدهای محاسباتی LAME
        int max_mp3_size = static_cast<int>(1.25 * format.samplerate + 7200);
        m_mp3_buffer.resize(max_mp3_size);

        return true;
    }

    void LameEncoder::Close()
    {
        if (m_lame != nullptr)
        {
            if (m_active && m_file.Tell() > 0)
            {
                int write_bytes = lame_encode_flush(m_lame, m_mp3_buffer.data(), m_mp3_buffer.size());
                if (write_bytes > 0)
                {
                    m_file.Write(reinterpret_cast<const char*>(m_mp3_buffer.data()), write_bytes);
                }
            }
            lame_close(m_lame);
            m_lame = nullptr;
        }

        m_file.Close();
        m_active = false;
    }

    int LameEncoder::ProcessAudioEncoder(const media::AudioFrame& frame, bool flush)
    {
        if (m_lame == nullptr || !m_active)
            return -1;

        int write_bytes = 0;
        if (frame.input_samples > 0)
        {
            if (m_format.channels == 2)
            {
                write_bytes = lame_encode_buffer_interleaved(m_lame, 
                                                            frame.input_buffer, 
                                                            frame.input_samples, 
                                                            m_mp3_buffer.data(), 
                                                            static_cast<int>(m_mp3_buffer.size()));
            }
            else
            {
                write_bytes = lame_encode_buffer(m_lame, 
                                                 frame.input_buffer, 
                                                 nullptr, 
                                                 frame.input_samples, 
                                                 m_mp3_buffer.data(), 
                                                 static_cast<int>(m_mp3_buffer.size()));
            }

            if (write_bytes < 0)
            {
                return -1;
            }

            if (write_bytes > 0)
            {
                m_file.Write(reinterpret_cast<const char*>(m_mp3_buffer.data()), write_bytes);
            }
        }

        if (flush)
        {
            int flush_bytes = lame_encode_flush(m_lame, m_mp3_buffer.data(), m_mp3_buffer.size());
            if (flush_bytes > 0)
            {
                m_file.Write(reinterpret_cast<const char*>(m_mp3_buffer.data()), flush_bytes);
                write_bytes += flush_bytes;
            }
        }

        return write_bytes;
    }
}