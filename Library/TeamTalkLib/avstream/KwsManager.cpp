#include "KwsManager.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>

namespace teamtalk {

struct KwsInternalState {
    std::recursive_mutex mutex;
    
    // استفاده صریح از هدر پایدار C-API
    const SherpaOnnxVoiceActivityDetector* vad = nullptr;
    const SherpaOnnxKeywordSpotter* spotter = nullptr;
    const SherpaOnnxOnlineStream* stream = nullptr;
    const SherpaOnnxLinearResampler* resampler = nullptr;
    
    JavaVM* jvm = nullptr;
    jobject java_listener_ref = nullptr;
    jmethodID callback_method_id = nullptr;
    bool active = false;
    int current_samplerate = 0;

    std::vector<float> mono_buffer;
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
        // ۱. مقداردهی اولیه VAD به کمک ساختارهای C-API
        SherpaOnnxVadModelConfig vad_config;
        std::memset(&vad_config, 0, sizeof(vad_config));
        vad_config.silero_vad.model = vad_path.c_str();
        vad_config.silero_vad.threshold = 0.35f; 
        vad_config.silero_vad.min_silence_duration = 0.5f;
        vad_config.silero_vad.min_speech_duration = 0.10f; 
        vad_config.silero_vad.window_size = 512;
        vad_config.silero_vad.max_speech_duration = 20.0f;
        vad_config.sample_rate = 16000;
        vad_config.num_threads = 1;
        vad_config.provider = "cpu";

        g_state.vad = SherpaOnnxCreateVoiceActivityDetector(&vad_config, 10.0f);
        if (!g_state.vad) return false;

        // ۲. مقداردهی اولیه KeywordSpotter به کمک ساختارهای C-API
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

        g_state.spotter = SherpaOnnxCreateKeywordSpotter(&config);
        if (!g_state.spotter) {
            SherpaOnnxDestroyVoiceActivityDetector(g_state.vad);
            g_state.vad = nullptr;
            return false;
        }

        // ۳. ساخت لوله صوتی پایدار مستقیم
        g_state.stream = SherpaOnnxCreateKeywordStream(g_state.spotter);
        if (!g_state.stream) {
            SherpaOnnxDestroyKeywordSpotter(g_state.spotter);
            SherpaOnnxDestroyVoiceActivityDetector(g_state.vad);
            g_state.spotter = nullptr;
            g_state.vad = nullptr;
            return false;
        }

        jclass clazz = env->GetObjectClass(jlistener);
        g_state.callback_method_id = env->GetMethodID(clazz, "onKeywordDetected", "(Ljava/lang/String;)V");
        if (!g_state.callback_method_id) {
            SherpaOnnxDestroyOnlineStream(g_state.stream);
            SherpaOnnxDestroyKeywordSpotter(g_state.spotter);
            SherpaOnnxDestroyVoiceActivityDetector(g_state.vad);
            g_state.stream = nullptr;
            g_state.spotter = nullptr;
            g_state.vad = nullptr;
            return false;
        }

        g_state.java_listener_ref = env->NewGlobalRef(jlistener);
        g_state.active = true;
        g_state.current_samplerate = 0;

        return true;
    } catch (...) {
        if (g_state.stream) SherpaOnnxDestroyOnlineStream(g_state.stream);
        if (g_state.spotter) SherpaOnnxDestroyKeywordSpotter(g_state.spotter);
        if (g_state.vad) SherpaOnnxDestroyVoiceActivityDetector(g_state.vad);
        g_state.stream = nullptr;
        g_state.spotter = nullptr;
        g_state.vad = nullptr;
        return false;
    }
}

void KwsStop(JNIEnv* env) {
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    if (!g_state.active) return;

    g_state.active = false;
    
    // مدیریت مستقیم و امن آزادسازی آدرس‌ها
    if (g_state.stream) {
        SherpaOnnxDestroyOnlineStream(g_state.stream);
        g_state.stream = nullptr;
    }   
    if (g_state.spotter) {
        SherpaOnnxDestroyKeywordSpotter(g_state.spotter);
        g_state.spotter = nullptr;
    }  
    if (g_state.vad) {
        SherpaOnnxDestroyVoiceActivityDetector(g_state.vad);
        g_state.vad = nullptr;
    }      
    if (g_state.resampler) {
        SherpaOnnxDestroyLinearResampler(g_state.resampler);
        g_state.resampler = nullptr;
    }
    
    g_state.mono_buffer.clear();

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
    try {
        std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
        if (!g_state.active || !g_state.stream || !g_state.spotter || !g_state.vad) return;

        g_state.mono_buffer.resize(samples);
        for (int i = 0; i < samples; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c) {
                sum += static_cast<float>(buffer[i * channels + c]);
            }
            g_state.mono_buffer[i] = sum / (channels * 32768.0f);
        }

        // بررسی و ساخت نمونه ریسمپلر به صورت نیتیو
        if (!g_state.resampler || g_state.current_samplerate != samplerate) {
            if (g_state.resampler) {
                SherpaOnnxDestroyLinearResampler(g_state.resampler);
            }
            g_state.resampler = SherpaOnnxCreateLinearResampler(samplerate, 16000, 0, 6);
            g_state.current_samplerate = samplerate;
        }

        // استفاده صحیح از ساختار خروجی SherpaOnnxResampleOut
        const SherpaOnnxResampleOut* resampled_output = SherpaOnnxLinearResamplerResample(
            g_state.resampler,
            g_state.mono_buffer.data(),
            g_state.mono_buffer.size(),
            0
        );

        if (resampled_output && resampled_output->samples && resampled_output->n > 0) {
            SherpaOnnxVoiceActivityDetectorAcceptWaveform(g_state.vad, resampled_output->samples, resampled_output->n);

            while (SherpaOnnxVoiceActivityDetectorEmpty(g_state.vad) == 0) {
                const SherpaOnnxSpeechSegment* segment = SherpaOnnxVoiceActivityDetectorFront(g_state.vad);
                SherpaOnnxVoiceActivityDetectorPop(g_state.vad);

                if (segment && segment->samples && segment->n > 0) {
                    // تزریق پیوسته فریم‌ها به لوله صوتی پایدار C-API بدون باگ‌های تخریب کلاس‌های واسطه
                    SherpaOnnxOnlineStreamAcceptWaveform(g_state.stream, 16000, segment->samples, segment->n);

                    while (SherpaOnnxIsKeywordStreamReady(g_state.spotter, g_state.stream) == 1) {
                        SherpaOnnxDecodeKeywordStream(g_state.spotter, g_state.stream);

                        const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResult(g_state.spotter, g_state.stream);
                        if (result) {
                            if (result->keyword && std::strlen(result->keyword) > 0) {
                                NotifyJavaListener(result->keyword);
                                SherpaOnnxResetKeywordStream(g_state.spotter, g_state.stream);
                            }
                            SherpaOnnxDestroyKeywordResult(result);
                        }
                    }
                }
                
                if (segment) {
                    SherpaOnnxDestroySpeechSegment(segment);
                }
            }
        }
        
        if (resampled_output) {
            SherpaOnnxLinearResamplerResampleFree(resampled_output);
        }
    } catch (const std::exception& e) {
        if (g_state.spotter && g_state.stream) {
            SherpaOnnxResetKeywordStream(g_state.spotter, g_state.stream);
        }
    } catch (...) {
        // مدیریت کرش غیرمنتظره صوتی
    }
}

} // namespace teamtalk