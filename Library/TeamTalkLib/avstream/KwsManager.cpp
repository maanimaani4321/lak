#include "KwsManager.h"
#include "codec/OggFileIO.h"
#include "codec/MediaUtil.h"
#include "avstream/AudioResampler.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <atomic> // هدر اتمیک برای متغیرهای سبک و سریع

extern "C" void TT_UpdateBackgroundMicAll();

namespace teamtalk {

static std::recursive_mutex g_mutex;
static const SherpaOnnxVoiceActivityDetector* g_vad = nullptr;
static const SherpaOnnxKeywordSpotter* g_spotter = nullptr;
static const SherpaOnnxOnlineStream* g_stream = nullptr;
static const SherpaOnnxLinearResampler* g_resampler = nullptr;

// فیلدهای کمکی سیستم تأیید هویت گوینده
static const SherpaOnnxSpeakerEmbeddingExtractor* g_speaker_extractor = nullptr;
static bool g_speaker_verify_enabled = false;
static float g_speaker_verify_threshold = 0.5f;
static std::vector<float> g_target_speaker_embedding;
static std::vector<float> g_speaker_audio_buffer; // بافر لغزان ۳ ثانیه‌ای (۴۸۰۰۰ سمپل در فرکانس ۱۶ کیلوهرتز)

// مدیریت ماشین وضعیت با استفاده از نوع داده اتمیک سبک
enum KwsState {
    STATE_KWS_ACTIVE,
    STATE_ENROLLMENT_ACTIVE,
    STATE_INACTIVE
};
static std::atomic<int> g_state{STATE_INACTIVE};
static std::vector<float> g_enrollment_speech_buffer; // بافر جمع‌آوری صدای انسان برای تولید امضای صوتی

static JavaVM* g_jvm = nullptr;
static jobject g_java_listener_ref = nullptr;
static jmethodID g_callback_method_id = nullptr;
static jmethodID g_enroll_callback_method_id = nullptr;
static jmethodID g_recording_finished_method_id = nullptr;

// تغییر متغیرها به ساختار اتمیک جهت جلوگیری از قفل‌های سنگین در زمان ارزیابی وضعیت میکروفون
static std::atomic<bool> g_active{false};
static int g_current_samplerate = 0;
static std::vector<float> g_mono_buffer;

// بافر حلقوی صوتی برای تغذیه VAD در قالب بلوک‌های دقیق ۵۱۲ نمونه‌ای
static std::vector<float> g_vad_buffer;

// مدیریت ماشین وضعیت و لوله صوتی
static int g_silence_samples_counter = 56000; 
static bool g_kws_pipe_reset_done = true;
static std::vector<float> g_pre_roll_buffer; 

// ۱. تابع کمکی بررسی وجود فایل‌ها در حافظه
static bool FileExists(const std::string& path) {
    if (path.empty()) return false;
    FILE* f = fopen(path.c_str(), "r");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

// ۲. فرمول ریاضی محاسبه شباهت کسینوسی برداری
static float CosineSimilarity(const float* a, const float* b, int dim) {
    float dot_product = 0.0f;
    float norm_a = 0.0f;
    float norm_b = 0.0f;
    for (int i = 0; i < dim; ++i) {
        dot_product += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if (norm_a == 0.0f || norm_b == 0.0f) return 0.0f;
    return dot_product / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

// ۳. تابع ارسال سیگنال دتکت کلمه کلیدی به جاوا (بالای لوله پردازش صوتی قرار گرفت)
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

// ۴. ارزیابی تطبیق امضای صوتی با امضای هدف
static bool VerifySpeaker(const std::vector<float>& audio_samples) {
    if (audio_samples.empty() || !g_speaker_extractor) return false;

    const SherpaOnnxOnlineStream* speaker_stream = SherpaOnnxSpeakerEmbeddingExtractorCreateStream(g_speaker_extractor);
    if (!speaker_stream) return false;

    SherpaOnnxOnlineStreamAcceptWaveform(speaker_stream, 16000, audio_samples.data(), audio_samples.size());
    SherpaOnnxOnlineStreamInputFinished(speaker_stream);

    bool matched = false;
    if (SherpaOnnxSpeakerEmbeddingExtractorIsReady(g_speaker_extractor, speaker_stream) == 1) {
        const float* embedding = SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding(g_speaker_extractor, speaker_stream);
        if (embedding) {
            int32_t dim = SherpaOnnxSpeakerEmbeddingExtractorDim(g_speaker_extractor);
            float score = CosineSimilarity(embedding, g_target_speaker_embedding.data(), dim);
            if (score >= g_speaker_verify_threshold) {
                matched = true;
            }
            SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding(embedding);
        }
    }
    SherpaOnnxDestroyOnlineStream(speaker_stream);
    return matched;
}

// ۵. تولید امضای صوتی جدید و ارسال آن به جاوا در حالت ثبت نام (Enrollment)
static void ExtractAndNotifyEnrollmentEmbedding(const std::vector<float>& audio_samples) {
    if (!g_jvm || !g_java_listener_ref || !g_enroll_callback_method_id || !g_speaker_extractor) return;

    const SherpaOnnxOnlineStream* speaker_stream = SherpaOnnxSpeakerEmbeddingExtractorCreateStream(g_speaker_extractor);
    if (!speaker_stream) return;

    SherpaOnnxOnlineStreamAcceptWaveform(speaker_stream, 16000, audio_samples.data(), audio_samples.size());
    SherpaOnnxOnlineStreamInputFinished(speaker_stream);

    std::vector<float> extracted_embedding;
    if (SherpaOnnxSpeakerEmbeddingExtractorIsReady(g_speaker_extractor, speaker_stream) == 1) {
        const float* embedding = SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding(g_speaker_extractor, speaker_stream);
        if (embedding) {
            int32_t dim = SherpaOnnxSpeakerEmbeddingExtractorDim(g_speaker_extractor);
            extracted_embedding.assign(embedding, embedding + dim);
            SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding(embedding);
        }
    }
    SherpaOnnxDestroyOnlineStream(speaker_stream);

    if (extracted_embedding.empty()) return;

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
        jfloatArray j_embedding = env->NewFloatArray(extracted_embedding.size());
        if (j_embedding) {
            env->SetFloatArrayRegion(j_embedding, 0, extracted_embedding.size(), extracted_embedding.data());
            env->CallVoidMethod(g_java_listener_ref, g_enroll_callback_method_id, j_embedding);
            env->DeleteLocalRef(j_embedding);
        }
    }

    if (is_attached) {
        g_jvm->DetachCurrentThread();
    }
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
    return KwsStartEx(env, jlistener, encoder_path, decoder_path, joiner_path,
                      tokens_path, bpe_path, keywords_path, vad_path,
                      "", 0.40f, false, 0.5f, std::vector<float>());
}

bool KwsStartEx(JNIEnv* env, jobject jlistener,
                const std::string& encoder_path,
                const std::string& decoder_path,
                const std::string& joiner_path,
                const std::string& tokens_path,
                const std::string& bpe_path,
                const std::string& keywords_path,
                const std::string& vad_path,
                const std::string& wespeaker_path,
                float vad_threshold,
                bool speaker_verify_enabled,
                float speaker_verify_threshold,
                const std::vector<float>& target_speaker_embedding)
{
    // بررسی سریع وضعیت بدون نیاز به قفل کردن ملوتکس به جهت کاهش سربار مصرف پردازنده
    if (g_active.load(std::memory_order_relaxed)) return true;

    if (!FileExists(encoder_path) || !FileExists(decoder_path) || !FileExists(joiner_path) ||
        !FileExists(tokens_path) || !FileExists(bpe_path) || !FileExists(keywords_path) || !FileExists(vad_path)) {
        return false;
    }

    if (speaker_verify_enabled && !wespeaker_path.empty() && !FileExists(wespeaker_path)) {
        return false;
    }

    bool success = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        try {
            // ۱. مقداردهی اولیه VAD
            SherpaOnnxVadModelConfig vad_config;
            std::memset(&vad_config, 0, sizeof(vad_config));
            vad_config.silero_vad.model = vad_path.c_str();
            vad_config.silero_vad.threshold = vad_threshold; 
            vad_config.silero_vad.min_silence_duration = 0.5f;
            vad_config.silero_vad.min_speech_duration = 0.10f; 
            vad_config.silero_vad.window_size = 512;
            vad_config.silero_vad.max_speech_duration = 20.0f;
            vad_config.sample_rate = 16000;
            vad_config.num_threads = 1;
            vad_config.provider = "cpu";

            g_vad = SherpaOnnxCreateVoiceActivityDetector(&vad_config, 10.0f);
            if (!g_vad) {
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
            config.model_config.model_type = "zipformer2"; 
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

            // ۳. مقداردهی اولیه Speaker Embedding Extractor
            if (!wespeaker_path.empty()) {
                SherpaOnnxSpeakerEmbeddingExtractorConfig extractor_config;
                std::memset(&extractor_config, 0, sizeof(extractor_config));
                extractor_config.model = wespeaker_path.c_str();
                extractor_config.num_threads = 1;
                extractor_config.debug = 0;
                extractor_config.provider = "cpu";

                g_speaker_extractor = SherpaOnnxCreateSpeakerEmbeddingExtractor(&extractor_config);
                if (!g_speaker_extractor) {
                    SherpaOnnxDestroyOnlineStream(g_stream);
                    SherpaOnnxDestroyKeywordSpotter(g_spotter);
                    SherpaOnnxDestroyVoiceActivityDetector(g_vad);
                    g_stream = nullptr;
                    g_spotter = nullptr;
                    g_vad = nullptr;
                    return false;
                }
            }

            jclass clazz = env->GetObjectClass(jlistener);
            g_callback_method_id = env->GetMethodID(clazz, "onKeywordDetected", "(Ljava/lang/String;)V");
            
            g_enroll_callback_method_id = env->GetMethodID(clazz, "onSpeakerEmbeddingExtracted", "([F)V");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                g_enroll_callback_method_id = nullptr;
            }

            g_recording_finished_method_id = env->GetMethodID(clazz, "onVoiceRecordingFinished", "(Ljava/lang/String;I)V");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                g_recording_finished_method_id = nullptr;
            }

            if (!g_callback_method_id) {
                if (g_speaker_extractor) {
                    SherpaOnnxDestroySpeakerEmbeddingExtractor(g_speaker_extractor);
                    g_speaker_extractor = nullptr;
                }
                SherpaOnnxDestroyOnlineStream(g_stream);
                SherpaOnnxDestroyKeywordSpotter(g_spotter);
                SherpaOnnxDestroyVoiceActivityDetector(g_vad);
                g_stream = nullptr;
                g_spotter = nullptr;
                g_vad = nullptr;
                return false;
            }

            g_java_listener_ref = env->NewGlobalRef(jlistener);
            g_speaker_verify_enabled = speaker_verify_enabled;
            g_speaker_verify_threshold = speaker_verify_threshold;
            g_target_speaker_embedding = target_speaker_embedding;

            g_current_samplerate = 0;
            g_silence_samples_counter = 56000; 
            g_kws_pipe_reset_done = true;
            g_pre_roll_buffer.clear();
            g_vad_buffer.clear();
            g_speaker_audio_buffer.clear();
            g_enrollment_speech_buffer.clear();

            // تنظیم مقادیر اتمیک در انتهای فرآیند بالا آمدن منابع صوتی موتور ONNX
            g_active.store(true, std::memory_order_release);
            g_state.store(STATE_KWS_ACTIVE, std::memory_order_release);
            success = true;
        } catch (...) {
            if (g_speaker_extractor) {
                SherpaOnnxDestroySpeakerEmbeddingExtractor(g_speaker_extractor);
                g_speaker_extractor = nullptr;
            }
            if (g_stream) SherpaOnnxDestroyOnlineStream(g_stream);
            if (g_spotter) SherpaOnnxDestroyKeywordSpotter(g_spotter);
            if (g_vad) SherpaOnnxDestroyVoiceActivityDetector(g_vad);
            g_stream = nullptr;
            g_spotter = nullptr;
            g_vad = nullptr;
            return false;
        }
    }

    // خروج از محدوده قفل بالا قبل از اطلاع‌رسانی به هسته جهت ممانعت قاطع از Deadlock
    if (success) {
        TT_UpdateBackgroundMicAll();
    }
    return success;
}

bool KwsStartSpeakerEnrollment(JNIEnv* env) {
    bool success = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_active.load(std::memory_order_relaxed) || !g_speaker_extractor) return false;
        g_state.store(STATE_ENROLLMENT_ACTIVE, std::memory_order_release);
        g_enrollment_speech_buffer.clear();
        success = true;
    }
    if (success) {
        TT_UpdateBackgroundMicAll();
    }
    return true;
}

void KwsStop(JNIEnv* env) {
    bool was_active = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_active.load(std::memory_order_relaxed)) return;

        g_active.store(false, std::memory_order_release);
        g_state.store(STATE_INACTIVE, std::memory_order_release);
        
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
        if (g_speaker_extractor) {
            SherpaOnnxDestroySpeakerEmbeddingExtractor(g_speaker_extractor);
            g_speaker_extractor = nullptr;
        }
        
        g_mono_buffer.clear();
        g_pre_roll_buffer.clear();
        g_vad_buffer.clear();
        g_speaker_audio_buffer.clear();
        g_enrollment_speech_buffer.clear();
        g_target_speaker_embedding.clear();

        if (g_java_listener_ref && env) {
            env->DeleteGlobalRef(g_java_listener_ref);
            g_java_listener_ref = nullptr;
        }
        g_callback_method_id = nullptr;
        g_enroll_callback_method_id = nullptr;
        was_active = true;
    }

    if (was_active) {
        TT_UpdateBackgroundMicAll();
    }
}

void KwsProcessAudio(const short* buffer, int samples, int channels, int samplerate) {
    // بهینه‌سازی بسیار سبک: اگر کلید استارت کلاینت زده نشده، فورا خارج شو بدون اینکه قفل گارد سنگین بگیری
    if (!g_active.load(std::memory_order_relaxed)) return;

    try {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_active.load() || !g_stream || !g_spotter || !g_vad) return;

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

            // ۳. دریافت وضعیت آنی تشخیص صدای انسان
            bool speech_detected_now = (SherpaOnnxVoiceActivityDetectorDetected(g_vad) == 1);

            // حالت اول: دتکشن کلمات فعال است (KWS MODE)
            int current_state = g_state.load(std::memory_order_relaxed);
            if (current_state == STATE_KWS_ACTIVE) {
                
                // مدیریت بافرینگ مداوم ۳ ثانیه آخر صدا جهت تایید هویت در صورت دتکشن کلمه کلیدی
                g_speaker_audio_buffer.insert(g_speaker_audio_buffer.end(), resampled_output->samples, resampled_output->samples + resampled_output->n);
                if (g_speaker_audio_buffer.size() > 48000) {
                    g_speaker_audio_buffer.erase(g_speaker_audio_buffer.begin(), g_speaker_audio_buffer.end() - 48000);
                }

                // زمان بیداری صوتی (۳.۵ ثانیه در فرکانس ۱۶ کیلوهرتز)
                const int max_silence_samples = 3.5 * 16000; 

                if (speech_detected_now) {
                    // اگر فعالیت انسان تازه تشخیص داده شد، بافر سکوت قبل از کلمه را تزریق کن
                    if (g_silence_samples_counter >= max_silence_samples && !g_pre_roll_buffer.empty()) {
                        SherpaOnnxOnlineStreamAcceptWaveform(g_stream, 16000, g_pre_roll_buffer.data(), g_pre_roll_buffer.size());
                        g_pre_roll_buffer.clear();
                    }
                    g_silence_samples_counter = 0; // ریست شمارنده سکوت
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
                        SherpaOnnxDecodeKeywordStream(g_spotter, g_stream);
                    }

                    // بررسی نتیجه استخراج کلمه
                    const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResult(g_spotter, g_stream);
                    if (result) {
                        if (result->keyword && std::strlen(result->keyword) > 0) {
                            
                            // ارزیابی تطبیق امضای صوتی گوینده در صورت فعال بودن
                            bool is_speaker_verified = true;
                            if (g_speaker_verify_enabled && g_speaker_extractor && !g_target_speaker_embedding.empty()) {
                                is_speaker_verified = VerifySpeaker(g_speaker_audio_buffer);
                            }

                            if (is_speaker_verified) {
                                NotifyJavaListener(result->keyword);
                            }
                            
                            SherpaOnnxResetKeywordStream(g_spotter, g_stream);
                        }
                        SherpaOnnxDestroyKeywordResult(result);
                    }
                } else {
                    // ۵. صرفه‌جویی شدید در مصرف باتری در صورت سکوت طولانی‌مدت (بیش از ۳.۵ ثانیه):
                    if (!g_kws_pipe_reset_done) {
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
            // حالت دوم: استخراج نمونه امضای صوتی گوینده فعال است (ENROLLMENT MODE)
            else if (current_state == STATE_ENROLLMENT_ACTIVE) {
                // اگر صدای انسان توسط VAD تشخیص داده شد، نمونه‌ها را در بافر ثبت امضای صوتی ذخیره کن
                if (speech_detected_now) {
                    g_enrollment_speech_buffer.insert(g_enrollment_speech_buffer.end(), resampled_output->samples, resampled_output->samples + resampled_output->n);
                    
                    // به محض اتمام دریافت ۳ ثانیه مداوم از صدای گوینده (۴۸۰۰۰ سمپل):
                    if (g_enrollment_speech_buffer.size() >= 48000) {
                        ExtractAndNotifyEnrollmentEmbedding(g_enrollment_speech_buffer);
                        g_enrollment_speech_buffer.clear();
                        g_state.store(STATE_KWS_ACTIVE, std::memory_order_release); // بازگشت اتوماتیک به لوله دتکتور کلمه کلیدی
                        TT_UpdateBackgroundMicAll();
                    }
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

static std::unique_ptr<OpusEncFile> g_assistantEncoder = nullptr;
static std::shared_ptr<AudioResampler> g_assistantResampler = nullptr;
static std::vector<short> g_assistantFifo;
static std::atomic<bool> g_assistantActive{false};
static std::string g_assistantFile;
static int g_assistantRemainingSamples = 0;
static bool g_assistantSendToTT = true;

static std::unique_ptr<OpusEncFile> g_messageEncoder = nullptr;
static std::shared_ptr<AudioResampler> g_messageResampler = nullptr;
static std::vector<short> g_messageFifo;
static std::atomic<bool> g_messageActive{false};
static std::string g_messageFile;
static bool g_messageSendToTT = true;

static void NotifyVoiceRecordingFinished(const std::string& filepath, int type) {
    if (!g_jvm || !g_java_listener_ref || !g_recording_finished_method_id) return;

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
        jstring jpath = env->NewStringUTF(filepath.c_str());
        env->CallVoidMethod(g_java_listener_ref, g_recording_finished_method_id, jpath, type);
        env->DeleteLocalRef(jpath);
    }

    if (is_attached) {
        g_jvm->DetachCurrentThread();
    }
}

static void StopAssistant() {
    bool was_active = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_assistantActive.load(std::memory_order_relaxed)) {
            g_assistantActive.store(false, std::memory_order_release);
            if (g_assistantEncoder) {
                if (!g_assistantFifo.empty()) {
                    g_assistantFifo.resize(960, 0);
                    g_assistantEncoder->Encode(g_assistantFifo.data(), 960, true);
                }
                g_assistantEncoder->Close();
                g_assistantEncoder.reset();
            }
            g_assistantResampler.reset();
            g_assistantFifo.clear();
            g_assistantSendToTT = true;
            was_active = true;
        }
    }

    if (was_active) {
        NotifyVoiceRecordingFinished(g_assistantFile, 1);
        TT_UpdateBackgroundMicAll();
    }
}

VoiceFeaturesManager& VoiceFeaturesManager::Instance() {
    static VoiceFeaturesManager instance;
    return instance;
}

VoiceFeaturesManager::VoiceFeaturesManager() {}

bool VoiceFeaturesManager::StartVoiceAssistant(const std::string& outputFile, int durationSeconds, bool sendToTeamTalk) {
    bool success = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (g_assistantActive.load(std::memory_order_relaxed)) {
            g_assistantActive.store(false, std::memory_order_release);
            if (g_assistantEncoder) {
                if (!g_assistantFifo.empty()) {
                    g_assistantFifo.resize(960, 0);
                    g_assistantEncoder->Encode(g_assistantFifo.data(), 960, true);
                }
                g_assistantEncoder->Close();
                g_assistantEncoder.reset();
            }
            g_assistantResampler.reset();
            g_assistantFifo.clear();
            g_assistantSendToTT = true;
            NotifyVoiceRecordingFinished(g_assistantFile, 1);
        }

        g_assistantEncoder = std::make_unique<OpusEncFile>();
        
        #if defined(UNICODE)
            ACE_TString t_filename = Utf8ToUnicode(outputFile.c_str()).c_str();
        #else
            ACE_TString t_filename = outputFile.c_str();
        #endif

        int target_samplerate = 48000;
        int framesize = 960;
        int target_bitrate = 48000;

        if (!g_assistantEncoder->Open(t_filename, 1, target_samplerate, framesize, OPUS_APPLICATION_VOIP)) {
            g_assistantEncoder.reset();
            return false;
        }
        g_assistantEncoder->GetEncoder().SetBitrate(target_bitrate);

        g_assistantFile = outputFile;
        g_assistantRemainingSamples = durationSeconds * target_samplerate;
        g_assistantSendToTT = sendToTeamTalk;
        g_assistantFifo.clear();
        g_assistantActive.store(true, std::memory_order_release);
        success = true;
    }

    if (success) {
        TT_UpdateBackgroundMicAll();
    }
    return true;
}

bool VoiceFeaturesManager::StartVoiceMessage(const std::string& outputFile, bool sendToTeamTalk) {
    bool success = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (g_messageActive.load(std::memory_order_relaxed)) {
            g_messageActive.store(false, std::memory_order_release);
            if (g_messageEncoder) {
                if (!g_messageFifo.empty()) {
                    g_messageFifo.resize(320, 0);
                    g_messageEncoder->Encode(g_messageFifo.data(), 320, true);
                }
                g_messageEncoder->Close();
                g_messageEncoder.reset();
            }
            g_messageResampler.reset();
            g_messageFifo.clear();
            g_messageSendToTT = true;
            NotifyVoiceRecordingFinished(g_messageFile, 2);
        }

        g_messageEncoder = std::make_unique<OpusEncFile>();
        
        #if defined(UNICODE)
            ACE_TString t_filename = Utf8ToUnicode(outputFile.c_str()).c_str();
        #else
            ACE_TString t_filename = outputFile.c_str();
        #endif

        int target_samplerate = 16000;
        int framesize = 320;
        int target_bitrate = 16000;

        if (!g_messageEncoder->Open(t_filename, 1, target_samplerate, framesize, OPUS_APPLICATION_VOIP)) {
            g_messageEncoder.reset();
            return false;
        }
        g_messageEncoder->GetEncoder().SetBitrate(target_bitrate);

        g_messageFile = outputFile;
        g_messageSendToTT = sendToTeamTalk;
        g_messageFifo.clear();
        g_messageActive.store(true, std::memory_order_release);
        success = true;
    }

    if (success) {
        TT_UpdateBackgroundMicAll();
    }
    return true;
}

bool VoiceFeaturesManager::StopVoiceMessage() {
    bool success = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (g_messageActive.load(std::memory_order_relaxed)) {
            g_messageActive.store(false, std::memory_order_release);
            if (g_messageEncoder) {
                if (!g_messageFifo.empty()) {
                    g_messageFifo.resize(320, 0);
                    g_messageEncoder->Encode(g_messageFifo.data(), 320, true);
                }
                g_messageEncoder->Close();
                g_messageEncoder.reset();
            }
            g_messageResampler.reset();
            g_messageFifo.clear();
            g_messageSendToTT = true;
            success = true;
        }
    }

    if (success) {
        NotifyVoiceRecordingFinished(g_messageFile, 2);
        TT_UpdateBackgroundMicAll();
        return true;
    }
    return false;
}

void VoiceFeaturesManager::FeedAudio(const media::AudioFrame& frame) {
    // گارد سبک اتمیک جهت جلوگیری از قفل شدن نخ پردازش صوتی میکروفون
    if (!g_assistantActive.load(std::memory_order_relaxed) && !g_messageActive.load(std::memory_order_relaxed)) return;

    bool assistant_finished = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (g_assistantActive.load(std::memory_order_relaxed) && g_assistantEncoder) {
            int samples_to_encode = frame.input_samples;
            const short* pcm_ptr = frame.input_buffer;
            std::vector<short> resampled_buf;

            media::AudioFormat target_fmt(48000, 1);
            if (frame.inputfmt != target_fmt) {
                if (!g_assistantResampler || g_assistantResampler->GetInputFormat() != frame.inputfmt) {
                    g_assistantResampler = MakeAudioResampler(frame.inputfmt, target_fmt);
                }
                if (g_assistantResampler) {
                    int out_samples = CalcSamples(frame.inputfmt.samplerate, frame.input_samples, 48000);
                    resampled_buf.resize(out_samples);
                    int res = g_assistantResampler->Resample(frame.input_buffer, frame.input_samples, resampled_buf.data(), out_samples);
                    if (res > 0) {
                        pcm_ptr = resampled_buf.data();
                        samples_to_encode = res;
                    }
                }
            }

            g_assistantFifo.insert(g_assistantFifo.end(), pcm_ptr, pcm_ptr + samples_to_encode);
            while (g_assistantFifo.size() >= 960) {
                g_assistantRemainingSamples -= 960;
                bool last = (g_assistantRemainingSamples <= 0);
                g_assistantEncoder->Encode(g_assistantFifo.data(), 960, last);
                g_assistantFifo.erase(g_assistantFifo.begin(), g_assistantFifo.begin() + 960);
                
                if (last) {
                    g_assistantActive.store(false, std::memory_order_release);
                    if (g_assistantEncoder) {
                        g_assistantEncoder->Close();
                        g_assistantEncoder.reset();
                    }
                    g_assistantResampler.reset();
                    g_assistantFifo.clear();
                    g_assistantSendToTT = true;
                    assistant_finished = true;
                    break;
                }
            }
        }

        if (g_messageActive.load(std::memory_order_relaxed) && g_messageEncoder) {
            int samples_to_encode = frame.input_samples;
            const short* pcm_ptr = frame.input_buffer;
            std::vector<short> resampled_buf;

            media::AudioFormat target_fmt(16000, 1);
            if (frame.inputfmt != target_fmt) {
                if (!g_messageResampler || g_messageResampler->GetInputFormat() != frame.inputfmt) {
                    g_messageResampler = MakeAudioResampler(frame.inputfmt, target_fmt);
                }
                if (g_messageResampler) {
                    int out_samples = CalcSamples(frame.inputfmt.samplerate, frame.input_samples, 16000);
                    resampled_buf.resize(out_samples);
                    int res = g_messageResampler->Resample(frame.input_buffer, frame.input_samples, resampled_buf.data(), out_samples);
                    if (res > 0) {
                        pcm_ptr = resampled_buf.data();
                        samples_to_encode = res;
                    }
                }
            }

            g_messageFifo.insert(g_messageFifo.end(), pcm_ptr, pcm_ptr + samples_to_encode);
            while (g_messageFifo.size() >= 320) {
                g_messageEncoder->Encode(g_messageFifo.data(), 320, false);
                g_messageFifo.erase(g_messageFifo.begin(), g_messageFifo.begin() + 320);
            }
        }
    }

    if (assistant_finished) {
        NotifyVoiceRecordingFinished(g_assistantFile, 1);
        TT_UpdateBackgroundMicAll();
    }
}

bool VoiceFeaturesManager::ShouldSendToTeamTalk() {
    return g_assistantSendToTT && g_messageSendToTT;
}

bool IsBackgroundMicRequired() {
    return g_active.load(std::memory_order_relaxed) || 
           g_assistantActive.load(std::memory_order_relaxed) || 
           g_messageActive.load(std::memory_order_relaxed) || 
           (g_state.load(std::memory_order_relaxed) == STATE_ENROLLMENT_ACTIVE);
}
}