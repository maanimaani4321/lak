#include <oboe/Oboe.h>
#include <vector>
#include <mutex>
#include <cstring>
#include <algorithm>
#include <memory>
#include <android/log.h>

// فرض می‌کنیم هدر اصلی کلاس در این بخش اینکلود می‌شود
#include "OboeWrapper.h" 

#define LOG_TAG "OboeWrapper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// تعریف توابع کمکی خارجی در صورت لزوم (مانند کنترل صدا)
extern void SoftVolume(OboeOutputStreamer& streamer, short* buffer, int framesize, int volume, bool mute);

// برای جلوگیری از خطای کامپایل ناشی از تغییر هدر .h، نمونه‌های فعال استریم را به صورت استاتیک در همین فایل مدیریت می‌کنیم
static std::shared_ptr<oboe::AudioStream> gPlaybackStream = nullptr;
static std::shared_ptr<oboe::AudioStream> gRecordingStream = nullptr;
static std::unique_ptr<OboeOutputStreamer> gOutputStreamer = nullptr;
static std::unique_ptr<OboeInputStreamer> gInputStreamer = nullptr;

// ============================================================================
// پیاده‌سازی OboeOutputStreamer (بافر خروجی اسپیکر)
// ============================================================================

OboeOutputStreamer::OboeOutputStreamer(SoundPlayer* player, int framesize, int channels, int sndgrpid)
    : player(player), framesize(framesize), channels(channels), sndgrpid(sndgrpid), fifo_size(0) {
    // رزرو اولیه حافظه برای جلوگیری از بازتخصیص حافظه (Allocation) درون ترد صوتی
    fifo_buffer.reserve(48000 * channels); 
    cb_buffer.resize(framesize * channels);
}

oboe::DataCallbackResult OboeOutputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    short* outData = static_cast<short*>(audioData);
    int32_t currentChannels = oboeStream->getChannelCount();
    int32_t totalOutgoingSamples = numFrames * currentChannels;
    int32_t requiredSamples = framesize * currentChannels;

    std::lock_guard<std::mutex> lock(fifo_mutex);

    bool more = true;
    // پر کردن بافر محلی تا زمانی که داده کافی برای پاسخگویی به درخواست Oboe فراهم شود
    while (fifo_buffer.size() < static_cast<size_t>(totalOutgoingSamples) && more) {
        if (cb_buffer.size() < static_cast<size_t>(requiredSamples)) {
            cb_buffer.resize(requiredSamples);
        }

        // دریافت فریم صوتی از تیم‌تاک
        more = player->StreamPlayerCb(*this, cb_buffer.data(), framesize);

        // اعمال مدیریت سطح صدا و حالت قطع صدا (Mute)
        int mastervol = OboeWrapper::getInstance()->GetMasterVolume(sndgrpid);
        bool mastermute = OboeWrapper::getInstance()->IsAllMute(sndgrpid);
        SoftVolume(*this, cb_buffer.data(), framesize, mastervol, mastermute);

        // اضافه کردن داده‌های دریافتی جدید به انتهای بافر FIFO
        fifo_buffer.insert(fifo_buffer.end(), cb_buffer.begin(), cb_buffer.begin() + requiredSamples);
    }

    // انتقال داده‌ها از FIFO به بافر سخت‌افزار خروجی Oboe
    if (fifo_buffer.size() >= static_cast<size_t>(totalOutgoingSamples)) {
        std::copy(fifo_buffer.begin(), fifo_buffer.begin() + totalOutgoingSamples, outData);
        fifo_buffer.erase(fifo_buffer.begin(), fifo_buffer.begin() + totalOutgoingSamples);
    } else {
        // جلوگیری از ایجاد نویز استاتیک شدید در زمان کمبود داده صوتی (Underflow)
        size_t availableSamples = fifo_buffer.size();
        if (availableSamples > 0) {
            std::copy(fifo_buffer.begin(), fifo_buffer.end(), outData);
            fifo_buffer.clear();
        }
        std::fill(outData + availableSamples, outData + totalOutgoingSamples, 0);
    }

    // به روز رسانی متغیر طول بافر جهت حفظ هماهنگی با کدهای بیرونی احتمالی
    fifo_size = static_cast<int>(fifo_buffer.size());

    return oboe::DataCallbackResult::Continue;
}

void OboeOutputStreamer::onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) {
    LOGE("خروجی اسپیکر با خطا مواجه شد: %s", oboe::convertToText(error));
}

// ============================================================================
// پیاده‌سازی OboeInputStreamer (بافر ورودی میکروفون)
// ============================================================================

OboeInputStreamer::OboeInputStreamer(SoundRecorder* recorder, int framesize, int channels)
    : recorder(recorder), framesize(framesize), channels(channels), fifo_size(0) {
    fifo_buffer.reserve(48000 * channels);
    cb_buffer.resize(framesize * channels);
}

oboe::DataCallbackResult OboeInputStreamer::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    const short* inData = static_cast<const short*>(audioData);
    int32_t currentChannels = oboeStream->getChannelCount();
    int32_t totalIncomingSamples = numFrames * currentChannels;
    int32_t requiredSamples = framesize * currentChannels;

    std::lock_guard<std::mutex> lock(fifo_mutex);

    // دریافت سمپل‌های ضبط‌شده جدید و افزودن به FIFO ورودی
    fifo_buffer.insert(fifo_buffer.end(), inData, inData + totalIncomingSamples);

    // ارسال داده‌ها به صورت فریم‌های منظم به تیم‌تاک
    while (fifo_buffer.size() >= static_cast<size_t>(requiredSamples)) {
        if (cb_buffer.size() < static_cast<size_t>(requiredSamples)) {
            cb_buffer.resize(requiredSamples);
        }

        std::copy(fifo_buffer.begin(), fifo_buffer.begin() + requiredSamples, cb_buffer.begin());
        fifo_buffer.erase(fifo_buffer.begin(), fifo_buffer.begin() + requiredSamples);

        // ارسال فریم به سیستم ضبط صدا در تیم‌تاک
        recorder->StreamRecorderCb(*this, cb_buffer.data(), framesize);
    }

    fifo_size = static_cast<int>(fifo_buffer.size());

    return oboe::DataCallbackResult::Continue;
}

void OboeInputStreamer::onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) {
    LOGE("ورودی میکروفون با خطا مواجه شد: %s", oboe::convertToText(error));
}

// ============================================================================
// متدهای مدیریت استریم در OboeWrapper
// ============================================================================

bool OboeWrapper::StartPlayback(SoundPlayer* player, int samplerate, int channels, int framesize, int outputdeviceid, int inputdeviceid, int sndgrpid) {
    StopPlayback();

    gOutputStreamer = std::make_unique<OboeOutputStreamer>(player, framesize, channels, sndgrpid);

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setCallback(gOutputStreamer.get());

    // مدیریت مد‌های ارسالی دقیقاً مطابق درخواست شما:
    if (inputdeviceid == 1) {
        // ۱. حالت مکالمه صوتی دوطرفه (مخصوص ویس چت)
        builder.setUsage(oboe::Usage::VoiceCommunication);
        builder.setContentType(oboe::ContentType::Speech);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Shared);
    } else if (inputdeviceid == 1380) {
        // ۲. حالت بدون تاخیر (No Delay)
        builder.setUsage(oboe::Usage::Media);
        builder.setContentType(oboe::ContentType::Music);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive); // اختصاص کل پهنای باند صوتی سخت‌افزار برای سرعت حداکثر
    } else {
        // ۳. حالت کاملاً عادی (مقدار 0 یا غیره - دقیقاً مشابه پایداری صوتی OpenSL ES)
        builder.setUsage(oboe::Usage::Media);
        builder.setContentType(oboe::ContentType::Music);
        builder.setPerformanceMode(oboe::PerformanceMode::None); // حذف هرگونه فیلتر سخت‌افزاری پردازش صوتی سریع
        builder.setSharingMode(oboe::SharingMode::Shared);
    }

    if (outputdeviceid != 0 && outputdeviceid != -1) {
        builder.setDeviceId(outputdeviceid);
    }

    oboe::Result result = builder.openStream(gPlaybackStream);
    if (result != oboe::Result::OK) {
        LOGE("خطا در باز کردن استریم پخش: %s", oboe::convertToText(result));
        return false;
    }

    result = gPlaybackStream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("خطا در شروع استریم پخش: %s", oboe::convertToText(result));
        gPlaybackStream->close();
        gPlaybackStream.reset();
        return false;
    }

    LOGI("پخش صدا با موفقیت و بدون خطای سخت‌افزاری آغاز شد.");
    return true;
}

void OboeWrapper::StopPlayback() {
    if (gPlaybackStream) {
        gPlaybackStream->stop();
        gPlaybackStream->close();
        gPlaybackStream.reset();
    }
    gOutputStreamer.reset();
    LOGI("پخش صدا متوقف شد.");
}

bool OboeWrapper::StartRecording(SoundRecorder* recorder, int samplerate, int channels, int framesize, int inputdeviceid) {
    StopRecording();

    gInputStreamer = std::make_unique<OboeInputStreamer>(recorder, framesize, channels);

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(channels);
    builder.setSampleRate(samplerate);
    builder.setCallback(gInputStreamer.get());

    // مدیریت ورودی میکروفون بر اساس مد انتخابی:
    if (inputdeviceid == 1) {
        // حالت مکالمه (دارای ویژگی‌های نویزگیر و لغو اکو بومی گوشی)
        builder.setInputPreset(oboe::InputPreset::VoiceCommunication);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Shared);
    } else if (inputdeviceid == 1380) {
        // حالت خام و بدون تاخیر (بدون نویزگیر دیجیتالی سنگین سخت‌افزاری)
        builder.setInputPreset(oboe::InputPreset::Unprocessed);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Exclusive);
    } else {
        // حالت کاملاً عادی (همانند پایداری صدای خام ورودی در OpenSL ES)
        builder.setInputPreset(oboe::InputPreset::Generic); // پرهیز از رفتارهای متغیر تراشه‌ها روی حالت Unprocessed
        builder.setPerformanceMode(oboe::PerformanceMode::None); // پایداری کامل
        builder.setSharingMode(oboe::SharingMode::Shared);
    }

    if (inputdeviceid != 0 && inputdeviceid != 1 && inputdeviceid != 1380) {
        builder.setDeviceId(inputdeviceid);
    }

    oboe::Result result = builder.openStream(gRecordingStream);
    if (result != oboe::Result::OK) {
        LOGE("خطا در باز کردن استریم ورودی ضبط: %s", oboe::convertToText(result));
        return false;
    }

    result = gRecordingStream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("خطا در شروع استریم ورودی ضبط: %s", oboe::convertToText(result));
        gRecordingStream->close();
        gRecordingStream.reset();
        return false;
    }

    LOGI("ضبط صوتی بدون بروز تداخل فرکانسی آغاز شد.");
    return true;
}

void OboeWrapper::StopRecording() {
    if (gRecordingStream) {
        gRecordingStream->stop();
        gRecordingStream->close();
        gRecordingStream.reset();
    }
    gInputStreamer.reset();
    LOGI("ضبط صوتی متوقف شد.");
}