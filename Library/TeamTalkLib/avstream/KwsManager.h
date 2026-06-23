#ifndef KWSMANAGER_H
#define KWSMANAGER_H

#include <jni.h>
#include <string>

namespace teamtalk {

// مقداردهی اولیه جاوا اس‌دی‌کی
void KwsInit(JavaVM* vm);

// متد شروع گوش دادن به کلمات کلیدی
bool KwsStart(JNIEnv* env, jobject jlistener,
              const std::string& encoder_path,
              const std::string& decoder_path,
              const std::string& joiner_path,
              const std::string& tokens_path,
              const std::string& bpe_path,
              const std::string& keywords_path);

// متد توقف کامل موتور
void KwsStop(JNIEnv* env);

// پردازش داده‌های صوتی خام دریافتی از اوبو (میکروفون)
void KwsProcessAudio(const short* buffer, int samples, int channels, int samplerate);

} // namespace teamtalk

#endif