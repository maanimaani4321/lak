#include "KwsManager.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath> // برای محاسبه جذر در RMS

namespace teamtalk {

static std::recursive_mutex g_mutex;
static const SherpaOnnxKeywordSpotter* g_spotter = nullptr;
static const SherpaOnnxOnlineStream* g_stream = nullptr;
static const SherpaOnnxLinearResampler* g_resampler = nullptr;
static JavaVM* g_jvm = nullptr;
static jobject g_java_listener_ref = nullptr;
static jmethodID g_callback_method_id = nullptr;
static bool g_active = false;
static int g_current_samplerate = 0;
static std::vector<float> g_mono_buffer;

// مدیریت زمان‌بندی لوله صوتی پیوسته
static int g_silence_samples_counter = 56000; 
static bool g_kws_pipe_reset_done = true;

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
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_jvm = vm;
}

bool KwsStart(JNIEnv* env, jobject jlistener,
              const std::string& encoder_path,
              const std::string& decoder_path,
              const std::string& joiner_path,
              const std::string& tokens_path,
              const std::string& bpe_path,
              const std::string& keywords_path,
              const std::string& vad_path) // پارامتر VAD باقی‌مانده تا امضای متد جاوا بهم نخورد
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_active) return true;

    if (!FileExists(encoder_path) || !FileExists(decoder_path) || !FileExists(joiner_path) ||
        !FileExists(tokens_path) || !FileExists(bpe_path) || !FileExists(keywords_path)) {
        return false;
    }

    try {
        // مقداردهی اولیه موتور تشخیص کلمه کلیدی
        SherpaOnnxKeywordSpotterConfig config;
        std::memset(&config, 0, sizeof(config));
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;
        config.model_config.transducer.encoder = encoder_path.c_str();
        config.model_config.transducer.decoder = decoder_path.c_str();
        config.model_config.transducer.joiner = joiner_path.c_str();
        config.model_config.tokens = tokens_path.c_str();
        config.model_config.bpe_vocab = bpe_path.c_str();
        config.model_config.num_threads = 2; 
        config.model_config.provider = "cpu";
        config.keywords_file = keywords_path.c_str();
        config.max_active_paths = 4;
        config.keywords_score = 2.0f;
        config.keywords_threshold = 0.15f;

        g_spotter = SherpaOnnxCreateKeywordSpotter(&config);
        if (!g_spotter) return false;

        g_stream = SherpaOnnxCreateKeywordStream(g_spotter);
        if (!g_stream) {
            SherpaOnnxDestroyKeywordSpotter(g_spotter);
            g_spotter = nullptr;
            return false;
        }

        jclass clazz = env->GetObjectClass(jlistener);
        g_callback_method_id = env->GetMethodID(clazz, "onKeywordDetected", "(Ljava/lang/String;)V");
        if (!g_callback_method_id) {
            SherpaOnnxDestroyOnlineStream(g_stream);
            SherpaOnnxDestroyKeywordSpotter(g_spotter);
            g_stream = nullptr;
            g_spotter = nullptr;
            return false;
        }

        g_java_listener_ref = env->NewGlobalRef(jlistener);
        g_active = true;
        g_current_samplerate = 0;
        g_silence_samples_counter = 56000; // شروع با فرض سکوت اولیه
        g_kws_pipe_reset_done = true;

        return true;
    } catch (...) {
        if (g_stream) SherpaOnnxDestroyOnlineStream(g_stream);
        if (g_spotter) SherpaOnnxDestroyKeywordSpotter(g_spotter);
        g_stream = nullptr;
        g_spotter = nullptr;
        return false;
    }
}

void KwsStop(JNIEnv* env) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_active) return;

    g_active = false;
    
    if (g_stream) {
        SherpaOnnxDestroyOnlineStream(g_stream);
        g_stream = nullptr;
    }   
    if (g_spotter) {
        SherpaOnnxDestroyKeywordSpotter(g_spotter);
        g_spotter = nullptr;
    }       
    if (g_resampler) {
        SherpaOnnxDestroyLinearResampler(g_resampler);
        g_resampler = nullptr;
    }
    
    g_mono_buffer.clear();

    if (g_java_listener_ref && env) {
        env->DeleteGlobalRef(g_java_listener_ref);
        g_java_listener_ref = nullptr;
    }
    g_callback_method_id = nullptr;
}

static void NotifyJavaListener(const std::string& keyword) {
    if (!g_jvm || !g_java_listener_ref || !g_callback_method_id) return;

    JNIEnv* env = nullptr;
    bool is_attached = false;
    jint res = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        res = g_jvm->AttachCurrentThread(&env, nullptr);
        if (res == JNI_OK) is_attached = true;
    }

    if (env) {
        jstring jkeyword = env->NewStringUTF(keyword.c_str());
        env->CallVoidMethod(g_java_listener_ref, g_callback_method_id, jkeyword);
        env->DeleteLocalRef(jkeyword);
    }

    if (is_attached) g_jvm->DetachCurrentThread();
}

void KwsProcessAudio(const short* buffer, int samples, int channels, int samplerate) {
    try {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_active || !g_stream || !g_spotter) return;

        // ۱. تبدیل مونو و محاسبه همزمان انرژی سیگنال (RMS) برای بیداری هوشمند لوله
        g_mono_buffer.resize(samples);
        float energy_sum = 0.0f;
        for (int i = 0; i < samples; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c) {
                sum += static_cast<float>(buffer[i * channels + c]);
            }
            float normalized = sum / (channels * 32768.0f);
            g_mono_buffer[i] = normalized;
            energy_sum += normalized * normalized;
        }

        float rms = std::sqrt(energy_sum / samples);
        // آستانه تجربی حساسیت به سکوت (مقادیر کمتر از 0.008 یعنی سکوت محیطی یا نویز ناچیز دستگاه)
        bool is_quiet = (rms < 0.008f); 

        // ۲. انطباق فرکانس به ۱۶۰۰۰ هرتز
        if (!g_resampler || g_current_samplerate != samplerate) {
            if (g_resampler) SherpaOnnxDestroyLinearResampler(g_resampler);
            g_resampler = SherpaOnnxCreateLinearResampler(samplerate, 16000, 0, 6);
            g_current_samplerate = samplerate;
        }

        const SherpaOnnxResampleOut* resampled_output = SherpaOnnxLinearResamplerResample(
            g_resampler, g_mono_buffer.data(), g_mono_buffer.size(), 0
        );

        if (resampled_output && resampled_output->samples && resampled_output->n > 0) {
            const int max_silence_samples = 3.5 * 16000; // قانون ۳.۵ ثانیه تایم‌اوت سکوت شما

            if (!is_quiet) {
                g_silence_samples_counter = 0; // بازنشانی آنی تایمر در صورت شنیدن کوچک‌ترین فرکانس انسانی
                g_kws_pipe_reset_done = false;
            } else {
                g_silence_samples_counter += resampled_output->n;
            }

            // ۳. لوله صوتی متوالی و بدون انقطاع (Continuous Pipeline)
            if (g_silence_samples_counter < max_silence_samples) {
                // دیتای متوالی مستقیماً و بدون دستکاری یا تکه‌تکه شدن به استریم دکودر تزریق می‌شود
                SherpaOnnxOnlineStreamAcceptWaveform(g_stream, 16000, resampled_output->samples, resampled_output->n);

                while (SherpaOnnxIsKeywordStreamReady(g_spotter, g_stream) == 1) {
                    SherpaOnnxDecodeKeywordStream(g_spotter, g_stream);
                }

                const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResult(g_spotter, g_stream);
                if (result) {
                    if (result->keyword && std::strlen(result->keyword) > 0) {
                        NotifyJavaListener(result->keyword);
                        SherpaOnnxResetKeywordStream(g_spotter, g_stream); // ریست بلافاصله پس از شکار کلمه کلیدی
                    }
                    SherpaOnnxDestroyKeywordResult(result);
                }
            } else {
                // ۴. مدیریت هوشمند مصرف بافر: اگر بیش از ۳.۵ ثانیه سکوت مطلق حکم‌فرما بود لوله ریست می‌شود
                if (!g_kws_pipe_reset_done) {
                    SherpaOnnxResetKeywordStream(g_spotter, g_stream); 
                    g_kws_pipe_reset_done = true;
                }
            }
        }
        
        if (resampled_output) {
            SherpaOnnxLinearResamplerResampleFree(resampled_output);
        }
    } catch (...) {
        if (g_spotter && g_stream) SherpaOnnxResetKeywordStream(g_spotter, g_stream);
    }
}

}