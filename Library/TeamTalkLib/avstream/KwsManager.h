#ifndef KWSMANAGER_H
#define KWSMANAGER_H

#include <jni.h>
#include <string>

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

void KwsStop(JNIEnv* env);

void KwsProcessAudio(const short* buffer, int samples, int channels, int samplerate);

} // namespace teamtalk

#endif // KWSMANAGER_H