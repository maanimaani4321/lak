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
    GENERIC_PROCESSED_DEVICE_ID = (1380 & SOUND_DEVICEID_MASK), // شناسه انتخابی جدید شما
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
    
    // ارسال تمامی سمپل‌ریت‌ها تا تیم‌تاک متوجه پشتیبانی آن‌ها شود
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
    sounddevs[dev.id] = dev;

    // ثبت دیوایس 1
    DeviceInfo voicecom_dev = dev;
    voicecom_dev.id = VOICECOM_DEVICE_ID;
    voicecom_dev.devicename = ACE_TEXT("Voice Communication (Hardware AEC/NS)");
    voicecom_dev.features |= SOUNDDEVICEFEATURE_AEC;
    voicecom_dev.features |= SOUNDDEVICEFEATURE_AGC;
    voicecom_dev.features |= SOUNDDEVICEFEATURE_DENOISE;
    sounddevs[voicecom_dev.id] = voicecom_dev;

    // ثبت دیوایس 1380
    DeviceInfo generic_dev = dev;
    generic_dev.id = GENERIC_PROCESSED_DEVICE_ID;
    generic_dev.devicename = ACE_TEXT("Processed Voice (No Delay)");
    generic_dev.features |= SOUNDDEVICEFEATURE_DENOISE;
    sounddevs[generic_dev.id] = generic_dev;
}

// ---------------------------------------------------------
// INPUT STREAMER (Microphone)
// ---------------------------------------------------------
oboe::DataCallbackResult OboeInputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    short* pcmData = static_cast<short*>(audioData);
    int totalIncomingSamples = numFrames * channels;
    int requiredSamples = framesize * channels;

    if (fifo_size + totalIncomingSamples > fifo_buffer.size()) {
        fifo_buffer.resize(fifo_size + totalIncomingSamples + (requiredSamples * 2));
    }
    std::memcpy(&fifo_buffer[fifo_size], pcmData, totalIncomingSamples * sizeof(short));
    fifo_size += totalIncomingSamples;

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
    MYTRACE(ACE_TEXT("Oboe Input Stream closed. Error/Reason: %s\n"), oboe::convertToText(error));
    if (error == oboe::Result::ErrorDisconnected) {
        MYTRACE(ACE_TEXT("Oboe Input: Device disconnected. Falling back to default (0).\n"));
        StreamCapture* cachedRecorder = this->recorder;
        int cachedSndGrpId = this->sndgrpid;
        int cachedSampleRate = this->samplerate;
        int cachedChannels = this->channels;
        int cachedFrameSize = this->framesize;

        std::thread([cachedRecorder, cachedSndGrpId, cachedSampleRate, cachedChannels, cachedFrameSize]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            // استفاده از اشاره‌گر والد جهت جلوگیری از مخفی‌سازی نام (Name Hiding) متدها در C++
            std::shared_ptr<SoundSystem> sndSys = OboeWrapper::getInstance();
            if (sndSys) {
                sndSys->CloseInputStream(cachedRecorder); // پاکسازی استریم قدیمی و قطع‌شده
                sndSys->OpenInputStream(cachedRecorder, 0, cachedSndGrpId, cachedSampleRate, cachedChannels, cachedFrameSize);
            }
        }).detach();
    }
}

inputstreamer_t OboeWrapper::NewStream(StreamCapture* capture, int inputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeInputStreamer>(capture, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, inputdeviceid);

    streamer->fifo_buffer.resize(framesize * channels * 4);
    streamer->fifo_size = 0;

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setDataCallback(streamer.get());
    builder.setErrorCallback(streamer.get());

    // پشتیبانی اوبو برای تبدیل خودکار سمپل‌ریت در صورت نیاز
    builder.setChannelConversionAllowed(true);
    builder.setFormatConversionAllowed(true);
    builder.setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);

    if (inputdeviceid == DEFAULT_DEVICE_ID) {
        // تغییر به OpenSL ES برای حالت دیفالت
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        builder.setInputPreset(oboe::InputPreset::Generic);
        MYTRACE(ACE_TEXT("Oboe Input (Default 0): Forcing OpenSL ES backend with Generic mic preset\n"));
    } else if (inputdeviceid == VOICECOM_DEVICE_ID) {
        builder.setInputPreset(oboe::InputPreset::VoiceCommunication);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        MYTRACE(ACE_TEXT("Oboe Input (VoiceCom 1): Mic preset = VoiceCommunication (AEC/NS/AGC enabled)\n"));
    } else if (inputdeviceid == GENERIC_PROCESSED_DEVICE_ID) {
        builder.setInputPreset(oboe::InputPreset::Unprocessed);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive);
        MYTRACE(ACE_TEXT("Oboe Input (NoDelay 1380): Mic preset = Unprocessed, LowLatency, Exclusive\n"));
    } else {
        builder.setInputPreset(oboe::InputPreset::Unprocessed);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setDeviceId(inputdeviceid);
        MYTRACE(ACE_TEXT("Oboe Input (Other ID %d): Unprocessed, LowLatency\n"), inputdeviceid);
    }

    oboe::Result result = builder.openStream(streamer->stream);
    
    // فال‌بک اتوماتیک اگر باز کردن حالت سریع/اکسکلوسیو شکست خورد
    if (result != oboe::Result::OK && inputdeviceid != DEFAULT_DEVICE_ID) {
        MYTRACE(ACE_TEXT("Oboe: Selected input device %d failed to open (%s). Falling back to default (0).\n"), 
                inputdeviceid, oboe::convertToText(result));
        builder.setDeviceId(oboe::kUnspecified);
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        builder.setInputPreset(oboe::InputPreset::Generic);
        result = builder.openStream(streamer->stream);
    }

    if (result != oboe::Result::OK) {
        MYTRACE(ACE_TEXT("Failed to open Oboe input stream: %s\n"), oboe::convertToText(result));
        return nullptr;
    }

    MYTRACE(ACE_TEXT("Opened Oboe Mic\n"));
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

bool OboeWrapper::UpdateStreamCaptureFeatures(inputstreamer_t) {
    return true; 
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
        
        std::shared_ptr<SoundSystem> sndSys = OboeWrapper::getInstance();
        int mastervol = sndSys->GetMasterVolume(sndgrpid);
        bool mastermute = sndSys->IsAllMute(sndgrpid);
        SoftVolume(*this, cb_buffer.data(), framesize, mastervol, mastermute);

        if (fifo_buffer.size() < (size_t)(fifo_size + requiredSamples)) {
            fifo_buffer.resize(fifo_size + requiredSamples + (requiredSamples * 2));
        }
        std::memcpy(&fifo_buffer[fifo_size], cb_buffer.data(), requiredSamples * sizeof(short));
        fifo_size += requiredSamples;
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
    MYTRACE(ACE_TEXT("Oboe Output Stream closed. Error/Reason: %s\n"), oboe::convertToText(error));
    if (error == oboe::Result::ErrorDisconnected) {
        MYTRACE(ACE_TEXT("Oboe Output: Device disconnected. Falling back to default (0).\n"));
        StreamPlayer* cachedPlayer = this->player;
        int cachedSndGrpId = this->sndgrpid;
        int cachedSampleRate = this->samplerate;
        int cachedChannels = this->channels;
        int cachedFrameSize = this->framesize;

        std::thread([cachedPlayer, cachedSndGrpId, cachedSampleRate, cachedChannels, cachedFrameSize]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::shared_ptr<SoundSystem> sndSys = OboeWrapper::getInstance();
            if (sndSys) {
                sndSys->CloseOutputStream(cachedPlayer); // پاکسازی استریم قدیمی
                sndSys->OpenOutputStream(cachedPlayer, 0, cachedSndGrpId, cachedSampleRate, cachedChannels, cachedFrameSize);
                sndSys->StartStream(cachedPlayer);       // استفاده از متد کلاس پایه برای عدم تداخل کامپایل
            }
        }).detach();
    }
}

outputstreamer_t OboeWrapper::NewStream(soundsystem::StreamPlayer* player, int outputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeOutputStreamer>(player, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, outputdeviceid);

    streamer->cb_buffer.resize(framesize * channels);
    streamer->fifo_buffer.resize(framesize * channels * 4);
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
        // تغییر به OpenSL ES برای حالت دیفالت خروجی به دلیل باگ اسپیکر 
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        builder.setUsage(oboe::Usage::Media);
        builder.setContentType(oboe::ContentType::Music);
        MYTRACE(ACE_TEXT("Oboe Playback (Default 0): Forcing OpenSL ES backend with Media/Music usage\n"));
    } else if (outputdeviceid == VOICECOM_DEVICE_ID) {
        builder.setUsage(oboe::Usage::VoiceCommunication);
        builder.setContentType(oboe::ContentType::Speech);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        MYTRACE(ACE_TEXT("Oboe Playback (VoiceCom 1): VoiceCommunication, Speech\n"));
    } else if (outputdeviceid == GENERIC_PROCESSED_DEVICE_ID) {
        builder.setUsage(oboe::Usage::Media);
        builder.setContentType(oboe::ContentType::Music);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive);
        MYTRACE(ACE_TEXT("Oboe Playback (NoDelay 1380): Media, Music, LowLatency, Exclusive\n"));
    } else {
        builder.setUsage(oboe::Usage::Media);
        builder.setContentType(oboe::ContentType::Music);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setDeviceId(outputdeviceid);
        MYTRACE(ACE_TEXT("Oboe Playback (Other ID %d): Media, Music, LowLatency\n"), outputdeviceid);
    }

    oboe::Result result = builder.openStream(streamer->stream);

    if (result != oboe::Result::OK && outputdeviceid != DEFAULT_DEVICE_ID) {
        MYTRACE(ACE_TEXT("Oboe: Selected output device %d failed to open (%s). Falling back to default (0).\n"), 
                outputdeviceid, oboe::convertToText(result));
        builder.setDeviceId(oboe::kUnspecified);
        builder.setAudioApi(oboe::AudioApi::OpenSLES);
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        result = builder.openStream(streamer->stream);
    }

    if (result != oboe::Result::OK) {
        MYTRACE(ACE_TEXT("Failed to create Oboe audio player: %s\n"), oboe::convertToText(result));
        return nullptr;
    }

    MYTRACE(ACE_TEXT("Opened Oboe playback, samplerate %d, channels %d\n"), streamer->stream->getSampleRate(), streamer->stream->getChannelCount());
    return streamer;
}

void OboeWrapper::CloseStream(outputstreamer_t streamer) {
    if (streamer && streamer->stream) {
        streamer->stream->requestStop();
        streamer->stream->close();
        streamer->stream.reset();
        MYTRACE(ACE_TEXT("Closed Oboe playback stream\n"));
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