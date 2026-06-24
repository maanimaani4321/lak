#include "KwsManager.h"
#include "sherpa-onnx/c-api/cxx-api.h"
#include <mutex>
#include <memory>
#include <vector>
#include <algorithm>

namespace teamtalk {

struct KwsInternalState {
    std::recursive_mutex mutex;
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
              const std::string& keywords_path)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    if (g_state.active) return true;

    // بررسی دقیق وجود فایل‌ها جهت تضمین ۱۰۰ درصدی عدم کرش برنامه
    if (!FileExists(encoder_path) || !FileExists(decoder_path) || !FileExists(joiner_path) ||
        !FileExists(tokens_path) || !FileExists(bpe_path) || !FileExists(keywords_path)) {
        return false;
    }

    try {
        sherpa_onnx::cxx::KeywordSpotterConfig config;
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;

        config.model_config.transducer.encoder = encoder_path;
        config.model_config.transducer.decoder = decoder_path;
        config.model_config.transducer.joiner = joiner_path;
        config.model_config.tokens = tokens_path;
        config.model_config.bpe_vocab = bpe_path;
        config.model_config.num_threads = 2; 
        config.model_config.provider = "cpu";

        config.keywords_file = keywords_path;
        config.max_active_paths = 4;
        config.keywords_score = 1.5f;
        config.keywords_threshold = 0.25f;

        g_state.spotter = std::make_unique<sherpa_onnx::cxx::KeywordSpotter>(
            sherpa_onnx::cxx::KeywordSpotter::Create(config)
        );

        if (!g_state.spotter || !g_state.spotter->Get()) {
            g_state.spotter.reset();
            return false;
        }

        g_state.stream = std::make_unique<sherpa_onnx::cxx::OnlineStream>(
            g_state.spotter->CreateStream()
        );

        jclass clazz = env->GetObjectClass(jlistener);
        g_state.callback_method_id = env->GetMethodID(clazz, "onKeywordDetected", "(Ljava/lang/String;)V");
        if (!g_state.callback_method_id) {
            g_state.spotter.reset();
            g_state.stream.reset();
            return false;
        }

        g_state.java_listener_ref = env->NewGlobalRef(jlistener);
        g_state.active = true;
        g_state.current_samplerate = 0; // وادار کردن رساپلر به تنظیم مجدد در اولین فریم صوتی

        return true;
    } catch (...) {
        g_state.spotter.reset();
        g_state.stream.reset();
        return false;
    }
}

void KwsStop(JNIEnv* env) {
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    if (!g_state.active) return;

    g_state.active = false;
    g_state.spotter.reset();
    g_state.stream.reset();
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
    if (!g_state.active || !g_state.stream || !g_state.spotter) return;

    // تبدیل داده‌های استریو یا مونو ورودی به فرمت شناور مونو و نرمال‌شده [-1.0f, 1.0f]
    std::vector<float> mono_float(samples);
    for (int i = 0; i < samples; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) {
            sum += static_cast<float>(buffer[i * channels + c]);
        }
        mono_float[i] = sum / (channels * 32768.0f);
    }

    // ایجاد یا به‌روزرسانی رساپلر پویا بر اساس فرکانس نمونه‌برداری میکروفون اوبو
    if (!g_state.resampler || g_state.current_samplerate != samplerate) {
        g_state.resampler = std::make_unique<sherpa_onnx::cxx::LinearResampler>(
            sherpa_onnx::cxx::LinearResampler::Create(samplerate, 16000, 0, 6)
        );
        g_state.current_samplerate = samplerate;
    }

    // انجام رساپلینگ به 16000 هرتز
    std::vector<float> resampled = g_state.resampler->Resample(mono_float.data(), mono_float.size(), false);

    if (!resampled.empty()) {
        g_state.stream->AcceptWaveform(16000, resampled.data(), resampled.size());

        while (g_state.spotter->IsReady(g_state.stream.get())) {
            g_state.spotter->Decode(g_state.stream.get());
        }

        auto result = g_state.spotter->GetResult(g_state.stream.get());
        if (!result.keyword.empty()) {
            NotifyJavaListener(result.keyword);
            g_state.spotter->Reset(g_state.stream.get());
        }
    }
}

}