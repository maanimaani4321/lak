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
    MYTRACE(ACE_TEXT("Initializing Oboe\n"));
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

    // دیوایس 0 برای استودیو (خام)
    sounddevs[dev.id] = dev;

    // دیوایس 1 برای مکالمه (سخت‌افزاری)
    DeviceInfo voicecom_dev = dev;
    voicecom_dev.id = VOICECOM_DEVICE_ID;
    voicecom_dev.devicename = ACE_TEXT("Voice Communication (Hardware AEC/NS)");
    voicecom_dev.features |= SOUNDDEVICEFEATURE_AEC;
    voicecom_dev.features |= SOUNDDEVICEFEATURE_AGC;
    voicecom_dev.features |= SOUNDDEVICEFEATURE_DENOISE;
    sounddevs[voicecom_dev.id] = voicecom_dev;
}

// ---------------------------------------------------------
// INPUT STREAMER (Microphone)
// ---------------------------------------------------------
oboe::DataCallbackResult OboeInputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    std::lock_guard<std::recursive_mutex> g(mutex);
    
    short* pcmData = static_cast<short*>(audioData);
    int totalIncomingSamples = numFrames * channels;
    int requiredSamples = framesize * channels;

    // کپی سریع در انتهای بافر استاتیک
    if (fifo_size + totalIncomingSamples > buffer.size()) {
        buffer.resize(fifo_size + totalIncomingSamples + (requiredSamples * 2));
    }
    std::memcpy(&buffer[fifo_size], pcmData, totalIncomingSamples * sizeof(short));
    fifo_size += totalIncomingSamples;

    // پمپ کردن دیتای استاندارد به هسته تیم‌تاک
    while (fifo_size >= requiredSamples) {
        recorder->StreamCaptureCb(*this, buffer.data(), framesize);
        
        // شیفت دادن بافر به عقب (مانند RingBuffer رفتار می‌کند)
        fifo_size -= requiredSamples;
        if (fifo_size > 0) {
            std::memmove(&buffer[0], &buffer[requiredSamples], fifo_size * sizeof(short));
        }
    }
    
    return oboe::DataCallbackResult::Continue;
}

inputstreamer_t OboeWrapper::NewStream(StreamCapture* capture, int inputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeInputStreamer>(capture, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, inputdeviceid);

    // پیش‌فرض بافر را بزرگ می‌گیریم تا نیازی به resize مداوم نباشد
    streamer->buffer.resize(framesize * channels * 4);
    streamer->fifo_size = 0;

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setDataCallback(streamer.get());

    // حل مشکل قفل شدن روی مکالمه: فقط اگر دیوایس 1 انتخاب شده باشد، مکالمه روشن می‌شود!
    if (inputdeviceid == VOICECOM_DEVICE_ID) {
        builder.setInputPreset(oboe::InputPreset::VoiceCommunication);
        MYTRACE(ACE_TEXT("Oboe: Mic preset = VoiceCommunication\n"));
    } else {
        builder.setInputPreset(oboe::InputPreset::Unprocessed);
        MYTRACE(ACE_TEXT("Oboe: Mic preset = Unprocessed (Raw)\n"));
    }

    oboe::Result result = builder.openStream(streamer->stream);
    if (result != oboe::Result::OK) {
        return nullptr;
    }

    MYTRACE(ACE_TEXT("Opened Oboe Mic\n"));
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


// ---------------------------------------------------------
// OUTPUT STREAMER (Speaker / Earpiece)
// ---------------------------------------------------------
oboe::DataCallbackResult OboeOutputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    std::lock_guard<std::recursive_mutex> g(mutex);
    
    short* outData = static_cast<short*>(audioData);
    int totalOutgoingSamples = numFrames * channels;
    int requiredSamples = framesize * channels;
    
    bool more = true;
    int samplesGenerated = 0;

    // تا زمانی که درخواستِ Oboe پر شود، از تیم‌تاک دیتا می‌گیریم
    while (samplesGenerated < totalOutgoingSamples && more) {
        // تیم‌تاک دقیقاً به اندازه framesize به ما دیتا می‌دهد
        more = player->StreamPlayerCb(*this, cb_buffer.data(), framesize);
        
        int mastervol = OboeWrapper::getInstance()->GetMasterVolume(sndgrpid);
        bool mastermute = OboeWrapper::getInstance()->IsAllMute(sndgrpid);
        SoftVolume(*this, cb_buffer.data(), framesize, mastervol, mastermute);

        // کپی در بافر خروجی نهایی Oboe
        int chunkToCopy = std::min(requiredSamples, totalOutgoingSamples - samplesGenerated);
        std::memcpy(outData + samplesGenerated, cb_buffer.data(), chunkToCopy * sizeof(short));
        samplesGenerated += chunkToCopy;
    }

    // اگر دیتای کافی تولید نشد (استریم قطع شد)، مابقی را با سکوت (صفر) پر می‌کنیم تا صدای ناهنجار تولید نشود
    if (samplesGenerated < totalOutgoingSamples) {
        std::memset(outData + samplesGenerated, 0, (totalOutgoingSamples - samplesGenerated) * sizeof(short));
    }

    return more ? oboe::DataCallbackResult::Continue : oboe::DataCallbackResult::Stop;
}

outputstreamer_t OboeWrapper::NewStream(soundsystem::StreamPlayer* player, int outputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeOutputStreamer>(player, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, outputdeviceid);

    // بافری دقیقاً منطبق با سایز مورد نیاز هسته TeamTalk
    streamer->cb_buffer.resize(framesize * channels);

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    // استفاده از LowLatency بهترین گزینه برای وویس چت است
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setDataCallback(streamer.get());

    // در اندروید، برای پخش صدای تماس باید از حالت UsageVoiceCommunication استفاده شود
    // این کار از طریق لایه جاوا (AudioManager) تغییر اسپیکر/گوشی را راحت‌تر می‌کند
    builder.setUsage(oboe::Usage::VoiceCommunication);
    builder.setContentType(oboe::ContentType::Speech);

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