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
#include "myace/MyACE.h"
#include <cassert>
#include <cstring>
#include <algorithm>

namespace soundsystem {

enum AndroidSoundDevice
{
    DEFAULT_DEVICE_ID           = (0 & SOUND_DEVICEID_MASK),
};

constexpr auto DEFAULT_SAMPLERATE = 48000;

OboeWrapper::OboeWrapper()
{
    Init();
}

OboeWrapper::~OboeWrapper()
{
    Close();
    MYTRACE(ACE_TEXT("~OboeWrapper()\n"));
}

bool OboeWrapper::Init()
{
    RefreshDevices();
    MYTRACE(ACE_TEXT("Initializing Oboe Audio Wrapper\n"));
    return true;
}

void OboeWrapper::Close()
{
    MYTRACE(ACE_TEXT("Closed Oboe Audio Wrapper\n"));
}

std::shared_ptr<OboeWrapper> OboeWrapper::getInstance()
{
    static std::shared_ptr<OboeWrapper> p(new OboeWrapper());
    return p;
}

soundgroup_t OboeWrapper::NewSoundGroup()
{
    return std::make_shared<OboeSoundGroup>();
}

void OboeWrapper::RemoveSoundGroup(soundgroup_t grp)
{
    assert(grp);
    MYTRACE(ACE_TEXT("Removing Oboe Sound Group %p\n"), grp.get());
}

bool OboeWrapper::GetDefaultDevices(int& inputdeviceid,
                                        int& outputdeviceid)
{
    return GetDefaultDevices(SOUND_API_OBOE_ANDROID,
                             inputdeviceid,
                             outputdeviceid);
}

bool OboeWrapper::GetDefaultDevices(SoundAPI sndsys,
                                        int& inputdeviceid,
                                        int& outputdeviceid)
{
    if(sndsys == SOUND_API_OBOE_ANDROID)
    {
        inputdeviceid = outputdeviceid = DEFAULT_DEVICE_ID;
        return true;
    }
    return false;
}

bool OboeWrapper::UpdateStreamCaptureFeatures(inputstreamer_t streamer)
{
    // از آنجا که پردازش صدا را غیرفعال (Unprocessed) کرده‌ایم، این بخش صرفاً تاییدیه صادر می‌کند
    return true;
}

// ---------------------------------------------------------
// INPUT STREAMER (Microphone) - Data & Callback
// ---------------------------------------------------------
oboe::DataCallbackResult OboeInputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames)
{
    std::lock_guard<std::recursive_mutex> g(mutex);

    short* pcmData = static_cast<short*>(audioData);
    int totalIncomingSamples = numFrames * channels;
    int requiredSamples = framesize * channels;

    // ۱. افزایش فضای بافر موقت در صورت کمبود ظرفیت جهت جلوگیری از سرریز
    if (fifo_size + totalIncomingSamples > buffer.size())
    {
        buffer.resize(fifo_size + totalIncomingSamples + (requiredSamples * 2));
    }

    // ۲. الحاق فریم‌های ورودی جدید سخت‌افزار به انتهای بافر
    std::memcpy(&buffer[fifo_size], pcmData, totalIncomingSamples * sizeof(short));
    fifo_size += totalIncomingSamples;

    // ۳. استخراج و انتقال بسته‌ها در قالب فریم‌های ثابت مورد انتظار TeamTalk
    while (fifo_size >= requiredSamples)
    {
        recorder->StreamCaptureCb(*this, buffer.data(), framesize);
        
        fifo_size -= requiredSamples;
        if (fifo_size > 0)
        {
            std::memmove(&buffer[0], &buffer[requiredSamples], fifo_size * sizeof(short));
        }
    }

    return oboe::DataCallbackResult::Continue;
}

void OboeInputStreamer::onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error)
{
    MYTRACE(ACE_TEXT("Oboe capture stream closed unexpectedly due to error: %s\n"), oboe::convertToText(error));
}

inputstreamer_t OboeWrapper::NewStream(StreamCapture* capture,
                                           int inputdeviceid, int sndgrpid,
                                           int samplerate, int channels,
                                           int framesize)
{
    soundgroup_t sg = GetSoundGroup(sndgrpid);
    if (!sg)
        return inputstreamer_t();

    auto streamer = std::make_shared<OboeInputStreamer>(capture,
                                                        sndgrpid,
                                                        framesize,
                                                        samplerate,
                                                        channels,
                                                        SOUND_API_OBOE_ANDROID,
                                                        inputdeviceid);

    // آماده‌سازی اولیه بافر با ضریب ظرفیت بالا
    streamer->buffer.resize(framesize * channels * 4);
    streamer->fifo_size = 0;

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setCallback(streamer.get());

    // استفاده از حالت کاملاً خام و بدون دستکاری الگوریتمی
    builder.setInputPreset(oboe::InputPreset::Unprocessed);

    oboe::Result result = builder.openStream(streamer->stream);
    if (result != oboe::Result::OK)
    {
        MYTRACE(ACE_TEXT("Failed to create Oboe input stream. Error: %s\n"), oboe::convertToText(result));
        return inputstreamer_t();
    }

    MYTRACE(ACE_TEXT("Opened Oboe capture stream %p, samplerate %d, channels %d, framesize %d\n"),
            capture, samplerate, channels, framesize);

    return streamer;
}

bool OboeWrapper::StartStream(inputstreamer_t streamer)
{
    assert(streamer->stream);
    oboe::Result result = streamer->stream->requestStart();
    assert(oboe::Result::OK == result);
    return oboe::Result::OK == result;
}

bool OboeWrapper::StopStream(inputstreamer_t streamer)
{
    assert(streamer->stream);
    oboe::Result result = streamer->stream->requestStop();
    assert(oboe::Result::OK == result);
    return oboe::Result::OK == result;
}

void OboeWrapper::CloseStream(inputstreamer_t streamer)
{
    if (streamer->stream)
    {
        streamer->stream->requestStop();
        
        {
            // همگام‌سازی با کالبک‌ها قبل از نابود کردن استریم سخت‌افزاری
            std::lock_guard<std::recursive_mutex> g(streamer->mutex);
        }

        streamer->stream->close();
        streamer->stream.reset();
    }
    MYTRACE(ACE_TEXT("Closed Oboe capture stream %p\n"), streamer->recorder);
}

bool OboeWrapper::IsStreamStopped(inputstreamer_t streamer)
{
    if (!streamer->stream)
        return true;

    oboe::StreamState state = streamer->stream->getState();
    return state == oboe::StreamState::Stopped ||
           state == oboe::StreamState::Closed ||
           state == oboe::StreamState::Stopping;
}

// ---------------------------------------------------------
// OUTPUT STREAMER (Speaker) - Data & Callback
// ---------------------------------------------------------
oboe::DataCallbackResult OboeOutputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames)
{
    std::lock_guard<std::recursive_mutex> g(mutex);

    short* outData = static_cast<short*>(audioData);
    int totalOutgoingSamples = numFrames * channels;
    int requiredSamples = framesize * channels;

    int mastervol = OboeWrapper::getInstance()->GetMasterVolume(sndgrpid);
    bool mastermute = OboeWrapper::getInstance()->IsAllMute(sndgrpid);

    bool more = true;

    // ۱. پمپ مداوم دیتا از TeamTalk تا انباشته شدن کافی نمونه‌ها درون بافر
    while (fifo_size < totalOutgoingSamples && more)
    {
        if (fifo_size + requiredSamples > buffer.size())
        {
            buffer.resize(fifo_size + requiredSamples + (requiredSamples * 2));
        }

        // فراخوانی مستقیم و اعمال کنترل صدا روی دیتای استخراجی جدید
        more = player->StreamPlayerCb(*this, &buffer[fifo_size], framesize);
        SoftVolume(*this, &buffer[fifo_size], framesize, mastervol, mastermute);

        fifo_size += requiredSamples;
    }

    // ۲. تحویل نمونه‌های صوتی لازم به بافر اوبو جهت خروجی سخت‌افزاری
    int samplesToCopy = std::min(fifo_size, totalOutgoingSamples);
    std::memcpy(outData, buffer.data(), samplesToCopy * sizeof(short));

    // ۳. جابجایی دیتای پخش‌نشده باقی‌مانده به نقطه شروع بافر جهت آمادگی کالبک بعدی
    if (samplesToCopy < fifo_size)
    {
        int remaining = fifo_size - samplesToCopy;
        std::memmove(&buffer[0], &buffer[samplesToCopy], remaining * sizeof(short));
        fifo_size = remaining;
    }
    else
    {
        fifo_size = 0;
    }

    // ۴. بیصدا کردن انتهای بافر در صورت اتمام ناگهانی منبع جهت ممانعت از ایجاد نویز استاتیک
    if (samplesToCopy < totalOutgoingSamples)
    {
        std::memset(outData + samplesToCopy, 0, (totalOutgoingSamples - samplesToCopy) * sizeof(short));
    }

    return more ? oboe::DataCallbackResult::Continue : oboe::DataCallbackResult::Stop;
}

void OboeOutputStreamer::onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error)
{
    MYTRACE(ACE_TEXT("Oboe playback stream closed unexpectedly due to error: %s\n"), oboe::convertToText(error));
}

outputstreamer_t OboeWrapper::NewStream(soundsystem::StreamPlayer* player,
                                            int outputdeviceid,
                                            int sndgrpid, int samplerate,
                                            int channels, int framesize)
{
    soundgroup_t sg = GetSoundGroup(sndgrpid);
    if (!sg)
        return outputstreamer_t();

    auto streamer = std::make_shared<OboeOutputStreamer>(player,
                                                         sndgrpid,
                                                         framesize,
                                                         samplerate,
                                                         channels,
                                                         SOUND_API_OBOE_ANDROID,
                                                         outputdeviceid);

    streamer->buffer.resize(framesize * channels * 4);
    streamer->fifo_size = 0;

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setCallback(streamer.get());

    // اولویت اجرای خروجی صوتی روی حالت پخش رسانه‌ای خام
    builder.setUsage(oboe::Usage::Media);
    builder.setContentType(oboe::ContentType::Music);

    oboe::Result result = builder.openStream(streamer->stream);
    if (result != oboe::Result::OK)
    {
        MYTRACE(ACE_TEXT("Failed to create Oboe output stream. Error: %s\n"), oboe::convertToText(result));
        return outputstreamer_t();
    }

    MYTRACE(ACE_TEXT("Opened playback stream, samplerate %d, channels %d, framesize %d\n"),
            samplerate, channels, framesize);

    return streamer;
}

void OboeWrapper::CloseStream(outputstreamer_t streamer)
{
    if (streamer->stream)
    {
        streamer->stream->requestStop();

        {
            std::lock_guard<std::recursive_mutex> g(streamer->mutex);
        }

        streamer->stream->close();
        streamer->stream.reset();
    }
    MYTRACE(ACE_TEXT("Closed Oboe playback stream\n"));
}

bool OboeWrapper::StartStream(outputstreamer_t streamer)
{
    assert(streamer->stream);
    oboe::Result result = streamer->stream->requestStart();
    assert(oboe::Result::OK == result);
    return oboe::Result::OK == result;
}

bool OboeWrapper::StopStream(outputstreamer_t streamer)
{
    assert(streamer->stream);
    oboe::Result result = streamer->stream->requestStop();
    assert(oboe::Result::OK == result);
    return oboe::Result::OK == result;
}

bool OboeWrapper::IsStreamStopped(outputstreamer_t streamer)
{
    if (!streamer->stream)
        return true;

    oboe::StreamState state = streamer->stream->getState();
    return state == oboe::StreamState::Stopped ||
           state == oboe::StreamState::Closed ||
           state == oboe::StreamState::Stopping;
}

// ---------------------------------------------------------
// DEVICE PROBING (Dynamic Hardware Capabilities)
// ---------------------------------------------------------
void OboeWrapper::FillDevices(sounddevices_t& sounddevs)
{
    DeviceInfo dev;
    dev.devicename = ACE_TEXT("Default Sound Device (Oboe - Raw)");
    dev.soundsystem = SOUND_API_OBOE_ANDROID;
    dev.id = DEFAULT_DEVICE_ID;

    // ۱. تست پویای ورودی صوتی سخت‌افزار (میکروفون) برای شناسایی فرکانس و کانال‌های قابل پشتیبانی
    for (int sr : standardSampleRates)
    {
        for (int c = 1; c <= 2; c++)
        {
            oboe::AudioStreamBuilder builder;
            builder.setDirection(oboe::Direction::Input);
            builder.setFormat(oboe::AudioFormat::I16);
            builder.setChannelCount(c);
            builder.setSampleRate(sr);
            builder.setInputPreset(oboe::InputPreset::Unprocessed);

            std::shared_ptr<oboe::AudioStream> testStream;
            oboe::Result result = builder.openStream(testStream);
            if (result == oboe::Result::OK)
            {
                dev.input_channels.insert(c);
                if (c > dev.max_input_channels)
                    dev.max_input_channels = c;
                
                dev.input_samplerates.insert(sr);
                testStream->close();
                MYTRACE(ACE_TEXT("Oboe input probe success: rate=%d, channels=%d\n"), sr, c);
            }
            else
            {
                MYTRACE(ACE_TEXT("Oboe input probe failed: rate=%d, channels=%d, error=%s\n"), sr, c, oboe::convertToText(result));
            }
        }
    }

    // ۲. تست پویای خروجی صوتی سخت‌افزار (بلندگو/هدفون)
    for (int sr : standardSampleRates)
    {
        for (int c = 1; c <= 2; c++)
        {
            oboe::AudioStreamBuilder builder;
            builder.setDirection(oboe::Direction::Output);
            builder.setFormat(oboe::AudioFormat::I16);
            builder.setChannelCount(c);
            builder.setSampleRate(sr);
            builder.setUsage(oboe::Usage::Media);
            builder.setContentType(oboe::ContentType::Music);

            std::shared_ptr<oboe::AudioStream> testStream;
            oboe::Result result = builder.openStream(testStream);
            if (result == oboe::Result::OK)
            {
                dev.output_channels.insert(c);
                if (c > dev.max_output_channels)
                    dev.max_output_channels = c;
                
                dev.output_samplerates.insert(sr);
                testStream->close();
                MYTRACE(ACE_TEXT("Oboe output probe success: rate=%d, channels=%d\n"), sr, c);
            }
            else
            {
                MYTRACE(ACE_TEXT("Oboe output probe failed: rate=%d, channels=%d, error=%s\n"), sr, c, oboe::convertToText(result));
            }
        }
    }

    // انتخاب بهینه‌ترین فرکانس کاری پیش‌فرض
    dev.default_samplerate = DEFAULT_SAMPLERATE;
    if (!dev.output_samplerates.empty())
    {
        dev.default_samplerate = *dev.output_samplerates.rbegin();
    }

    sounddevs[dev.id] = dev;
}

} // namespace soundsystem