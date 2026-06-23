#include "KwsManager.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <android/log.h> // هدر لاگ اندروید برای دیباگ بومی

#define LOG_TAG "TeamTalk_KWS"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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

// بافر حلقوی صوتی برای تغذیه VAD در قالب بلوک‌های دقیق ۵۱۲ نمونه‌ای
static std::vector<float> g_vad_buffer;

// مدیریت ماشین وضعیت و لوله صوتی
static int g_silence_samples_counter = 56000; 
static bool g_kws_pipe_reset_done = true;
static std::vector<float> g_pre_roll_buffer; 

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

    LOGE("[سی‌پلاس‌پلاس] تلاش برای شروع متد KwsStart...");

    if (!FileExists(encoder_path) || !FileExists(decoder_path) || !FileExists(joiner_path) ||
        !FileExists(tokens_path) || !FileExists(bpe_path) || !FileExists(keywords_path) || !FileExists(vad_path)) {
        LOGE("[سی‌پلاس‌پلاس] خطا: یکی از فایل‌های مدل یا کلمات کلیدی وجود ندارد!");
        return false;
    }

    try {
        // ۱. مقداردهی اولیه VAD
        SherpaOnnxVadModelConfig vad_config;
        std::memset(&vad_config, 0, sizeof(vad_config));
        vad_config.silero_vad.model = vad_path.c_str();
        vad_config.silero_vad.threshold = 0.40f; 
        vad_config.silero_vad.min_silence_duration = 0.5f;
        vad_config.silero_vad.min_speech_duration = 0.10f; 
        vad_config.silero_vad.window_size = 512;
        vad_config.silero_vad.max_speech_duration = 20.0f;
        vad_config.sample_rate = 16000;
        vad_config.num_threads = 1;
        vad_config.provider = "cpu";

        g_vad = SherpaOnnxCreateVoiceActivityDetector(&vad_config, 10.0f);
        if (!g_vad) {
            LOGE("[سی‌پلاس‌پلاس] خطا در ساخت VoiceActivityDetector");
            return false;
        }

        // ۲. مقداردهی اولیه KeywordSpotter
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
            LOGE("[سی‌پلاس‌پلاس] خطا در ساخت KeywordSpotter");
            SherpaOnnxDestroyVoiceActivityDetector(g_vad);
            g_vad = nullptr;
            return false;
        }

        g_stream = SherpaOnnxCreateKeywordStream(g_spotter);
        if (!g_stream) {
            LOGE("[سی‌پلاس‌پلاس] خطا در ساخت KeywordStream");
            SherpaOnnxDestroyKeywordSpotter(g_spotter);
            SherpaOnnxDestroyVoiceActivityDetector(g_vad);
            g_spotter = nullptr;
            g_vad = nullptr;
            return false;
        }

        jclass clazz = env->GetObjectClass(jlistener);
        g_callback_method_id = env->GetMethodID(clazz, "onKeywordDetected", "(Ljava/lang/String;)V");
        if (!g_callback_method_id) {
            LOGE("[سی‌پلاس‌پلاس] خطا: متد Callback در جاوا یافت نشد!");
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
        g_silence_samples_counter = 56000; 
        g_kws_pipe_reset_done = true;
        g_pre_roll_buffer.clear();
        g_vad_buffer.clear();

        LOGE("[سی‌پلاس‌پلاس] موتور بیداری با موفقیت شروع به کار کرد.");
        return true;
    } catch (...) {
        LOGE("[سی‌پلاس‌پلاس] خطای ناشناخته در بخش شروع کار!");
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

    LOGE("[سی‌پلاس‌پلاس] تلاش برای متوقف کردن موتور تشخیص کلمه...");
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
    g_vad_buffer.clear();

    if (g_java_listener_ref && env) {
        env->DeleteGlobalRef(g_java_listener_ref);
        g_java_listener_ref = nullptr;
    }
    g_callback_method_id = nullptr;
    LOGE("[سی‌پلاس‌پلاس] موتور تشخیص کلمه با موفقیت متوقف شد.");
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

        // تبدیل کانال‌ها و نرمال‌سازی سیگنال صدا
        g_mono_buffer.resize(samples);
        for (int i = 0; i < samples; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c) {
                sum += static_cast<float>(buffer[i * channels + c]);
            }
            g_mono_buffer[i] = sum / (channels * 32768.0f);
        }

        // همسان‌سازی فرکانس نمونه‌برداری به ۱۶۰۰۰ هرتز
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
            
            // الف) تجمیع نمونه‌های جدید صوتی در بافر VAD جهت فیلتر نویز ابعاد ONNX
            g_vad_buffer.insert(g_vad_buffer.end(), resampled_output->samples, resampled_output->samples + resampled_output->n);

            // ب) فید دادن صوتی به VAD *فقط و فقط* در قالب بلوک‌های دقیق ۵۱۲ نمونه‌ای
            while (g_vad_buffer.size() >= 512) {
                SherpaOnnxVoiceActivityDetectorAcceptWaveform(g_vad, g_vad_buffer.data(), 512);
                g_vad_buffer.erase(g_vad_buffer.begin(), g_vad_buffer.begin() + 512);
            }

            // ۲. تخلیه آنی صف سگمنت‌های بومی VAD
            SherpaOnnxVoiceActivityDetectorClear(g_vad);

            // ۳. دریافت وضعیت آنی تشخیص صدای انسان (بدون خطای ابعاد ONNX)
            bool speech_detected_now = (SherpaOnnxVoiceActivityDetectorDetected(g_vad) == 1);

            // زمان بیداری صوتی (۳.۵ ثانیه در فرکانس ۱۶ کیلوهرتز)
            const int max_silence_samples = 3.5 * 16000; 

            if (speech_detected_now) {
                if (g_silence_samples_counter >= max_silence_samples && !g_pre_roll_buffer.empty()) {
                    LOGE("[سی‌پلاس‌پلاس] صدای انسان شنیده شد! در حال تزریق بافر پیش‌رو...");
                    SherpaOnnxOnlineStreamAcceptWaveform(g_stream, 16000, g_pre_roll_buffer.data(), g_pre_roll_buffer.size());
                    g_pre_roll_buffer.clear();
                }
                g_silence_samples_counter = 0; 
                g_kws_pipe_reset_done = false;
            } else {
                g_silence_samples_counter += resampled_output->n;
            }

            // ۴. لوله دکود مداوم کلمات
            if (g_silence_samples_counter < max_silence_samples) {
                // تزریق جریان پیوسته صدا به مدل دکودر
                SherpaOnnxOnlineStreamAcceptWaveform(g_stream, 16000, resampled_output->samples, resampled_output->n);

                // عملیات رمزگشایی فریم‌های صوتی
                while (SherpaOnnxIsKeywordStreamReady(g_spotter, g_stream) == 1) {
                    LOGE("[سی‌پلاس‌پلاس] در حال رمزگشایی و جستجوی کلمه کلیدی...");
                    SherpaOnnxDecodeKeywordStream(g_spotter, g_stream);
                }

                // بررسی نتیجه استخراج کلمه
                const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResult(g_spotter, g_stream);
                if (result) {
                    if (result->keyword && std::strlen(result->keyword) > 0) {
                        LOGE("[سی‌پلاس‌پلاس] کلمه بیداری با موفقیت تشخیص داده شد: %s", result->keyword);
                        NotifyJavaListener(result->keyword);
                        SherpaOnnxResetKeywordStream(g_spotter, g_stream);
                    }
                    SherpaOnnxDestroyKeywordResult(result);
                }
            } else {
                // ۵. صرفه‌جویی شدید در مصرف باتری در صورت سکوت طولانی‌مدت (بیش از ۳.۵ ثانیه):
                if (!g_kws_pipe_reset_done) {
                    LOGE("[سی‌پلاس‌پلاس] سکوت طولانی؛ استریم موقتاً ریست شد.");
                    SherpaOnnxResetKeywordStream(g_spotter, g_stream); 
                    g_kws_pipe_reset_done = true;
                    g_pre_roll_buffer.clear();
                }

                // ذخیره ثانیه‌های آخر سکوت محیط جهت چسبندگی صوتی در بیداری بعدی
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
        LOGE("[سی‌پلاس‌پلاس] خطا و کرش صوتی در KwsProcessAudio!");
        if (g_spotter && g_stream) {
            SherpaOnnxResetKeywordStream(g_spotter, g_stream);
        }
    }
}

} // namespace teamtalk