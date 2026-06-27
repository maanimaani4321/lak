#include "KwsManager.h"
#include "codec/OggFileIO.h"
#include "codec/MediaUtil.h"
#include "avstream/AudioResampler.h"
#include "sherpa-onnx/c-api/c-api.h"
#include "myace/MyINet.h"
#include "teamtalk/client/ClientNode.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <atomic>
#include <sstream>
#include <fstream>
#include <thread>

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

// پیاده‌سازی متغیرهای سراسری اشتراک‌گذاری شده با JNI (بدون کلمه کلیدی static)
JavaVM* g_jvm = nullptr;
jobject g_java_listener_ref = nullptr;
jmethodID g_callback_method_id = nullptr;
jmethodID g_enroll_callback_method_id = nullptr;
jmethodID g_recording_finished_method_id = nullptr;
jmethodID g_assistant_result_method_id = nullptr;

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

// تبدیل کاملاً ایمن و قابل حمل ACE_TString به std::string بدون در نظر گرفتن فلگ یونیکد
static std::string GetUtf8String(const ACE_TString& str) {
#if defined(UNICODE)
    return UnicodeToUtf8(str).c_str();
#else
    return str.c_str();
#endif
}

// متد فوق‌العاده امن و استاندارد جهت اسکیپ کردن کاراکترهای خاص در قالب JSON
static std::string EscapeJsonString(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size() * 1.1);
    for (char c : input) {
        switch (c) {
            case '\"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c >= 0 && c < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    escaped += buf;
                } else {
                    escaped += c;
                }
                break;
        }
    }
    return escaped;
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

            g_assistant_result_method_id = env->GetMethodID(clazz, "onVoiceAssistantResult", "(Ljava/lang/String;Z)V");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                g_assistant_result_method_id = nullptr;
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

    if (success) {
        TT_UpdateBackgroundMicAll();
    }
    return success;
}

bool KwsStartSpeakerEnrollment(JNIEnv* env) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    
    if (!g_active.load(std::memory_order_relaxed) || !g_enroll_callback_method_id) {
        return false;
    }

    g_enrollment_speech_buffer.clear();
    g_state.store(STATE_ENROLLMENT_ACTIVE, std::memory_order_release);
    TT_UpdateBackgroundMicAll();
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
        g_assistant_result_method_id = nullptr;
        was_active = true;
    }

    if (was_active) {
        TT_UpdateBackgroundMicAll();
    }
}

void KwsProcessAudio(const short* buffer, int samples, int channels, int samplerate) {
    if (!g_active.load(std::memory_order_relaxed)) return;

    try {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_active.load() || !g_stream || !g_spotter || !g_vad) return;

        g_mono_buffer.resize(samples);
        for (int i = 0; i < samples; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c) {
                sum += static_cast<float>(buffer[i * channels + c]);
            }
            g_mono_buffer[i] = sum / (channels * 32768.0f);
        }

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
            g_vad_buffer.insert(g_vad_buffer.end(), resampled_output->samples, resampled_output->samples + resampled_output->n);

            while (g_vad_buffer.size() >= 512) {
                SherpaOnnxVoiceActivityDetectorAcceptWaveform(g_vad, g_vad_buffer.data(), 512);
                g_vad_buffer.erase(g_vad_buffer.begin(), g_vad_buffer.begin() + 512);
            }

            SherpaOnnxVoiceActivityDetectorClear(g_vad);

            bool speech_detected_now = (SherpaOnnxVoiceActivityDetectorDetected(g_vad) == 1);

            int current_state = g_state.load(std::memory_order_relaxed);
            if (current_state == STATE_KWS_ACTIVE) {
                g_speaker_audio_buffer.insert(g_speaker_audio_buffer.end(), resampled_output->samples, resampled_output->samples + resampled_output->n);
                if (g_speaker_audio_buffer.size() > 48000) {
                    g_speaker_audio_buffer.erase(g_speaker_audio_buffer.begin(), g_speaker_audio_buffer.end() - 48000);
                }

                const int max_silence_samples = 3.5 * 16000; 

                if (speech_detected_now) {
                    if (g_silence_samples_counter >= max_silence_samples && !g_pre_roll_buffer.empty()) {
                        SherpaOnnxOnlineStreamAcceptWaveform(g_stream, 16000, g_pre_roll_buffer.data(), g_pre_roll_buffer.size());
                        g_pre_roll_buffer.clear();
                    }
                    g_silence_samples_counter = 0; 
                    g_kws_pipe_reset_done = false;
                } else {
                    g_silence_samples_counter += resampled_output->n;
                }

                if (g_silence_samples_counter < max_silence_samples) {
                    SherpaOnnxOnlineStreamAcceptWaveform(g_stream, 16000, resampled_output->samples, resampled_output->n);

                    while (SherpaOnnxIsKeywordStreamReady(g_spotter, g_stream) == 1) {
                        SherpaOnnxDecodeKeywordStream(g_spotter, g_stream);
                    }

                    const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResult(g_spotter, g_stream);
                    if (result) {
                        if (result->keyword && std::strlen(result->keyword) > 0) {
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
                    if (!g_kws_pipe_reset_done) {
                        SherpaOnnxResetKeywordStream(g_spotter, g_stream); 
                        g_kws_pipe_reset_done = true;
                        g_pre_roll_buffer.clear();
                    }

                    g_pre_roll_buffer.insert(g_pre_roll_buffer.end(), resampled_output->samples, resampled_output->samples + resampled_output->n);
                    if (g_pre_roll_buffer.size() > 16000) {
                        g_pre_roll_buffer.erase(g_pre_roll_buffer.begin(), g_pre_roll_buffer.end() - 16000);
                    }
                }
            }
            else if (current_state == STATE_ENROLLMENT_ACTIVE) {
                if (speech_detected_now) {
                    g_enrollment_speech_buffer.insert(g_enrollment_speech_buffer.end(), resampled_output->samples, resampled_output->samples + resampled_output->n);
                    if (g_enrollment_speech_buffer.size() >= 48000) {
                        ExtractAndNotifyEnrollmentEmbedding(g_enrollment_speech_buffer);
                        g_enrollment_speech_buffer.clear();
                        g_state.store(STATE_KWS_ACTIVE, std::memory_order_release);
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

// متغیرها و متدهای اختصاصی سیستم ارسال بومی دستیار صوتی کلاینت
static std::string g_licenseKey;
static std::string g_androidId;
static std::string g_groqToken;
static std::string g_preferredLanguage;
static std::string g_location;
static int g_serversCount = 0;
static std::string g_userServJson;
static void* g_clientnode = nullptr;

static void CollectChannelsNative(const clientchannel_t& chan, std::ostringstream& oss, bool& first) {
    if (!chan) return;
    if (!first) oss << ",";
    first = false;
    oss << "\"" << chan->GetChannelID() << "\":{";
    
    std::string chan_name = "";
    if (chan->GetName().length() > 0) {
        // رفع خطای کامپایل اندروید با کمک متد ایمن GetUtf8String به جای کدهای قدیمی
        chan_name = EscapeJsonString(GetUtf8String(chan->GetName()));
    }
    
    oss << "\"n\":\"" << chan_name << "\",";
    oss << "\"p\":" << (chan->GetParentChannel() ? chan->GetParentChannel()->GetChannelID() : 0);
    oss << "}";
    for (const auto& sub : chan->GetSubChannels()) {
        CollectChannelsNative(sub, oss, first);
    }
}

static void CollectUsersNative(const clientchannel_t& root, std::ostringstream& oss, bool& first) {
    if (!root) return;
    ClientChannel::users_t users;
    root->GetUsers(users, true);
    for (const auto& u : users) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << u->GetUserID() << "\":{";
        
        std::string nick = "";
        if (u->GetNickname().length() > 0) {
            // رفع خطای کامپایل اندروید با کمک متد ایمن GetUtf8String به جای کدهای قدیمی
            nick = EscapeJsonString(GetUtf8String(u->GetNickname()));
        }
        
        std::string username = "";
        if (u->GetUsername().length() > 0) {
            // رفع خطای کامپایل اندروید با کمک متد ایمن GetUtf8String به جای کدهای قدیمی
            username = EscapeJsonString(GetUtf8String(u->GetUsername()));
        }

        oss << "\"n\":\"" << nick << "\",";
        oss << "\"u\":\"" << username << "\",";
        oss << "\"chan\":" << (u->GetChannel() ? u->GetChannel()->GetChannelID() : 0);
        oss << "}";
    }
}

static std::string BuildAssistantContextNative(void* clientnode_ptr, int serversCount, const std::string& userServJson) {
    if (clientnode_ptr == nullptr) return "{}";
    auto* clientnode = static_cast<ClientNode*>(clientnode_ptr);
    
    std::ostringstream oss;
    oss << "{";
    oss << "\"servers\":" << serversCount << ",";
    oss << "\"myid\":" << clientnode->GetUserID() << ",";
    
    if (userServJson.empty()) {
        oss << "\"userserv\":{},";
    } else {
        oss << "\"userserv\":" << userServJson << ",";
    }
    
    oss << "\"chans\":{";
    bool first_chan = true;
    CollectChannelsNative(clientnode->GetRootChannel(), oss, first_chan);
    oss << "},";
    
    oss << "\"users\":{";
    bool first_user = true;
    CollectUsersNative(clientnode->GetRootChannel(), oss, first_user);
    oss << "}";
    
    oss << "}";
    return oss.str();
}

static void NotifyVoiceAssistantResult(const std::string& resultJson, bool success) {
    if (!g_jvm || !g_java_listener_ref || !g_assistant_result_method_id) return;

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
        jstring j_res = env->NewStringUTF(resultJson.c_str());
        env->CallVoidMethod(g_java_listener_ref, g_assistant_result_method_id, j_res, success ? JNI_TRUE : JNI_FALSE);
        env->DeleteLocalRef(j_res);
    }

    if (is_attached) {
        g_jvm->DetachCurrentThread();
    }
}

static void AsyncSendVoiceAssistantRequest(
    void* clientnode_ptr,
    const std::string& licenseKey,
    const std::string& androidId,
    const std::string& groqToken,
    const std::string& preferredLanguage,
    const std::string& location,
    int serversCount,
    const std::string& userServJson,
    const std::string& audioFilePath
) {
    std::string url = (location == "IR") ? "https://api.djfa.ir/voice/index.php" : "https://api.rnvda.ir/voice/index.php";
    
    std::string contextJson = BuildAssistantContextNative(clientnode_ptr, serversCount, userServJson);
    
    std::string boundary = "===Boundary" + std::to_string(GETTIMESTAMP()) + "====";
    std::ostringstream body;
    
    auto write_form_field = [&](const std::string& name, const std::string& value) {
        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"" << name << "\"\r\n";
        body << "Content-Type: text/plain; charset=UTF-8\r\n\r\n";
        body << value << "\r\n";
    };

    write_form_field("license_key", licenseKey);
    write_form_field("android_id", androidId);
    write_form_field("language", preferredLanguage);
    write_form_field("groq_token", groqToken);
    write_form_field("context", contextJson);

    std::ifstream file(audioFilePath, std::ios::binary);
    if (file.is_open()) {
        std::vector<char> file_data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.ogg\"\r\n";
        body << "Content-Type: audio/ogg\r\n\r\n";
        body.write(file_data.data(), file_data.size());
        body << "\r\n";
    }
    body << "--" << boundary << "--\r\n";

    std::string post_data = body.str();
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;

    std::string response;
    ACE::HTTP::Status::Code statusCode = ACE::HTTP::Status::INVALID;

    int ret = HttpPostRequest(url.c_str(), post_data.data(), post_data.size(), headers, response, &statusCode);
    
    bool request_success = (ret == 1 && statusCode == ACE::HTTP::Status::HTTP_OK);
    
    if (request_success) {
        if (response.find("\"status\":\"error\"") != std::string::npos) {
            if (response.find("LICENSE") != std::string::npos || 
                response.find("DEVICE_BANNED") != std::string::npos ||
                response.find("DEVICE_REMOVED") != std::string::npos ||
                response.find("NOT_FOUND") != std::string::npos) {
                
                if (clientnode_ptr != nullptr) {
                    auto* clientnode = static_cast<ClientNode*>(clientnode_ptr);
                    clientnode->Disconnect();
                    MYTRACE(ACE_TEXT("Voice Assistant: Native auto-disconnect triggered due to License Error.\n"));
                }
            }
        }
        NotifyVoiceAssistantResult(response, true);
    } else {
        NotifyVoiceAssistantResult("{\"status\":\"error\",\"message\":\"Network connection failed.\"}", false);
    }

    if (FileExists(audioFilePath)) {
        std::remove(audioFilePath.c_str());
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
        NotifyVoiceRecordingFinished("", 1);
        TT_UpdateBackgroundMicAll();

        std::thread net_thread(
            AsyncSendVoiceAssistantRequest,
            g_clientnode,
            g_licenseKey,
            g_androidId,
            g_groqToken,
            g_preferredLanguage,
            g_location,
            g_serversCount,
            g_userServJson,
            g_assistantFile
        );
        net_thread.detach();
    }
}

VoiceFeaturesManager& VoiceFeaturesManager::Instance() {
    static VoiceFeaturesManager instance;
    return instance;
}

VoiceFeaturesManager::VoiceFeaturesManager() {}

bool VoiceFeaturesManager::StartVoiceAssistant(
    void* clientnode_ptr,
    const std::string& temp_file,
    const std::string& licenseKey,
    const std::string& androidId,
    const std::string& groqToken,
    const std::string& preferredLanguage,
    const std::string& location,
    int serversCount,
    const std::string& userServJson,
    int durationSeconds, 
    bool sendToTeamTalk) 
{
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
            if (FileExists(g_assistantFile)) {
                std::remove(g_assistantFile.c_str());
            }
        }

        g_assistantEncoder = std::make_unique<OpusEncFile>();
        
        #if defined(UNICODE)
            ACE_TString t_filename = Utf8ToUnicode(temp_file.c_str()).c_str();
        #else
            ACE_TString t_filename = temp_file.c_str();
        #endif

        int target_samplerate = 48000;
        int framesize = 960;
        int target_bitrate = 48000;

        if (!g_assistantEncoder->Open(t_filename, 1, target_samplerate, framesize, OPUS_APPLICATION_VOIP)) {
            g_assistantEncoder.reset();
            return false;
        }
        g_assistantEncoder->GetEncoder().SetBitrate(target_bitrate);

        g_assistantFile = temp_file;
        g_assistantRemainingSamples = durationSeconds * target_samplerate;
        g_assistantSendToTT = sendToTeamTalk;
        
        g_clientnode = clientnode_ptr;
        g_licenseKey = licenseKey;
        g_androidId = androidId;
        g_groqToken = groqToken;
        g_preferredLanguage = preferredLanguage;
        g_location = location;
        g_serversCount = serversCount;
        g_userServJson = userServJson;

        g_assistantFifo.clear();
        g_assistantActive.store(true, std::memory_order_release);
        success = true;
    }

    if (success) {
        TT_UpdateBackgroundMicAll();
    }
    return true;
}

void VoiceFeaturesManager::StopVoiceAssistant() {
    StopAssistant();
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
        NotifyVoiceRecordingFinished("", 1);
        
        std::thread net_thread(
            AsyncSendVoiceAssistantRequest,
            g_clientnode,
            g_licenseKey,
            g_androidId,
            g_groqToken,
            g_preferredLanguage,
            g_location,
            g_serversCount,
            g_userServJson,
            g_assistantFile
        );
        net_thread.detach();
        
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