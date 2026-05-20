/* OboeWrapper.cpp */
#include "OboeWrapper.h"
#include "myace/MyACE.h"
#include <cassert>
#include <cstring>
#include <algorithm>

namespace soundsystem {

enum AndroidSoundDevice {
    DEFAULT_DEVICE_ID  = (0 & SOUND_DEVICEID_MASK),
    VOICECOM_DEVICE_ID = (1 & SOUND_DEVICEID_MASK),
};

constexpr auto DEFAULT_SAMPLERATE = 48000;

OboeWrapper::OboeWrapper() {
    Init();
}

OboeWrapper::~OboeWrapper() {
    Close();
    MYTRACE(ACE_TEXT("~OboeWrapper()\n"));
}

bool OboeWrapper::Init() {
    RefreshDevices();
    MYTRACE(ACE_TEXT("Initializing Oboe (AAudio/OpenSL ES)\n"));
    return true;
}

void OboeWrapper::Close() {
    MYTRACE(ACE_TEXT("Closed Oboe\n"));
}

std::shared_ptr<OboeWrapper> OboeWrapper::getInstance() {
    static std::shared_ptr<OboeWrapper> p(new OboeWrapper());
    return p;
}

soundgroup_t OboeWrapper::NewSoundGroup() {
    return std::make_shared<OboeSoundGroup>();
}

void OboeWrapper::RemoveSoundGroup(soundgroup_t grp) {
}

bool OboeWrapper::GetDefaultDevices(int& inputdeviceid, int& outputdeviceid) {
    return GetDefaultDevices(SOUND_API_OBOE_ANDROID, inputdeviceid, outputdeviceid);
}

bool OboeWrapper::GetDefaultDevices(SoundAPI sndsys, int& inputdeviceid, int& outputdeviceid) {
    if(sndsys == SOUND_API_OBOE_ANDROID) {
        inputdeviceid = outputdeviceid = DEFAULT_DEVICE_ID;
        return true;
    }
    return false;
}

void OboeWrapper::FillDevices(sounddevices_t& sounddevs) {
    DeviceInfo dev;
    dev.devicename = ACE_TEXT("Default sound device (Oboe)");
    dev.soundsystem = SOUND_API_OBOE_ANDROID;
    dev.id = DEFAULT_DEVICE_ID;
    
    for(int sr : standardSampleRates) {
        dev.input_samplerates.insert(sr);
        dev.output_samplerates.insert(sr);
    }
    
    dev.max_input_channels = 2;
    dev.max_output_channels = 2;
    dev.input_channels.insert(1);
    dev.input_channels.insert(2);
    dev.output_channels.insert(1);
    dev.output_channels.insert(2);
    dev.default_samplerate = DEFAULT_SAMPLERATE;

    // دیوایس 0
    sounddevs[dev.id] = dev;

    // دیوایس 1 (دارای حذف اکو و نویز سخت‌افزاری)
    DeviceInfo voicecom_dev = dev;
    voicecom_dev.id = VOICECOM_DEVICE_ID;
    voicecom_dev.devicename = ACE_TEXT("Voice Communication Sound Device (Oboe)");
    voicecom_dev.features |= SOUNDDEVICEFEATURE_AEC;
    voicecom_dev.features |= SOUNDDEVICEFEATURE_AGC;
    voicecom_dev.features |= SOUNDDEVICEFEATURE_DENOISE;
    sounddevs[voicecom_dev.id] = voicecom_dev;
}

oboe::DataCallbackResult OboeInputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    std::lock_guard<std::recursive_mutex> g(mutex);
    
    short* pcmData = static_cast<short*>(audioData);
    int totalNewSamples = numFrames * channels;

    // ذخیره فریم‌های جدیدِ Oboe در بافر واسط
    buffer.insert(buffer.end(), pcmData, pcmData + totalNewSamples);

    int requiredSamples = framesize * channels;

    // ارسال به TeamTalk فقط زمانی که دقیقاً به اندازه سایز استانداردِ تیم‌تاک دیتا جمع شده باشد
    while (buffer.size() >= (size_t)requiredSamples) {
        recorder->StreamCaptureCb(*this, buffer.data(), framesize);
        buffer.erase(buffer.begin(), buffer.begin() + requiredSamples);
    }
    
    return oboe::DataCallbackResult::Continue;
}

inputstreamer_t OboeWrapper::NewStream(StreamCapture* capture, int inputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeInputStreamer>(capture, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, inputdeviceid);

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setDataCallback(streamer.get());

    if (inputdeviceid == VOICECOM_DEVICE_ID || (capture->GetCaptureFeatures() & SOUNDDEVICEFEATURE_AEC)) {
        builder.setInputPreset(oboe::InputPreset::VoiceCommunication);
        MYTRACE(ACE_TEXT("Oboe: Activated VoiceCommunication preset for Hardware AEC/NS\n"));
    } else {
        builder.setInputPreset(oboe::InputPreset::Generic);
    }

    oboe::Result result = builder.openStream(streamer->stream);
    if (result != oboe::Result::OK) {
        MYTRACE(ACE_TEXT("Failed to create Oboe audio recorder: %s\n"), oboe::convertToText(result));
        return nullptr;
    }

    MYTRACE(ACE_TEXT("Opened Oboe capture stream %p, samplerate %d, channels %d\n"), capture, streamer->stream->getSampleRate(), streamer->stream->getChannelCount());
    return streamer;
}

bool OboeWrapper::StartStream(inputstreamer_t streamer) {
    if (!streamer->stream) return false;
    return streamer->stream->requestStart() == oboe::Result::OK;
}

void OboeWrapper::CloseStream(inputstreamer_t streamer) {
    if (streamer->stream) {
        streamer->stream->requestStop();
        streamer->stream->close();
        streamer->stream.reset();
        MYTRACE(ACE_TEXT("Closed Oboe capture stream\n"));
    }
}

bool OboeWrapper::IsStreamStopped(inputstreamer_t streamer) {
    if (!streamer->stream) return true;
    return streamer->stream->getState() == oboe::StreamState::Stopped || 
           streamer->stream->getState() == oboe::StreamState::Closed ||
           streamer->stream->getState() == oboe::StreamState::Stopping;
}

bool OboeWrapper::UpdateStreamCaptureFeatures(inputstreamer_t streamer) {
    return true; 
}


oboe::DataCallbackResult OboeOutputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    std::lock_guard<std::recursive_mutex> g(mutex);
    
    short* outData = static_cast<short*>(audioData);
    int requiredSamplesForOboe = numFrames * channels;
    int teamTalkChunkSamples = framesize * channels;
    
    bool more = true;

    // پر کردن بافر واسط تا زمانی که به اندازه درخواست Oboe دیتا داشته باشیم
    while (buffer.size() < (size_t)requiredSamplesForOboe && more) {
        std::vector<short> tmpChunk(teamTalkChunkSamples, 0);
        
        more = player->StreamPlayerCb(*this, tmpChunk.data(), framesize);
        
        int mastervol = OboeWrapper::getInstance()->GetMasterVolume(sndgrpid);
        bool mastermute = OboeWrapper::getInstance()->IsAllMute(sndgrpid);
        SoftVolume(*this, tmpChunk.data(), framesize, mastervol, mastermute);

        buffer.insert(buffer.end(), tmpChunk.begin(), tmpChunk.end());
    }

    // کپی کردن دیتا برای پخش در اسپیکر
    int copySamples = std::min((int)buffer.size(), requiredSamplesForOboe);
    std::memcpy(outData, buffer.data(), copySamples * sizeof(short));
    buffer.erase(buffer.begin(), buffer.begin() + copySamples);

    // اگر دیتای کافی وجود نداشت (مثلاً پایان استریم)، مابقی را با سکوت (صفر) پر می‌کنیم تا صدای ناهنجار تولید نشود
    if (copySamples < requiredSamplesForOboe) {
        std::memset(outData + copySamples, 0, (requiredSamplesForOboe - copySamples) * sizeof(short));
    }

    return more ? oboe::DataCallbackResult::Continue : oboe::DataCallbackResult::Stop;
}

outputstreamer_t OboeWrapper::NewStream(soundsystem::StreamPlayer* player, int outputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeOutputStreamer>(player, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, outputdeviceid);

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setDataCallback(streamer.get());

    oboe::Result result = builder.openStream(streamer->stream);
    if (result != oboe::Result::OK) {
        MYTRACE(ACE_TEXT("Failed to create Oboe audio player: %s\n"), oboe::convertToText(result));
        return nullptr;
    }

    MYTRACE(ACE_TEXT("Opened Oboe playback, samplerate %d, channels %d\n"), streamer->stream->getSampleRate(), streamer->stream->getChannelCount());
    return streamer;
}

void OboeWrapper::CloseStream(outputstreamer_t streamer) {
    if (streamer->stream) {
        streamer->stream->requestStop();
        streamer->stream->close();
        streamer->stream.reset();
        MYTRACE(ACE_TEXT("Closed Oboe playback stream\n"));
    }
}

bool OboeWrapper::StartStream(outputstreamer_t streamer) {
    if (!streamer->stream) return false;
    return streamer->stream->requestStart() == oboe::Result::OK;
}

bool OboeWrapper::StopStream(outputstreamer_t streamer) {
    if (!streamer->stream) return false;
    return streamer->stream->requestStop() == oboe::Result::OK;
}

bool OboeWrapper::IsStreamStopped(outputstreamer_t streamer) {
    if (!streamer->stream) return true;
    return streamer->stream->getState() == oboe::StreamState::Stopped || 
           streamer->stream->getState() == oboe::StreamState::Closed ||
           streamer->stream->getState() == oboe::StreamState::Stopping;
}

} // namespace soundsystem