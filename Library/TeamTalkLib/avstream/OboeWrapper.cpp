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

namespace soundsystem {

enum AndroidSoundDevice {
    DEFAULT_DEVICE_ID           = (0 & SOUND_DEVICEID_MASK),
    VOICECOM_DEVICE_ID          = (1 & SOUND_DEVICEID_MASK),
    GENERIC_PROCESSED_DEVICE_ID = (1380 & SOUND_DEVICEID_MASK), // شناسه انتخابی جدید
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
    dev.devicename = ACE_TEXT("Default (Raw/High-Sensitivity)");
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

    // Device 0: Raw Unprocessed
    sounddevs[dev.id] = dev;

    // Device 1: Voice Communication (Hardware AEC/NS/AGC)
    DeviceInfo voicecom_dev = dev;
    voicecom_dev.id = VOICECOM_DEVICE_ID;
    voicecom_dev.devicename = ACE_TEXT("Voice Communication (Hardware AEC/NS)");
    voicecom_dev.features |= SOUNDDEVICEFEATURE_AEC;
    voicecom_dev.features |= SOUNDDEVICEFEATURE_AGC;
    voicecom_dev.features |= SOUNDDEVICEFEATURE_DENOISE;
    sounddevs[voicecom_dev.id] = voicecom_dev;

    // Device 1380: Generic Processed (Noise Gate)
    DeviceInfo generic_dev = dev;
    generic_dev.id = GENERIC_PROCESSED_DEVICE_ID;
    generic_dev.devicename = ACE_TEXT("Processed Voice (System Noise Gate)");
    generic_dev.features |= SOUNDDEVICEFEATURE_DENOISE;
    sounddevs[generic_dev.id] = generic_dev;
}

// ---------------------------------------------------------
// INPUT STREAMER (Microphone)
// ---------------------------------------------------------
oboe::DataCallbackResult OboeInputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    // حذف قفل صوتی جهت تضمین عملکرد Real-time و جلوگیری از بن‌بست
    short* pcmData = static_cast<short*>(audioData);
    int totalIncomingSamples = numFrames * channels;
    int requiredSamples = framesize * channels;

    // Fast append to static FIFO buffer
    if (fifo_size + totalIncomingSamples > fifo_buffer.size()) {
        fifo_buffer.resize(fifo_size + totalIncomingSamples + (requiredSamples * 2));
    }
    std::memcpy(&fifo_buffer[fifo_size], pcmData, totalIncomingSamples * sizeof(short));
    fifo_size += totalIncomingSamples;

    // Flush standard chunk sizes to TeamTalk
    while (fifo_size >= requiredSamples) {
        recorder->StreamCaptureCb(*this, fifo_buffer.data(), framesize);
        
        // Shift buffer (RingBuffer behavior)
        fifo_size -= requiredSamples;
        if (fifo_size > 0) {
            std::memmove(&fifo_buffer[0], &fifo_buffer[requiredSamples], fifo_size * sizeof(short));
        }
    }
    
    return oboe::DataCallbackResult::Continue;
}

void OboeInputStreamer::onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) {
    MYTRACE(ACE_TEXT("Oboe Input Stream closed. Error/Reason: %s\n"), oboe::convertToText(error));
}

inputstreamer_t OboeWrapper::NewStream(StreamCapture* capture, int inputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeInputStreamer>(capture, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, inputdeviceid);

    streamer->fifo_buffer.resize(framesize * channels * 4);
    streamer->fifo_size = 0;

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setDataCallback(streamer.get());
    builder.setErrorCallback(streamer.get());

    builder.setChannelConversionAllowed(true);
    builder.setFormatConversionAllowed(true);
    builder.setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium); // اصلاح نحوه فعال‌سازی مبدل سمپل‌ریت

    if (inputdeviceid == VOICECOM_DEVICE_ID) {
        builder.setInputPreset(oboe::InputPreset::VoiceCommunication);
        MYTRACE(ACE_TEXT("Oboe: Mic preset = VoiceCommunication (AEC/NS/AGC enabled)\n"));
    } else if (inputdeviceid == GENERIC_PROCESSED_DEVICE_ID) {
        builder.setInputPreset(oboe::InputPreset::Generic);
        MYTRACE(ACE_TEXT("Oboe: Mic preset = Generic (System Noise Gate)\n"));
    } else {
        builder.setInputPreset(oboe::InputPreset::Unprocessed);
        MYTRACE(ACE_TEXT("Oboe: Mic preset = Unprocessed (Raw High-Sensitivity)\n"));
        if (inputdeviceid != DEFAULT_DEVICE_ID) {
            builder.setDeviceId(inputdeviceid);
            MYTRACE(ACE_TEXT("Oboe: Mic Device ID = %d\n"), inputdeviceid);
        }
    }

    oboe::Result result = builder.openStream(streamer->stream);
    
    if (result != oboe::Result::OK && inputdeviceid != DEFAULT_DEVICE_ID) {
        MYTRACE(ACE_TEXT("Oboe: Selected input device %d failed to open (%s). Falling back to default (0).\n"), 
                inputdeviceid, oboe::convertToText(result));
        builder.setDeviceId(oboe::kUnspecified);
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
    // حذف قفل صوتی برای جلوگیری از بن‌بست صوتی
    short* outData = static_cast<short*>(audioData);
    int totalOutgoingSamples = numFrames * channels;
    int requiredSamples = framesize * channels;
    
    bool more = true;

    while (fifo_size < totalOutgoingSamples && more) {
        more = player->StreamPlayerCb(*this, cb_buffer.data(), framesize);
        
        int mastervol = OboeWrapper::getInstance()->GetMasterVolume(sndgrpid);
        bool mastermute = OboeWrapper::getInstance()->IsAllMute(sndgrpid);
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
            std::memmove(&fifo_buffer[0], &fifo_buffer[samplesToCopy], fifo_size * sizeof(short)); // تصحیح نام متغیر از buffer به fifo_buffer
        }
    }

    if (samplesToCopy < totalOutgoingSamples) {
        std::memset(outData + samplesToCopy, 0, (totalOutgoingSamples - samplesToCopy) * sizeof(short));
    }

    return more ? oboe::DataCallbackResult::Continue : oboe::DataCallbackResult::Stop;
}

void OboeOutputStreamer::onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) {
    MYTRACE(ACE_TEXT("Oboe Output Stream closed. Error/Reason: %s\n"), oboe::convertToText(error));
}

outputstreamer_t OboeWrapper::NewStream(soundsystem::StreamPlayer* player, int outputdeviceid, int sndgrpid, int samplerate, int channels, int framesize) {
    auto streamer = std::make_shared<OboeOutputStreamer>(player, sndgrpid, framesize, samplerate, channels, SOUND_API_OBOE_ANDROID, outputdeviceid);

    streamer->cb_buffer.resize(framesize * channels);
    streamer->fifo_buffer.resize(framesize * channels * 4);
    streamer->fifo_size = 0;

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setDataCallback(streamer.get());
    builder.setErrorCallback(streamer.get());

    builder.setChannelConversionAllowed(true);
    builder.setFormatConversionAllowed(true);
    builder.setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium); // اصلاح نحوه فعال‌سازی مبدل سمپل‌ریت

    if (outputdeviceid == VOICECOM_DEVICE_ID) {
        builder.setUsage(oboe::Usage::VoiceCommunication);
        builder.setContentType(oboe::ContentType::Speech);
        MYTRACE(ACE_TEXT("Oboe: Playback usage = VoiceCommunication, Speech\n"));
    } else {
        builder.setUsage(oboe::Usage::Media);
        builder.setContentType(oboe::ContentType::Music);
        MYTRACE(ACE_TEXT("Oboe: Playback usage = Media, Music\n"));
        if (outputdeviceid != DEFAULT_DEVICE_ID) {
            builder.setDeviceId(outputdeviceid);
            MYTRACE(ACE_TEXT("Oboe: Playback Device ID = %d\n"), outputdeviceid);
        }
    }

    oboe::Result result = builder.openStream(streamer->stream);

    if (result != oboe::Result::OK && outputdeviceid != DEFAULT_DEVICE_ID) {
        MYTRACE(ACE_TEXT("Oboe: Selected output device %d failed to open (%s). Falling back to default (0).\n"), 
                outputdeviceid, oboe::convertToText(result));
        builder.setDeviceId(oboe::kUnspecified);
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