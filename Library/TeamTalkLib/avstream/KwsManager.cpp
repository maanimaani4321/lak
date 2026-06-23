#include "KwsManager.h"
#include "sherpa-onnx/c-api/cxx-api.h"
#include <mutex>
#include <memory>
#include <vector>
#include <algorithm>

namespace teamtalk {

struct KwsInternalState {
    std::recursive_mutex mutex;
    std::unique_ptr<sherpa_onnx::cxx::VoiceActivityDetector> vad; // مدل سبک و بهینه VAD
    std::unique_ptr<sherpa_onnx::cxx::KeywordSpotter> spotter;
    std::unique_ptr<sherpa_onnx::cxx::OnlineStream> stream;
    std::unique_ptr<sherpa_onnx::cxx::LinearResampler> resampler;
    JavaVM* jvm = nullptr;
    jobject java_listener_ref = nullptr;
    jmethodID callback_method_id = nullptr;
    bool active = false;
    int current_samplerate = 0;
};

static KwsInternalState g_state;

static bool FileExists(const std::string& path) {
    if (path.empty()) return false;
    FILE* f = fopen(path.c_str(), "r");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

void KwsInit(JavaVM* vm) {
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    g_state.jvm = vm;
}

bool KwsStart(JNIEnv* env, jobject jlistener,
              const std::string& encoder_path,
              const std::string& decoder_path,
              const std::string& joiner_path,
              const std::string& tokens_path,
              const std::string& bpe_path,
              const std::string& keywords_path,
              const std::string& vad_path)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    if (g_state.active) return true;

    if (!FileExists(encoder_path) || !FileExists(decoder_path) || !FileExists(joiner_path) ||
        !FileExists(tokens_path) || !FileExists(bpe_path) || !FileExists(keywords_path) || !FileExists(vad_path)) {
        return false;
    }

    try {
        // ۱. تنظیم بهینه پارامترهای VAD برای مقابله با چیده شدن ابتدای گفتار و مصرف باتری ایده آل
        sherpa_onnx::cxx::VadModelConfig vad_config;
        vad_config.silero_vad.model = vad_path.c_str();
        
        // کاهش آستانه به 0.35 تا لبه‌های صدا و شروع‌های آرام هم شکار شده و کلمه از ابتدا کامل ذخیره شود
        vad_config.silero_vad.threshold = 0.35f; 
        
        vad_config.silero_vad.min_silence_duration = 0.5f;
        
        // کاهش مدت زمان تایید شروع کلام به ۱۰۰ میلی‌ثانیه تا تاخیر روشن شدن VAD کلمه را قیچی نکند
        vad_config.silero_vad.min_speech_duration = 0.10f; 
        
        vad_config.silero_vad.window_size = 512;
        vad_config.sample_rate = 16000;
        vad_config.num_threads = 1; // ۱ ترد برای پردازش پس‌زمینه VAD بسیار کم‌مصرف است
        vad_config.provider = "cpu";

        g_state.vad = std::make_unique<sherpa_onnx::cxx::VoiceActivityDetector>(
            sherpa_onnx::cxx::VoiceActivityDetector::Create(vad_config, 10.0f)
        );

        if (!g_state.vad || !g_state.vad->Get()) {
            g_state.vad.reset();
            return false;
        }

        // ۲. تنظیم پارامترهای حساسیت KWS برای گفتار طبیعی و محاوره‌ای
        sherpa_onnx::cxx::KeywordSpotterConfig config;
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;

        config.model_config.transducer.encoder = encoder_path;
        config.model_config.transducer.decoder = decoder_path;
        config.model_config.transducer.joiner = joiner_path;
        config.model_config.tokens = tokens_path;
        config.model_config.bpe_vocab = bpe_path;
        config.model_config.num_threads = 2; // تعادل ایده آل پردازش همزمان و باتری گوشی
        config.model_config.provider = "cpu";

        config.keywords_file = keywords_path;
        config.max_active_paths = 4;
        
        // بالا بردن ضریب تقویت کلمات هدف به منظور سهولت در فعالسازی بدون رباتیک صحبت کردن
        config.keywords_score = 2.0f;
        
        // کاهش آستانه تصمیم به 0.15 تا نوسانات و عدم انطباق‌های لحن صوتی مانع تشخیص طبیعی نشوند
        config.keywords_threshold = 0.15f;

        g_state.spotter = std::make_unique<sherpa_onnx::cxx::KeywordSpotter>(
            sherpa_onnx::cxx::KeywordSpotter::Create(config)
        );

        if (!g_state.spotter || !g_state.spotter->Get()) {
            g_state.vad.reset();
            g_state.spotter.reset();
            return false;
        }

        g_state.stream = std::make_unique<sherpa_onnx::cxx::OnlineStream>(
            g_state.spotter->CreateStream()
        );

        jclass clazz = env->GetObjectClass(jlistener);
        g_state.callback_method_id = env->GetMethodID(clazz, "onKeywordDetected", "(Ljava/lang/String;)V");
        if (!g_state.callback_method_id) {
            g_state.vad.reset();
            g_state.spotter.reset();
            g_state.stream.reset();
            return false;
        }

        g_state.java_listener_ref = env->NewGlobalRef(jlistener);
        g_state.active = true;
        g_state.current_samplerate = 0;

        return true;
    } catch (...) {
        g_state.vad.reset();
        g_state.spotter.reset();
        g_state.stream.reset();
        return false;
    }
}

void KwsStop(JNIEnv* env) {
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    if (!g_state.active) return;

    g_state.active = false;
    
    // رعایت زنجیره آزادسازی ایده آل منابع (مخرب ها به ترتیب وابستگی) برای پایداری در سطح سیستم عامل اندروید
    g_state.stream.reset();   
    g_state.spotter.reset();  
    g_state.vad.reset();      
    g_state.resampler.reset();

    if (g_state.java_listener_ref && env) {
        env->DeleteGlobalRef(g_state.java_listener_ref);
        g_state.java_listener_ref = nullptr;
    }
    g_state.callback_method_id = nullptr;
}

static void NotifyJavaListener(const std::string& keyword) {
    if (!g_state.jvm || !g_state.java_listener_ref || !g_state.callback_method_id) return;

    JNIEnv* env = nullptr;
    bool is_attached = false;
    jint res = g_state.jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        res = g_state.jvm->AttachCurrentThread(&env, nullptr);
        if (res == JNI_OK) {
            is_attached = true;
        }
    }

    if (env) {
        jstring jkeyword = env->NewStringUTF(keyword.c_str());
        env->CallVoidMethod(g_state.java_listener_ref, g_state.callback_method_id, jkeyword);
        env->DeleteLocalRef(jkeyword);
    }

    if (is_attached) {
        g_state.jvm->DetachCurrentThread();
    }
}

void KwsProcessAudio(const short* buffer, int samples, int channels, int samplerate) {
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    if (!g_state.active || !g_state.stream || !g_state.spotter || !g_state.vad) return;

    // ۱. تبدیل کانال‌های صوتی استریو به مونو فلوت [-1.0f, 1.0f]
    std::vector<float> mono_float(samples);
    for (int i = 0; i < samples; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) {
            sum += static_cast<float>(buffer[i * channels + c]);
        }
        mono_float[i] = sum / (channels * 32768.0f);
    }

    // ۲. رساپلر پویا به فرکانس ۱۶ کیلوهرتز در صورت لزوم
    if (!g_state.resampler || g_state.current_samplerate != samplerate) {
        g_state.resampler = std::make_unique<sherpa_onnx::cxx::LinearResampler>(
            sherpa_onnx::cxx::LinearResampler::Create(samplerate, 16000, 0, 6)
        );
        g_state.current_samplerate = samplerate;
    }

    std::vector<float> resampled = g_state.resampler->Resample(mono_float.data(), mono_float.size(), false);

    if (!resampled.empty()) {
        // ۳. بررسی صدا در فاز سبک VAD
        g_state.vad->AcceptWaveform(resampled.data(), resampled.size());

        // ۴. حلقه پردازش سگمنت‌های حاوی گفتار صادرشده از VAD
        while (!g_state.vad->IsEmpty()) {
            auto segment = g_state.vad->Front();
            g_state.vad->Pop();

            if (!segment.samples.empty()) {
                // ریست کردن استریم دکودر قبل از فرستادن سگمنت صوتی جدید برای رفع باگ کرش ابعاد ONNX
                g_state.spotter->Reset(g_state.stream.get());

                // ارسال صدای انسانِ تایید شده به موتور KWS
                g_state.stream->AcceptWaveform(16000, segment.samples.data(), segment.samples.size());

                // بررسی فوری و بلادرنگ نتایج در داخل لوپ IsReady جهت به حداقل رساندن لیتنسی بیدارباش
                while (g_state.spotter->IsReady(g_state.stream.get())) {
                    g_state.spotter->Decode(g_state.stream.get());

                    auto result = g_state.spotter->GetResult(g_state.stream.get());
                    if (!result.keyword.empty()) {
                        // اطلاع رسانی تشخیص به اپلیکیشن
                        NotifyJavaListener(result.keyword);
                        
                        // بازنشانی وضعیت پس از تشخیص
                        g_state.spotter->Reset(g_state.stream.get());
                        
                        // شکستن حلقه پس از تایید بیدارباش جهت جلوگیری از تحلیل بیهوده مابقی این سگمنت و ذخیره حداکثری شارژ باتری
                        break; 
                    }
                }
            }
        }
    }
}

}