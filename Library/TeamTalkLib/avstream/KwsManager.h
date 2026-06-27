#ifndef KWSMANAGER_H
#define KWSMANAGER_H

#include <jni.h>
#include <string>
#include <vector>
#include <mutex>

namespace media {
    struct AudioFrame;
}

namespace teamtalk {

void KwsInit(JavaVM* vm);

bool KwsStart(JNIEnv* env, jobject jlistener,
              const std::string& encoder_path,
              const std::string& decoder_path,
              const std::string& joiner_path,
              const std::string& tokens_path,
              const std::string& bpe_path,
              const std::string& keywords_path,
              const std::string& vad_path);

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
                const std::vector<float>& target_speaker_embedding);

bool KwsStartSpeakerEnrollment(JNIEnv* env);

void KwsStop(JNIEnv* env);

void KwsProcessAudio(const short* buffer, int samples, int channels, int samplerate);

class VoiceFeaturesManager {
public:
    static VoiceFeaturesManager& Instance();

    bool StartVoiceAssistant(void* clientnode_ptr,
                             const std::string& licenseKey,
                             const std::string& androidId,
                             const std::string& groqToken,
                             const std::string& preferredLanguage,
                             const std::string& location,
                             int serversCount,
                             const std::string& userServJson,
                             int durationSeconds, 
                             bool sendToTeamTalk);
    bool StartVoiceMessage(const std::string& outputFile, bool sendToTeamTalk);
    bool StopVoiceMessage();
    void StopVoiceAssistant();

    void FeedAudio(const media::AudioFrame& frame);
    bool ShouldSendToTeamTalk();

private:
    VoiceFeaturesManager();
    ~VoiceFeaturesManager() = default;

    std::recursive_mutex m_mutex;
};

bool IsBackgroundMicRequired();
} // namespace teamtalk

#endif