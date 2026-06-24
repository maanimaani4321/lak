#ifndef LAMEENCODER_H
#define LAMEENCODER_H

#include "codec/MediaUtil.h"
#include "myace/MyACE.h"
#include <lame/lame.h>
#include <memory>
#include <vector>

namespace teamtalk {

    class LameEncoder;
    using lame_encoder_t = std::unique_ptr<LameEncoder>;

    class LameEncoder
    {
    public:
        LameEncoder();
        ~LameEncoder();

        static lame_encoder_t CreateMP3(const media::AudioFormat& format, int bitrate, const ACE_TString& filename);

        bool Init(const media::AudioFormat& format, int bitrate, const ACE_TString& filename);
        void Close();

        int ProcessAudioEncoder(const media::AudioFrame& frame, bool flush);

    private:
        lame_t m_lame = nullptr;
        media::AudioFormat m_format;
        std::vector<unsigned char> m_mp3_buffer;
        MyFile m_file;
        bool m_active = false;
    };
}

#endif