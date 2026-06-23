#include "KwsManager.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>

namespace teamtalk {

static std::recursive_mutex g_mutex;
static const SherpaOnnxVoiceActivityDetector* g_vad = nullptr;
static const SherpaOnnxKeywordSpotter* g_spotter = nullptr;
static const SherpaOnnxOnlineStream* g_stream = nullptr;
static const SherpaOnnxLinearResampler* g_resampler = nullptr;
static JavaVM* g_jvm = nullptr;
static jobject g_java_listener_ref = nullptr;
static jmethodID g_callback_method_id = nullptr;
static bool g_active = false;
static int g_current_samplerate = 0;
static std::vector<float> g_mono_buffer;

// مدیریت وضعیت لوله صوتی و بیداری مبتنی بر فعالیت گفتار
static int g_silence_samples_counter = 56000; // شروع با وضعیت سکوت فرضی اولیه (۳.۵ ثانیه)
static bool g_kws_pipe_reset_done = true;
static std::vector<float> g_pre_roll_buffer;   // بافر سکوت پیش از شروع صحبت جهت جلوگیری از قطع شدن هجاهای اول

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
              const std::string& vad_path)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_active) return true;

    if (!FileExists(encoder_path) || !FileExists(decoder_path) || !FileExists(joiner_path) ||
        !FileExists(tokens_path) || !FileExists(bpe_path) || !FileExists(keywords_path) || !FileExists(vad_path)) {
        return false;
    }

    try {
        // ۱. مقداردهی اولیه VAD با تنظیمات بهینه
        SherpaOnnxVadModelConfig vad_config;
        std::memset(&vad_config, 0, sizeof(vad_config));
        vad_config.silero_vad.model = vad_path.c_str();
        vad_config.silero_vad.threshold = 0.40f; // آستانه حساسیت برای نادیده گرفتن نویز محیط
        vad_config.silero_vad.min_silence_duration = 0.5f;
        vad_config.silero_vad.min_speech_duration = 0.10f; 
        vad_config.silero_vad.window_size = 512;
        vad_config.silero_vad.max_speech_duration = 20.0f;
        vad_config.sample_rate = 16000;
        vad_config.num_threads = 1;
        vad_config.provider = "cpu";

        g_vad = SherpaOnnxCreateVoiceActivityDetector(&vad_config, 10.0f);
        if (!g_vad) return false;

        // ۲. مقداردهی اولیه مدل تشخیص کلمه کلیدی (Keyword Spotter)
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
        if (!g_spotter) {
            SherpaOnnxDestroyVoiceActivityDetector(g_vad);
            g_vad = nullptr;
            return false;
        }

        g_stream = SherpaOnnxCreateKeywordStream(g_spotter);
        if (!g_stream) {
            SherpaOnnxDestroyKeywordSpotter(g_spotter);
            SherpaOnnxDestroyVoiceActivityDetector(g_vad);
            g_spotter = nullptr;
            g_vad = nullptr;
            return false;
        }

        jclass clazz = env->GetObjectClass(jlistener);
        g_callback_method_id = env->GetMethodID(clazz, "onKeywordDetected", "(Ljava/lang/String;)V");
        if (!g_callback_method_id) {
            SherpaOnnxDestroyOnlineStream(g_stream);
            SherpaOnnxDestroyKeywordSpotter(g_spotter);
            SherpaOnnxDestroyVoiceActivityDetector(g_vad);
            g_stream = nullptr;
            g_spotter = nullptr;
            g_vad = nullptr;
            return false;
        }

        g_java_listener_ref = env->NewGlobalRef(jlistener);
        g_active = true;
        g_current_samplerate = 0;
        g_silence_samples_counter = 56000; // آماده‌باش اولیه
        g_kws_pipe_reset_done = true;
        g_pre_roll_buffer.clear();

        return true;
    } catch (...) {
        if (g_stream) SherpaOnnxDestroyOnlineStream(g_stream);
        if (g_spotter) SherpaOnnxDestroyKeywordSpotter(g_spotter);
        if (g_vad) SherpaOnnxDestroyVoiceActivityDetector(g_vad);
        g_stream = nullptr;
        g_spotter = nullptr;
        g_vad = nullptr;
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
    if (g_vad) {
        SherpaOnnxDestroyVoiceActivityDetector(g_vad);
        g_vad = nullptr;
    }      
    if (g_resampler) {
        SherpaOnnxDestroyLinearResampler(g_resampler);
        g_resampler = nullptr;
    }
    
    g_mono_buffer.clear();
    g_pre_roll_buffer.clear();

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
        if (res == JNI_OK) {
            is_attached = true;
        }
    }

    if (env) {
        jstring jkeyword = env->NewStringUTF(keyword.c_str());
        env->CallVoidMethod(g_java_listener_ref, g_callback_method_id, jkeyword);
        env->DeleteLocalRef(jkeyword);
    }

    if (is_attached) {
        g_jvm->DetachCurrentThread();
    }
}

void KwsProcessAudio(const short* buffer, int samples, int channels, int samplerate) {
    try {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_active || !g_stream || !g_spotter || !g_vad) return;

        // کانال‌بندی و تبدیل به نمونه‌های مونو با مقدار نرمال‌سازی شده [-1.0f, 1.0f]
        g_mono_buffer.resize(samples);
        for (int i = 0; i < samples; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c) {
                sum += static_cast<float>(buffer[i * channels + c]);
            }
            g_mono_buffer[i] = sum / (channels * 32768.0f);
        }

        // انطباق مداوم فرکانس نمونه‌برداری به ۱۶۰۰۰ هرتز (استاندارد مدل‌ها)
        if (!g_resampler || g_current_samplerate != samplerate) {
            if (g_resampler) {
                SherpaOnnxDestroyLinearResampler(g_resampler);
            }
            g_resampler = SherpaOnnxCreateLinearResampler(samplerate, 16000, 0, 6);
            g_current_samplerate = samplerate;
        }

        const SherpaOnnxResampleOut* resampled_output = SherpaOnnxLinearResamplerResample(
            g_resampler,
            g_mono_buffer.data(),
            g_mono_buffer.size(),
            0
        );

        if (resampled_output && resampled_output->samples && resampled_output->n > 0) {
            // ۱. تزریق مداوم فریم‌ها به ماژول VAD
            SherpaOnnxVoiceActivityDetectorAcceptWaveform(g_vad, resampled_output->samples, resampled_output->n);

            // ۲. تخلیه آنی و ۱۰۰٪ ایمن کل صف VAD در هسته بومی برای جلوگیری از انباشت حافظه (بدون نیاز به تخصیص Front/Pop در C)
            SherpaOnnxVoiceActivityDetectorClear(g_vad);

            // ۳. دریافت درلحظه وضعیت تشخیص صدای انسان بدون انتظار برای پایان سگمنت
            bool speech_detected_now = (SherpaOnnxVoiceActivityDetectorDetected(g_vad) == 1);

            // مدیریت زمان بیداری (تایمر ۳.۵ ثانیه‌ای در فرکانس ۱۶ کیلوهرتز)
            const int max_silence_samples = 3.5 * 16000; 

            if (speech_detected_now) {
                // تزریق بافر پیش‌رو (سکوت و هجاهای شروع صحبت) به مجرای تشخیص کلمه
                if (g_silence_samples_counter >= max_silence_samples && !g_pre_roll_buffer.empty()) {
                    SherpaOnnxOnlineStreamAcceptWaveform(g_stream, 16000, g_pre_roll_buffer.data(), g_pre_roll_buffer.size());
                    g_pre_roll_buffer.clear();
                }
                g_silence_samples_counter = 0; // بازنشانی تایمر سکوت
                g_kws_pipe_reset_done = false;
            } else {
                g_silence_samples_counter += resampled_output->n;
            }

            // ۴. لوله پردازش متوالی کلمات (KWS Pipeline)
            if (g_silence_samples_counter < max_silence_samples) {
                // تزریق پیوسته فریم‌ها به موتور دکودر
                SherpaOnnxOnlineStreamAcceptWaveform(g_stream, 16000, resampled_output->samples, resampled_output->n);

                // چرخه دکودینگ تا خالی شدن کامل بافر ورودی
                while (SherpaOnnxIsKeywordStreamReady(g_spotter, g_stream) == 1) {
                    SherpaOnnxDecodeKeywordStream(g_spotter, g_stream);
                }

                // بررسی نهایی کلمه کلیدی استخراج‌شده
                const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResult(g_spotter, g_stream);
                if (result) {
                    if (result->keyword && std::strlen(result->keyword) > 0) {
                        NotifyJavaListener(result->keyword);
                        SherpaOnnxResetKeywordStream(g_spotter, g_stream);
                    }
                    SherpaOnnxDestroyKeywordResult(result);
                }
            } else {
                // ۵. صرفه‌جویی شدید در مصرف باتری: اگر بیش از ۳.۵ ثانیه سکوت برقرار شد:
                if (!g_kws_pipe_reset_done) {
                    SherpaOnnxResetKeywordStream(g_spotter, g_stream); // پاکسازی حافظه دکودر KWS
                    g_kws_pipe_reset_done = true;
                    g_pre_roll_buffer.clear();
                }

                // ثبت ۱ ثانیه سکوت نهایی محیط جهت بازیابی فیلترهای فرکانسی در تلاش بعدی بیداری
                g_pre_roll_buffer.insert(g_pre_roll_buffer.end(), resampled_output->samples, resampled_output->samples + resampled_output->n);
                if (g_pre_roll_buffer.size() > 16000) {
                    g_pre_roll_buffer.erase(g_pre_roll_buffer.begin(), g_pre_roll_buffer.end() - 16000);
                }
            }
        }
        
        if (resampled_output) {
            SherpaOnnxLinearResamplerResampleFree(resampled_output);
        }
    } catch (...) {
        if (g_spotter && g_stream) {
            SherpaOnnxResetKeywordStream(g_spotter, g_stream);
        }
    }
}

}