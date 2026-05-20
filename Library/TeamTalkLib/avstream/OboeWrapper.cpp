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

#include "OboeWrapper.h"
#include "SoundSystemEx.h"
#include "myace/MyACE.h"
#include <cassert>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>

namespace soundsystem {

enum AndroidSoundDevice {
    DEFAULT_DEVICE_ID           = (0 & SOUND_DEVICEID_MASK),
    VOICECOM_DEVICE_ID          = (1 & SOUND_DEVICEID_MASK),
    GENERIC_PROCESSED_DEVICE_ID = (1380 & SOUND_DEVICEID_MASK), 
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

// ---------------------------------------------------------
// Thread Safe Validation & Reconnect Logic
// ---------------------------------------------------------
bool OboeWrapper::IsCaptureValid(StreamCapture* capture) {
    std::lock_guard<std::recursive_mutex> g(CaptureLock());
    // استفاده از پوینتر بومی کلاس والد به جای لوپ‌های مکرر فرعی صوتی
    return static_cast<bool>(GetStream(capture, true, false));
}

bool OboeWrapper::IsPlayerValid(StreamPlayer* player) {
    std::lock_guard<std::recursive_mutex> g(PlayersLock());
    return static_cast<bool>(GetStream(player));
}

void OboeWrapper::SafeRestartInputStream(StreamCapture* capture) {
    std::lock_guard<std::recursive_mutex> g(CaptureLock());
    inputstreamer_t streamer = GetStream(capture, true, false);
    if (streamer) {
        int sndgrpid = streamer->sndgrpid;
        int samplerate = streamer->samplerate;
        int channels = streamer->channels;
        int framesize = streamer->framesize;
        
        CloseInputStream(capture);
        OpenInputStream(capture, DEFAULT_DEVICE_ID, sndgrpid, samplerate, channels, framesize);
    }
}

void OboeWrapper::SafeRestartOutputStream(StreamPlayer* player) {
    std::lock_guard<std::recursive_mutex> g(PlayersLock());
    outputstreamer_t streamer = GetStream(player);
    if (streamer) {
        int sndgrpid = streamer->sndgrpid;
        int samplerate = streamer->samplerate;
        int channels = streamer->channels;
        int framesize = streamer->framesize;
        
        CloseOutputStream(player);
        OpenOutputStream(player, DEFAULT_DEVICE_ID, sndgrpid, samplerate, channels, framesize);
        StartStream(player); 
    }
}

// ---------------------------------------------------------
soundgroup_t OboeWrapper::NewSoundGroup() {
    return std::make_shared<OboeSoundGroup>();
}

void OboeWrapper::RemoveSoundGroup(soundgroup_t) {
}

bool OboeWrapper::GetDefaultDevices(int& inputdeviceid, int& outputdeviceid) {
    return GetDefaultDevices(SOUND_API_OBOE_ANDROID, inputdeviceid, outputdeviceid);
}

bool OboeWrapper::GetDefaultDevices(SoundAPI sndsys, int& inputdeviceid, int& outputdeviceid) {
    if (sndsys == SOUND_API_OBOE_ANDROID) {
        inputdeviceid = outputdeviceid = DEFAULT_DEVICE_ID;
        return true;
    }
    return false;
}

void OboeWrapper::FillDevices(sounddevices_t& sounddevs) {
    DeviceInfo dev;
    dev.devicename = ACE_TEXT("Default (OpenSL ES / Raw)");
    dev.soundsystem = SOUND_API_OBOE_ANDROID;
    dev.id = DEFAULT_DEVICE_ID;
    
    for (int sr : standardSampleRates) {
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

    // ثبت دیوایس 0
    dev.features |= SOUNDDEVICEFEATURE_AEC | SOUNDDEVICEFEATURE_AGC | SOUNDDEVICEFEATURE_DENOISE;
    sounddevs[dev.id] = dev;

    // ثبت دیوایس 1
    DeviceInfo voicecom_dev = dev;
    voicecom_dev.id = VOICECOM_DEVICE_ID;
    voicecom_dev.devicename = ACE_TEXT("Voice Communication (Hardware AEC/NS)");
    sounddevs[voicecom_dev.id] = voicecom_dev;

    // ثبت دیوایس 1380
    DeviceInfo generic_dev = dev;
    generic_dev.id = GENERIC_PROCESSED_DEVICE_ID;
    generic_dev.devicename = ACE_TEXT("Processed Voice (No Delay)");
    sounddevs[generic_dev.id] = generic_dev;
}

// ---------------------------------------------------------
// INPUT STREAMER (Microphone)
// ---------------------------------------------------------
oboe::DataCallbackResult OboeInputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    short* pcmData = static_cast<short*>(audioData);
    int totalIncomingSamples = numFrames * channels;
    int requiredSamples = framesize * channels;

    if (fifo_size + totalIncomingSamples <= fifo_capacity) {
        std::memcpy(&fifo_buffer[fifo_size], pcmData, totalIncomingSamples * sizeof(short));
        fifo_size += totalIncomingSamples;
    } else {
        MYTRACE(ACE_TEXT("Oboe Input: Buffer Overrun avoided.\n"));
    }

    while (fifo_size >= requiredSamples) {
        recorder->StreamCaptureCb(*this, fifo_buffer.data(), framesize);
        
        fifo_size -= requiredSamples;
        if (fifo_size > 0) {
            std::memmove(&fifo_buffer[0], &fifo_buffer[requiredSamples], fifo_size * sizeof(short));
        }
    }
    
    return oboe::DataCallbackResult::Continue;
}

void OboeInputStreamer::onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) {
    MYTRACE(ACE_TEXT("Oboe Input Stream closed. Error: %s\n"), oboe::convertToText(error));
    if (error == oboe::Result::ErrorDisconnected) {
        StreamCapture* cachedRecorder = this->recorder;
        std::thread([cachedRecorder]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::shared_ptr<OboeWrapper> sndSys = OboeWrapper::getInstance();
            if (sndSys && sndSys->IsCaptureValid(cachedRecorder)) {
                sndSys->SafeRestartInputStream(cachedRecorder);
            }
        }).detach();
    }
}

inputstreamer_t OboeWrapper::NewStream(StreamCapture* capture, int inputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeInputStreamer>(capture, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, inputdeviceid);

    // پیش‌تخصیص ظرفیت بافر به اندازه 2 ثانیه صدا جهت عدم اجرای resize حین ضبط Real-time
    streamer->fifo_capacity = samplerate * channels * 2;
    streamer->fifo_buffer.resize(streamer->fifo_capacity, 0);
    streamer->fifo_size = 0;

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setDataCallback(streamer.get());
    builder.setErrorCallback(streamer.get());

    builder.setChannelConversionAllowed(true);
    builder.setFormatConversionAllowed(true);
    builder.setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);

    if (inputdeviceid == DEFAULT_DEVICE_ID) {
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        builder.setInputPreset(oboe::InputPreset::Generic);
        MYTRACE(ACE_TEXT("Oboe Input (Default 0): OpenSL ES backend with Generic mic preset\n"));
    } else if (inputdeviceid == VOICECOM_DEVICE_ID) {
        builder.setInputPreset(oboe::InputPreset::VoiceCommunication);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        MYTRACE(ACE_TEXT("Oboe Input (VoiceCom 1): Mic preset = VoiceCommunication\n"));
    } else if (inputdeviceid == GENERIC_PROCESSED_DEVICE_ID) {
        builder.setInputPreset(oboe::InputPreset::Unprocessed);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive);
        MYTRACE(ACE_TEXT("Oboe Input (NoDelay 1380): Mic preset = Unprocessed, LowLatency, Exclusive\n"));
    } else {
        builder.setInputPreset(oboe::InputPreset::Unprocessed);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setDeviceId(inputdeviceid);
    }

    oboe::Result result = builder.openStream(streamer->stream);
    
    if (result != oboe::Result::OK && inputdeviceid != DEFAULT_DEVICE_ID) {
        builder.setDeviceId(oboe::kUnspecified);
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        builder.setInputPreset(oboe::InputPreset::Generic);
        result = builder.openStream(streamer->stream);
    }

    if (result != oboe::Result::OK) {
        return nullptr;
    }

    return streamer;
}

bool OboeWrapper::StartStream(inputstreamer_t streamer) {
    if (!streamer || !streamer->stream) return false;
    return streamer->stream->requestStart() == oboe::Result::OK;
}

void OboeWrapper::CloseStream(inputstreamer_t streamer) {
    if (streamer && streamer->stream) {
        streamer->stream->requestStop();
        streamer->stream->close();
        streamer->stream.reset();
    }
}

bool OboeWrapper::IsStreamStopped(inputstreamer_t streamer) {
    if (!streamer || !streamer->stream) return true;
    return streamer->stream->getState() == oboe::StreamState::Stopped || 
           streamer->stream->getState() == oboe::StreamState::Closed ||
           streamer->stream->getState() == oboe::StreamState::Stopping;
}

bool OboeWrapper::UpdateStreamCaptureFeatures(inputstreamer_t streamer) {
    std::lock_guard<std::recursive_mutex> g(CaptureLock());
    if (!streamer || !streamer->stream) return false;

    // خواندن داینامیک وضعیت ویژگی‌های درخواستی تیم‌تاک (AEC, AGC, NS)
    SoundDeviceFeatures features = streamer->recorder->GetCaptureFeatures();
    
    streamer->stream->requestStop();
    streamer->stream->close();
    streamer->stream.reset();

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(streamer->channels);
    builder.setSampleRate(streamer->samplerate);
    builder.setDataCallback(streamer.get());
    builder.setErrorCallback(streamer.get());
    
    if ((features & SOUNDDEVICEFEATURE_AEC) || (features & SOUNDDEVICEFEATURE_AGC) || (features & SOUNDDEVICEFEATURE_DENOISE)) {
        builder.setInputPreset(oboe::InputPreset::VoiceCommunication);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        MYTRACE(ACE_TEXT("Oboe: Reconfigured to VoiceCommunication due to requested Dynamic Features.\n"));
    } else {
        if (streamer->inputdeviceid == DEFAULT_DEVICE_ID) {
            builder.setAudioApi(oboe::AudioApi::OpenSLES);
            builder.setPerformanceMode(oboe::PerformanceMode::None);
            builder.setInputPreset(oboe::InputPreset::Generic);
            MYTRACE(ACE_TEXT("Oboe: Reconfigured to Generic (OpenSLES) due to disabled Dynamic Features.\n"));
        } else {
            builder.setInputPreset(oboe::InputPreset::Unprocessed);
            builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
            builder.setDeviceId(streamer->inputdeviceid);
        }
    }

    if (builder.openStream(streamer->stream) == oboe::Result::OK) {
        streamer->stream->requestStart();
        return true;
    }
    return false;
}

// ---------------------------------------------------------
// OUTPUT STREAMER (Speaker / Earpiece)
// ---------------------------------------------------------
oboe::DataCallbackResult OboeOutputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    short* outData = static_cast<short*>(audioData);
    int totalOutgoingSamples = numFrames * channels;
    int requiredSamples = framesize * channels;
    
    bool more = true;

    while (fifo_size < totalOutgoingSamples && more) {
        more = player->StreamPlayerCb(*this, cb_buffer.data(), framesize);
        
        // استفاده از اشاره‌گر خام جهت دوری از قفل‌های اتمیک Reference Counting کاندید بر روی ترد صوتی
        OboeWrapper* sndSys = OboeWrapper::getInstance().get();
        if (sndSys) {
            int mastervol = sndSys->GetMasterVolume(sndgrpid);
            bool mastermute = sndSys->IsAllMute(sndgrpid);
            SoftVolume(*this, cb_buffer.data(), framesize, mastervol, mastermute);
        }

        if (fifo_size + requiredSamples <= fifo_capacity) {
            std::memcpy(&fifo_buffer[fifo_size], cb_buffer.data(), requiredSamples * sizeof(short));
            fifo_size += requiredSamples;
        } else {
            break; 
        }
    }

    int samplesToCopy = std::min(totalOutgoingSamples, fifo_size);
    if (samplesToCopy > 0) {
        std::memcpy(outData, fifo_buffer.data(), samplesToCopy * sizeof(short));
        fifo_size -= samplesToCopy;
        if (fifo_size > 0) {
            std::memmove(&fifo_buffer[0], &fifo_buffer[samplesToCopy], fifo_size * sizeof(short));
        }
    }

    if (samplesToCopy < totalOutgoingSamples) {
        std::memset(outData + samplesToCopy, 0, (totalOutgoingSamples - samplesToCopy) * sizeof(short));
    }

    return more ? oboe::DataCallbackResult::Continue : oboe::DataCallbackResult::Stop;
}

void OboeOutputStreamer::onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) {
    MYTRACE(ACE_TEXT("Oboe Output Stream closed. Error: %s\n"), oboe::convertToText(error));
    if (error == oboe::Result::ErrorDisconnected) {
        StreamPlayer* cachedPlayer = this->player;
        std::thread([cachedPlayer]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::shared_ptr<OboeWrapper> sndSys = OboeWrapper::getInstance();
            if (sndSys && sndSys->IsPlayerValid(cachedPlayer)) {
                sndSys->SafeRestartOutputStream(cachedPlayer);
            }
        }).detach();
    }
}

outputstreamer_t OboeWrapper::NewStream(soundsystem::StreamPlayer* player, int outputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeOutputStreamer>(player, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, outputdeviceid);

    streamer->cb_buffer.resize(framesize * channels);
    streamer->fifo_capacity = samplerate * channels * 2;
    streamer->fifo_buffer.resize(streamer->fifo_capacity, 0);
    streamer->fifo_size = 0;

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setDataCallback(streamer.get());
    builder.setErrorCallback(streamer.get());

    builder.setChannelConversionAllowed(true);
    builder.setFormatConversionAllowed(true);
    builder.setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);

    if (outputdeviceid == DEFAULT_DEVICE_ID) {
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        builder.setUsage(oboe::Usage::Media);
        builder.setContentType(oboe::ContentType::Music);
        MYTRACE(ACE_TEXT("Oboe Playback (Default 0): OpenSL ES backend with Media/Music usage\n"));
    } else if (outputdeviceid == VOICECOM_DEVICE_ID) {
        builder.setUsage(oboe::Usage::VoiceCommunication);
        builder.setContentType(oboe::ContentType::Speech);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    } else if (outputdeviceid == GENERIC_PROCESSED_DEVICE_ID) {
        builder.setUsage(oboe::Usage::Media);
        builder.setContentType(oboe::ContentType::Music);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive);
    } else {
        builder.setUsage(oboe::Usage::Media);
        builder.setContentType(oboe::ContentType::Music);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setDeviceId(outputdeviceid);
    }

    oboe::Result result = builder.openStream(streamer->stream);

    if (result != oboe::Result::OK && outputdeviceid != DEFAULT_DEVICE_ID) {
        builder.setDeviceId(oboe::kUnspecified);
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        result = builder.openStream(streamer->stream);
    }

    if (result != oboe::Result::OK) {
        return nullptr;
    }

    return streamer;
}

void OboeWrapper::CloseStream(outputstreamer_t streamer) {
    if (streamer && streamer->stream) {
        streamer->stream->requestStop();
        streamer->stream->close();
        streamer->stream.reset();
    }
}

bool OboeWrapper::StartStream(outputstreamer_t streamer) {
    if (!streamer || !streamer->stream) return false;
    return streamer->stream->requestStart() == oboe::Result::OK;
}

bool OboeWrapper::StopStream(outputstreamer_t streamer) {
    if (!streamer || !streamer->stream) return false;
    return streamer->stream->requestStop() == oboe::Result::OK;
}

bool OboeWrapper::IsStreamStopped(outputstreamer_t streamer) {
    if (!streamer || !streamer->stream) return true;
    return streamer->stream->getState() == oboe::StreamState::Stopped || 
           streamer->stream->getState() == oboe::StreamState::Closed ||
           streamer->stream->getState() == oboe::StreamState::Stopping;
}

} // namespace soundsystem