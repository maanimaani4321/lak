/* OboeWrapper.h */
#ifndef OBOEWRAPPER_H
#define OBOEWRAPPER_H

#include "SoundSystemBase.h"
#include <oboe/Oboe.h>
#include <mutex>
#include <vector>

namespace soundsystem {

    struct OboeInputStreamer : InputStreamer, public oboe::AudioStreamDataCallback {
        std::shared_ptr<oboe::AudioStream> stream;
        std::recursive_mutex mutex;
        std::vector<short> buffer; // بافر برای تطبیق سایز فریم Oboe با TeamTalk

        OboeInputStreamer(StreamCapture* r, int sg, int fs, int sr, int chs, SoundAPI sndsys, int devid)
            : InputStreamer(r, sg, fs, sr, chs, sndsys, devid) { }

        oboe::DataCallbackResult onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) override;
    };

    struct OboeOutputStreamer : OutputStreamer, public oboe::AudioStreamDataCallback {
        std::shared_ptr<oboe::AudioStream> stream;
        std::recursive_mutex mutex;
        std::vector<short> buffer; // بافر برای تطبیق سایز فریم TeamTalk با Oboe

        OboeOutputStreamer(StreamPlayer* p, int sg, int fs, int sr, int chs, SoundAPI sndsys, int devid)
            : OutputStreamer(p, sg, fs, sr, chs, sndsys, devid) { }

        oboe::DataCallbackResult onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) override;
    };

    struct OboeSoundGroup : SoundGroup {
        std::recursive_mutex mutex;
        OboeSoundGroup() = default;
    };

    typedef SoundSystemBase< OboeSoundGroup, OboeInputStreamer, OboeOutputStreamer, DuplexStreamer > SSB;

    class OboeWrapper : public SSB {
    private:
        OboeWrapper();
        OboeWrapper(const OboeWrapper&) = delete;
        const OboeWrapper& operator=(const OboeWrapper&) = delete;

    protected:
        bool Init() override;
        void Close() override;
        void FillDevices(sounddevices_t& sounddevs) override;

        soundgroup_t NewSoundGroup() override;
        void RemoveSoundGroup(soundgroup_t sndgrp) override;

        // Input
        inputstreamer_t NewStream(StreamCapture* capture, int inputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) override;
        bool StartStream(inputstreamer_t streamer) override;
        void CloseStream(inputstreamer_t streamer) override;
        bool IsStreamStopped(inputstreamer_t streamer) override;
        bool UpdateStreamCaptureFeatures(inputstreamer_t streamer) override;

        // Output
        outputstreamer_t NewStream(StreamPlayer* player, int outputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) override;
        void CloseStream(outputstreamer_t streamer) override;
        bool StartStream(outputstreamer_t streamer) override;
        bool StopStream(outputstreamer_t streamer) override;
        bool IsStreamStopped(outputstreamer_t streamer) override;

        // Duplex 
        duplexstreamer_t NewStream(StreamDuplex* duplex, int inputdeviceid, int outputdeviceid, int sndgrpid, int samplerate, int input_channels, int output_channels, int framesize) override { return nullptr; }
        void CloseStream(duplexstreamer_t streamer) override { }
        bool StartStream(duplexstreamer_t streamer) override { return false; }
        bool IsStreamStopped(duplexstreamer_t streamer) override { return true; }
        bool AddDuplexOutputStream(StreamDuplex* duplex, StreamPlayer* player) override { return false; }
        bool RemoveDuplexOutputStream(StreamDuplex* duplex, StreamPlayer* player) override { return false; }

    public:
        virtual ~OboeWrapper();
        static std::shared_ptr<OboeWrapper> getInstance();

        bool GetDefaultDevices(int& inputdeviceid, int& outputdeviceid) override;
        bool GetDefaultDevices(SoundAPI sndsys, int& inputdeviceid, int& outputdeviceid) override;
    };

    typedef SSB::soundgroup_t soundgroup_t;
    typedef SSB::inputstreamer_t inputstreamer_t;
    typedef SSB::outputstreamer_t outputstreamer_t;

}

#endif // OBOEWRAPPER_H