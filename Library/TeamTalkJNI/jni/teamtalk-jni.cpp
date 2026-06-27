#include "ttconvert-jni.h"

#include "avstream/KwsManager.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <map>
#include <mutex>
#include <vector>

static std::mutex ttinstmutex;
static std::map<jint, TTInstance*> ttinstances;
extern "C" void TT_PushInternalAudio(TTInstance* lpTTInstance, const short* lpData, int nSamples, int nGain, TTBOOL bForceTransmitCheck);
extern "C" TTBOOL TT_StartInternalVideoTransmission(TTInstance* lpTTInstance, IN const VideoCodec* lpVideoCodec, int nWidth, int nHeight, int nFPS);
extern "C" TTBOOL TT_StopInternalVideoTransmission(TTInstance* lpTTInstance);
extern "C" void TT_PushInternalVideo(TTInstance* lpTTInstance, const char* lpData, int nDataSize, int nWidth, int nHeight, int nFourCC, TTBOOL bTopDown);
extern "C" TTBOOL TT_SetForceMono(TTInstance* lpTTInstance, TTBOOL bEnable);
extern "C" TTBOOL TT_GetForceMono(TTInstance* lpTTInstance);
extern "C" TTBOOL TT_GetSoundInputFilter(TTInstance* lpTTInstance, TTCHAR szFilter[TT_STRLEN]);
extern "C" TTBOOL TT_SetUserSoundFilter(TTInstance* lpTTInstance, int nUserID, const TTCHAR* lpFilter);

extern jmethodID g_assistant_result_method_id;
extern jobject g_java_listener_ref;

static void AddTTInstance(JNIEnv* env, jobject thiz, TTInstance* ttinst)
{
    auto hash = hashCode(env, thiz);

    std::lock_guard<std::mutex> const g(ttinstmutex);
    ttinstances[hash] = ttinst;
}

static TTInstance* RemoveTTInstance(JNIEnv* env, jobject thiz)
{
    auto hash = hashCode(env, thiz);

    std::lock_guard<std::mutex> const g(ttinstmutex);
    TTInstance* ttinst = ttinstances[hash];
    ttinstances.erase(hash);
    return ttinst;
}

static TTInstance* GetTTInstance(JNIEnv* env, jobject thiz)
{
    auto hash = hashCode(env, thiz);
    std::lock_guard<std::mutex> const g(ttinstmutex);
    return ttinstances[hash];
}

extern "C" {

    JNIEXPORT jstring JNICALL Java_dk_bearware_TeamTalkBase_getVersion(JNIEnv* env,
                                                                       jclass /*unused*/)
    {
        const TTCHAR* ttv = TT_GetVersion();
        return NEW_JSTRING(env, ttv);
    }

    JNIEXPORT jlong JNICALL Java_dk_bearware_TeamTalkBase_initTeamTalkPoll(JNIEnv* env,
                                                                           jobject thiz)
    {
        TTInstance* inst = TT_InitTeamTalkPoll();
        AddTTInstance(env, thiz, inst);
        return reinterpret_cast<jlong>(inst);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_closeTeamTalk(JNIEnv* env,
                                                                           jobject thiz)
    {
        return TT_CloseTeamTalk(RemoveTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getMessage(JNIEnv* env,
                                                                        jobject thiz,
                                                                        jobject pMsg,
                                                                        jint pnWaitMs)
    {
        THROW_NULLEX(env, pMsg, false);

        INT32 const _pnWaitMs = pnWaitMs;
        TTMessage msg;
        TTBOOL const b = TT_GetMessage(GetTTInstance(env, thiz),
                                 &msg, &_pnWaitMs);
        if(b != 0)
            setTTMessage(env, msg, pMsg);
        return b;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_pumpMessage(JNIEnv* env,
                                                                         jobject thiz,
                                                                         jint nClientEvent,
                                                                         jint nIdentifier)
    {
        return TT_PumpMessage(GetTTInstance(env, thiz),
                              (ClientEvent)nClientEvent, nIdentifier);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getFlags(JNIEnv* env,
                                                                  jobject thiz)
    {
        return TT_GetFlags(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setLicenseInformation(JNIEnv* env,
                                                                                   jclass /*unused*/,
                                                                                   jstring szRegName,
                                                                                   jstring szRegKey)
    {
        THROW_NULLEX(env, szRegName, false);
        THROW_NULLEX(env, szRegKey, false);
        return TT_SetLicenseInformation(ttstr(env, szRegName), ttstr(env, szRegKey));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getDefaultSoundDevices(JNIEnv* env,
                                                                                    jclass /*unused*/,
                                                                                    jobject lpnInputDeviceID,
                                                                                    jobject lpnOutputDeviceID)
    {
        THROW_NULLEX(env, lpnInputDeviceID, false);
        THROW_NULLEX(env, lpnOutputDeviceID, false);

        int inputid;
        int outputid;
        if(TT_GetDefaultSoundDevices(&inputid, &outputid) != 0)
        {
            setIntPtr(env, lpnInputDeviceID, inputid);
            setIntPtr(env, lpnOutputDeviceID, outputid);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getDefaultSoundDevicesEx(JNIEnv* env,
                                                                                      jclass /*unused*/,
                                                                                      jint nSndSystem,
                                                                                      jobject lpnInputDeviceID,
                                                                                      jobject lpnOutputDeviceID)
    {
        THROW_NULLEX(env, lpnInputDeviceID, false);
        THROW_NULLEX(env, lpnOutputDeviceID, false);

        int inputid;
        int outputid;
        if(TT_GetDefaultSoundDevicesEx((SoundSystem)nSndSystem, &inputid, &outputid) != 0)
        {
            setIntPtr(env, lpnInputDeviceID, inputid);
            setIntPtr(env, lpnOutputDeviceID, outputid);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getSoundDevices(JNIEnv* env,
                                                                             jclass /*unused*/,
                                                                             jobjectArray lpSoundDevices,
                                                                             jobject lpnHowMany)
    {
        THROW_NULLEX(env, lpnHowMany, false);

        int howmany = 0;
        if(lpSoundDevices == nullptr)
        {
            if(TT_GetSoundDevices(nullptr, &howmany) == 0)
                return JFALSE;
            setIntPtr(env, lpnHowMany, howmany);
            return JTRUE;
        }

        auto arr_size = (INT32)env->GetArrayLength(lpSoundDevices);
        std::vector<SoundDevice> devs((size_t)arr_size);
        if((arr_size != 0) && (TT_GetSoundDevices(devs.data(), &arr_size) == 0))
            return JFALSE;

        for(jsize i=0;i<arr_size;i++)
            env->SetObjectArrayElement(lpSoundDevices, i, newSoundDevice(env, devs[i]));

        setIntPtr(env, lpnHowMany, arr_size);

        return JTRUE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_restartSoundSystem(JNIEnv* /*unused*/,
                                                                                jclass /*unused*/)
    {
        return TT_RestartSoundSystem();
    }

    JNIEXPORT jlong JNICALL Java_dk_bearware_TeamTalkBase_startSoundLoopbackTest(JNIEnv* env,
                                                                                 jclass /*unused*/,
                                                                                 jint nInputDeviceID,
                                                                                 jint nOutputDeviceID,
                                                                                 jint nSampleRate,
                                                                                 jint nChannels,
                                                                                 jboolean bDuplexMode,
                                                                                 jobject lpSpeexDSP)
    {

        TTSoundLoop* inst = nullptr;
        if(lpSpeexDSP != nullptr)
        {
            SpeexDSP spxdsp{};
            setSpeexDSP(env, spxdsp, lpSpeexDSP, J2N);

            inst = TT_StartSoundLoopbackTest(nInputDeviceID, nOutputDeviceID,
                                             nSampleRate, nChannels, bDuplexMode, &spxdsp);
        }
        else
        {
            inst = TT_StartSoundLoopbackTest(nInputDeviceID, nOutputDeviceID,
                                             nSampleRate, nChannels, bDuplexMode, nullptr);
        }
        return reinterpret_cast<jlong>(inst);
    }

    JNIEXPORT jlong JNICALL Java_dk_bearware_TeamTalkBase_startSoundLoopbackTestEx(JNIEnv* env,
                                                                                   jclass /*unused*/,
                                                                                   jint nInputDeviceID,
                                                                                   jint nOutputDeviceID,
                                                                                   jint nSampleRate,
                                                                                   jint nChannels,
                                                                                   jboolean bDuplexMode,
                                                                                   jobject lpAudioPreprocessor,
                                                                                   jobject lpSoundDeviceEffects)
    {

        TTSoundLoop* inst = nullptr;
        AudioPreprocessor preprocessor = {};
        SoundDeviceEffects effects = {};

        if (lpAudioPreprocessor != nullptr)
            setAudioPreprocessor(env, preprocessor, lpAudioPreprocessor, J2N);
        if (lpSoundDeviceEffects != nullptr)
            setSoundDeviceEffects(env, effects, lpSoundDeviceEffects, J2N);

        inst = TT_StartSoundLoopbackTestEx(nInputDeviceID, nOutputDeviceID,
                                           nSampleRate, nChannels, bDuplexMode,
                                           ((lpAudioPreprocessor != nullptr)? &preprocessor : nullptr),
                                           ((lpSoundDeviceEffects != nullptr)? &effects : nullptr));
        return reinterpret_cast<jlong>(inst);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_closeSoundLoopbackTest(JNIEnv* /*unused*/,
                                                                                    jclass /*unused*/,
                                                                                    jlong lpTTSoundLoop)
    {
        return TT_CloseSoundLoopbackTest(reinterpret_cast<TTSoundLoop*>(lpTTSoundLoop));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_initSoundInputDevice(JNIEnv* env,
                                                                                  jobject thiz,
                                                                                  jint nInputDeviceID)
    {
        return TT_InitSoundInputDevice(GetTTInstance(env, thiz), nInputDeviceID);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_initSoundInputSharedDevice(JNIEnv* /*unused*/,
                                                                                        jclass /*unused*/,
                                                                                        jint nSampleRate,
                                                                                        jint nChannels,
                                                                                        jint nFrameSize)
    {
        return TT_InitSoundInputSharedDevice(nSampleRate, nChannels, nFrameSize);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_initSoundOutputDevice(JNIEnv* env,
                                                                                   jobject thiz,
                                                                                   jint nOutputDeviceID)
    {
        return TT_InitSoundOutputDevice(GetTTInstance(env, thiz), nOutputDeviceID);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_initSoundOutputSharedDevice(JNIEnv* /*unused*/,
                                                                                        jclass /*unused*/,
                                                                                        jint nSampleRate,
                                                                                        jint nChannels,
                                                                                        jint nFrameSize)
    {
        return TT_InitSoundOutputSharedDevice(nSampleRate, nChannels, nFrameSize);
    }


    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_initSoundDuplexDevices(JNIEnv* env,
                                                                                    jobject thiz,
                                                                                    jint nInputDeviceID,
                                                                                    jint nOutputDeviceID)
    {
        return TT_InitSoundDuplexDevices(GetTTInstance(env, thiz), nInputDeviceID, nOutputDeviceID);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_closeSoundInputDevice(JNIEnv* env,
                                                                                   jobject thiz)
    {
        return TT_CloseSoundInputDevice(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_closeSoundOutputDevice(JNIEnv* env,
                                                                                    jobject thiz)
    {
        return TT_CloseSoundOutputDevice(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_closeSoundDuplexDevices(JNIEnv* env,
                                                                                     jobject thiz)
    {
        return TT_CloseSoundDuplexDevices(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setSoundDeviceEffects(JNIEnv* env,
                                                                                   jobject thiz,
                                                                                   jobject lpSoundDeviceEffects)
    {
        SoundDeviceEffects effects = {};
        setSoundDeviceEffects(env, effects, lpSoundDeviceEffects, J2N);

        return TT_SetSoundDeviceEffects(GetTTInstance(env, thiz), &effects);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getSoundDeviceEffects(JNIEnv* env,
                                                                                   jobject thiz,
                                                                                   jobject lpSoundDeviceEffects)
    {
        SoundDeviceEffects effects = {};
        if (TT_GetSoundDeviceEffects(GetTTInstance(env, thiz), &effects) != 0)
        {
            setSoundDeviceEffects(env, effects, lpSoundDeviceEffects, N2J);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getSoundInputLevel(JNIEnv* env,
                                                                            jobject thiz)
    {
        return TT_GetSoundInputLevel(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setSoundInputGainLevel(JNIEnv* env,
                                                                                    jobject thiz,
                                                                                    jint nLevel)
    {
        return TT_SetSoundInputGainLevel(GetTTInstance(env, thiz),
                                         nLevel);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getSoundInputGainLevel(JNIEnv* env,
                                                                                jobject thiz)
    {
        return TT_GetSoundInputGainLevel(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setSoundInputPreprocess(JNIEnv* env,
                                                                                     jobject thiz,
                                                                                     jobject lpSpeexDSP)
    {
        THROW_NULLEX(env, lpSpeexDSP, false);

        SpeexDSP spxdsp = {};
        setSpeexDSP(env, spxdsp, lpSpeexDSP, J2N);
        return TT_SetSoundInputPreprocess(GetTTInstance(env, thiz), &spxdsp);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getSoundInputPreprocess(JNIEnv* env,
                                                                                     jobject thiz,
                                                                                     jobject lpSpeexDSP)
    {
        THROW_NULLEX(env, lpSpeexDSP, false);

        SpeexDSP spxdsp = {};
        if(TT_GetSoundInputPreprocess(GetTTInstance(env, thiz), &spxdsp) != 0)
        {
            setSpeexDSP(env, spxdsp, lpSpeexDSP, N2J);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setSoundInputPreprocessEx(JNIEnv* env,
                                                                                       jobject thiz,
                                                                                       jobject lpAudioPreprocessor)
    {
        THROW_NULLEX(env, lpAudioPreprocessor, false);

        AudioPreprocessor preprocessor = {};
        setAudioPreprocessor(env, preprocessor, lpAudioPreprocessor, J2N);
        return TT_SetSoundInputPreprocessEx(GetTTInstance(env, thiz), &preprocessor);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getSoundInputPreprocessEx(JNIEnv* env,
                                                                                       jobject thiz,
                                                                                       jobject lpAudioPreprocessor)
    {
        THROW_NULLEX(env, lpAudioPreprocessor, false);

        AudioPreprocessor preprocess = {};
        if (TT_GetSoundInputPreprocessEx(GetTTInstance(env, thiz), &preprocess) != 0)
        {
            setAudioPreprocessor(env, preprocess, lpAudioPreprocessor, N2J);
            return JTRUE;
        }
        return JFALSE;
    }


    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setSoundOutputVolume(JNIEnv* env,
                                                                                  jobject thiz,
                                                                                  jint nVolume)
    {
        return TT_SetSoundOutputVolume(GetTTInstance(env, thiz),
                                       nVolume);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getSoundOutputVolume(JNIEnv* env,
                                                                              jobject thiz)
    {
        return TT_GetSoundOutputVolume(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setSoundOutputMute(JNIEnv* env,
                                                                                jobject thiz,
                                                                                jboolean bMuteAll)
    {
        return TT_SetSoundOutputMute(GetTTInstance(env, thiz),
                                     bMuteAll);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_enableVoiceTransmission(JNIEnv* env,
                                                                                     jobject thiz,
                                                                                     jboolean bEnable)
    {
        return TT_EnableVoiceTransmission(GetTTInstance(env, thiz),
                                          bEnable);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_enable3DSoundPositioning(JNIEnv* env,
                                                                                      jobject thiz,
                                                                                      jboolean bEnable)
    {
        return TT_Enable3DSoundPositioning(GetTTInstance(env, thiz),
                                           bEnable);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_autoPositionUsers(JNIEnv* env,
                                                                               jobject thiz)
    {
        return TT_AutoPositionUsers(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_enableAudioBlockEvent(JNIEnv* env,
                                                                                   jobject thiz,
                                                                                   jint nUserID,
                                                                                   jint nStreamType,
                                                                                   jboolean bEnable)
    {
        return TT_EnableAudioBlockEvent(GetTTInstance(env, thiz),
                                        nUserID, (StreamType)nStreamType, bEnable);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_enableAudioBlockEventEx(JNIEnv* env,
                                                                                     jobject thiz,
                                                                                     jint nUserID,
                                                                                     jint nStreamType,
                                                                                     jobject lpAudioFormat,
                                                                                     jboolean bEnable)
    {
        AudioFormat fmt = {};
        if (lpAudioFormat != nullptr)
            setAudioFormat(env, fmt, lpAudioFormat, J2N);

        return TT_EnableAudioBlockEventEx(GetTTInstance(env, thiz),
                                          nUserID, (StreamType)nStreamType, (lpAudioFormat != nullptr)? &fmt : nullptr, bEnable);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_insertAudioBlock(JNIEnv* env,
                                                                              jobject thiz,
                                                                              jobject lpAudioBlock)
    {
        AudioBlock ab = {};
        auto *byteArr = setAudioBlock(env, ab, lpAudioBlock, J2N);
        jboolean const b = TT_InsertAudioBlock(GetTTInstance(env, thiz), &ab);
        if (byteArr != nullptr)
            env->ReleaseByteArrayElements(byteArr, reinterpret_cast<jbyte*>(ab.lpRawAudio), JNI_ABORT);
        return b;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_enableVoiceActivation(JNIEnv* env,
                                                                                   jobject thiz,
                                                                                   jboolean bEnable)
    {
        return TT_EnableVoiceActivation(GetTTInstance(env, thiz),
                                        bEnable);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setForceMono(JNIEnv* env, jobject thiz, jboolean bEnable)
    {
        return TT_SetForceMono(GetTTInstance(env, thiz), bEnable ? JTRUE : JFALSE);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getForceMono(JNIEnv* env, jobject thiz)
    {
        return TT_GetForceMono(GetTTInstance(env, thiz));
    }

    JNIEXPORT jstring JNICALL Java_dk_bearware_TeamTalkBase_getSoundInputFilter(JNIEnv* env, jobject thiz)
    {
        TTCHAR szFilter[TT_STRLEN] = {};
        if (TT_GetSoundInputFilter(GetTTInstance(env, thiz), szFilter) != 0)
        {
            return NEW_JSTRING(env, szFilter);
        }
        return nullptr;
    }
    
    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startKwsEx(
        JNIEnv* env, jobject thiz,
        jstring encoder_path, jstring decoder_path, jstring joiner_path,
        jstring tokens_path, jstring bpe_path, jstring keywords_path,
        jstring vad_path, jstring wespeaker_path, jfloat vad_threshold,
        jboolean speaker_verify_enabled, jfloat speaker_verify_threshold,
        jfloatArray target_speaker_embedding)
    {
        auto to_std_str = [&](jstring jstr) -> std::string {
            if (!jstr) return "";
            const char* chars = env->GetStringUTFChars(jstr, nullptr);
            std::string s(chars);
            env->ReleaseStringUTFChars(jstr, chars);
            return s;
        };

        std::string enc = to_std_str(encoder_path);
        std::string dec = to_std_str(decoder_path);
        std::string join = to_std_str(joiner_path);
        std::string tok = to_std_str(tokens_path);
        std::string bpe = to_std_str(bpe_path);
        std::string kws_file = to_std_str(keywords_path);
        std::string vad = to_std_str(vad_path);
        std::string wespeaker = to_std_str(wespeaker_path);

        std::vector<float> target_emb;
        if (target_speaker_embedding != nullptr) {
            jsize len = env->GetArrayLength(target_speaker_embedding);
            if (len > 0) {
                target_emb.resize(len);
                env->GetFloatArrayRegion(target_speaker_embedding, 0, len, target_emb.data());
            }
        }

        JavaVM* vm = nullptr;
        env->GetJavaVM(&vm);
        teamtalk::KwsInit(vm);

        return teamtalk::KwsStartEx(env, thiz, enc, dec, join, tok, bpe, kws_file, vad,
                                    wespeaker, vad_threshold, speaker_verify_enabled,
                                    speaker_verify_threshold, target_emb) ? JTRUE : JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startSpeakerEnrollment(
        JNIEnv* env, jobject thiz)
    {
        return teamtalk::KwsStartSpeakerEnrollment(env) ? JTRUE : JFALSE;
    }
    
    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startKws(
        JNIEnv* env, jobject thiz,
        jstring encoder_path, jstring decoder_path, jstring joiner_path,
        jstring tokens_path, jstring bpe_path, jstring keywords_path,
        jstring vad_path)
    {
        auto to_std_str = [&](jstring jstr) -> std::string {
            if (!jstr) return "";
            const char* chars = env->GetStringUTFChars(jstr, nullptr);
            std::string s(chars);
            env->ReleaseStringUTFChars(jstr, chars);
            return s;
        };

        std::string enc = to_std_str(encoder_path);
        std::string dec = to_std_str(decoder_path);
        std::string join = to_std_str(joiner_path);
        std::string tok = to_std_str(tokens_path);
        std::string bpe = to_std_str(bpe_path);
        std::string kws_file = to_std_str(keywords_path);
        std::string vad = to_std_str(vad_path);

        JavaVM* vm = nullptr;
        env->GetJavaVM(&vm);
        teamtalk::KwsInit(vm);

        return teamtalk::KwsStart(env, thiz, enc, dec, join, tok, bpe, kws_file, vad) ? JTRUE : JFALSE;
    }

    JNIEXPORT void JNICALL Java_dk_bearware_TeamTalkBase_stopKws(JNIEnv* env, jobject thiz)
    {
        teamtalk::KwsStop(env);
    }
    
        JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startVoiceAssistant(
        JNIEnv* env, jobject thiz, jstring licenseKey, jstring androidId, jstring groqToken,
        jstring preferredLanguage, jstring location, jint serversCount, jstring userServJson,
        jint durationSeconds, jboolean sendToTeamTalk)
    {
        auto to_std_str = [&](jstring jstr) -> std::string {
            if (!jstr) return "";
            const char* chars = env->GetStringUTFChars(jstr, nullptr);
            std::string s(chars);
            env->ReleaseStringUTFChars(jstr, chars);
            return s;
        };

        std::string lic = to_std_str(licenseKey);
        std::string aid = to_std_str(androidId);
        std::string tok = to_std_str(groqToken);
        std::string lang = to_std_str(preferredLanguage);
        std::string loc = to_std_str(location);
        std::string user_serv = to_std_str(userServJson);

        // ثبت اتوماتیک هندل کالبک جاوا به کدهای بومی
        jclass clazz = env->GetObjectClass(thiz);
        g_assistant_result_method_id = env->GetMethodID(clazz, "onVoiceAssistantResult", "(Ljava/lang/String;Z)V");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            g_assistant_result_method_id = nullptr;
        }

        JavaVM* vm = nullptr;
        env->GetJavaVM(&vm);
        teamtalk::KwsInit(vm);

        TTInstance* inst = GetTTInstance(env, thiz);

        return teamtalk::VoiceFeaturesManager::Instance().StartVoiceAssistant(
            inst, lic, aid, tok, lang, loc, (int)serversCount, user_serv, (int)durationSeconds, sendToTeamTalk != 0
        ) ? JTRUE : JFALSE;
    }

    JNIEXPORT void JNICALL Java_dk_bearware_TeamTalkBase_stopVoiceAssistant(JNIEnv* env, jobject thiz)
    {
        teamtalk::VoiceFeaturesManager::Instance().StopVoiceAssistant();
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startVoiceMessage(
        JNIEnv* env, jobject thiz, jstring outputFile, jboolean sendToTeamTalk)
    {
        THROW_NULLEX(env, outputFile, false);
        const char* chars = env->GetStringUTFChars(outputFile, nullptr);
        std::string out_file(chars);
        env->ReleaseStringUTFChars(outputFile, chars);

        return teamtalk::VoiceFeaturesManager::Instance().StartVoiceMessage(out_file, sendToTeamTalk) ? JTRUE : JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_stopVoiceMessage(
        JNIEnv* env, jobject thiz)
    {
        return teamtalk::VoiceFeaturesManager::Instance().StopVoiceMessage() ? JTRUE : JFALSE;
    }
    
    JNIEXPORT void JNICALL Java_dk_bearware_TeamTalkBase_pushInternalAudio(JNIEnv* env,
                                                                           jobject thiz,
                                                                           jshortArray data,
                                                                           jint samples,
                                                                           jint gain,
                                                                           jboolean forceTransmitCheck)
    {
        // بررسی عدم null بودن آرایه ورودی برای جلوگیری از کرش JVM
        THROW_NULLEX(env, data, ); 

        // ۱. گرفتن نمونه کلاینت
        TTInstance* inst = GetTTInstance(env, thiz);
        if (inst == nullptr) return;

        // ۲. بررسی اعتبار طول داده‌ها
        jsize arrayLen = env->GetArrayLength(data);
        if (samples > arrayLen) samples = arrayLen;

        // ۳. دسترسی به داده‌های آرایه بدون کپی کردن
        jshort* body = env->GetShortArrayElements(data, nullptr);
        if (body == nullptr) return;

        // تبدیل jboolean به TTBOOL
        TTBOOL const bForceTransmit = forceTransmitCheck ? JTRUE : JFALSE;

        // ۴. فراخوانی تابع اصلی با تعداد سمیپل‌های مشخص شده
        TT_PushInternalAudio(inst, (const short*)body, (int)samples, (int)gain, bForceTransmit);

        // ۵. آزاد کردن دسترسی به آرایه
        env->ReleaseShortArrayElements(data, body, JNI_ABORT);
    }
    
    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startInternalVideoTransmission(JNIEnv* env,
                                                                                            jobject thiz,
                                                                                            jobject lpVideoCodec,
                                                                                            jint nWidth,
                                                                                            jint nHeight,
                                                                                            jint nFPS)
    {
        THROW_NULLEX(env, lpVideoCodec, false);

        VideoCodec vidcodec = {};
        setVideoCodec(env, vidcodec, lpVideoCodec, J2N);
        return TT_StartInternalVideoTransmission(GetTTInstance(env, thiz), &vidcodec, nWidth, nHeight, nFPS);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_stopInternalVideoTransmission(JNIEnv* env,
                                                                                           jobject thiz)
    {
        return TT_StopInternalVideoTransmission(GetTTInstance(env, thiz));
    }

    JNIEXPORT void JNICALL Java_dk_bearware_TeamTalkBase_pushInternalVideo(JNIEnv* env,
                                                                           jobject thiz,
                                                                           jbyteArray data,
                                                                           jint data_size,
                                                                           jint width,
                                                                           jint height,
                                                                           jint four_cc,
                                                                           jboolean top_down)
    {
        THROW_NULLEX(env, data, );

        TTInstance* inst = GetTTInstance(env, thiz);
        if (inst == nullptr) return;

        jsize arrayLen = env->GetArrayLength(data);
        if (data_size > arrayLen) data_size = arrayLen;

        jbyte* body = env->GetByteArrayElements(data, nullptr);
        if (body == nullptr) return;

        TT_PushInternalVideo(inst, reinterpret_cast<const char*>(body), (int)data_size, (int)width, (int)height, (int)four_cc, top_down ? JTRUE : JFALSE);

        env->ReleaseByteArrayElements(data, body, JNI_ABORT);
    }
    
    JNIEXPORT void JNICALL Java_dk_bearware_TeamTalkBase_pushInternalVideoYUV(
    JNIEnv* env,
    jobject thiz,
    jobject y_buf,
    jobject u_buf,
    jobject v_buf,
    jint y_stride,
    jint u_stride,
    jint v_stride,
    jint u_pix_stride,
    jint v_pix_stride,
    jint width,
    jint height,
    jint rotation) 
{
    THROW_NULLEX(env, y_buf, );
    THROW_NULLEX(env, u_buf, );
    THROW_NULLEX(env, v_buf, );

    // دریافت متد position از کلاس پایه java/nio/Buffer برای به دست آوردن آفست واقعی
    jclass bufferClass = env->FindClass("java/nio/Buffer");
    if (bufferClass == nullptr) return;
    
    jmethodID positionMethod = env->GetMethodID(bufferClass, "position", "()I");
    if (positionMethod == nullptr) return;

    jint y_pos = env->CallIntMethod(y_buf, positionMethod);
    jint u_pos = env->CallIntMethod(u_buf, positionMethod);
    jint v_pos = env->CallIntMethod(v_buf, positionMethod);

    // اعمال آفست روی پوینترهای بومی دریافتی از GetDirectBufferAddress
    uint8_t* y_data = reinterpret_cast<uint8_t*>(env->GetDirectBufferAddress(y_buf)) + y_pos;
    uint8_t* u_data = reinterpret_cast<uint8_t*>(env->GetDirectBufferAddress(u_buf)) + u_pos;
    uint8_t* v_data = reinterpret_cast<uint8_t*>(env->GetDirectBufferAddress(v_buf)) + v_pos;

    if (!y_data || !u_data || !v_data) return;

    TTInstance* inst = GetTTInstance(env, thiz);
    if (inst == nullptr) return;

    int out_width = (rotation == 90 || rotation == 270) ? height : width;
    int out_height = (rotation == 90 || rotation == 270) ? width : height;

    int y_size = out_width * out_height;
    int uv_width = out_width / 2;
    int uv_height = out_height / 2;
    int uv_size = uv_width * uv_height;
    int total_size = y_size + 2 * uv_size;

    static thread_local std::vector<uint8_t> out_buffer;
    if (out_buffer.size() < static_cast<size_t>(total_size)) {
        out_buffer.resize(total_size);
    }

    uint8_t* dst_y = out_buffer.data();
    uint8_t* dst_u = dst_y + y_size;
    uint8_t* dst_v = dst_u + uv_size;

    if (rotation == 90) {
        for (int r = 0; r < height; ++r) {
            int src_row = r * y_stride;
            for (int c = 0; c < width; ++c) {
                dst_y[c * height + (height - 1 - r)] = y_data[src_row + c];
            }
        }
        for (int r = 0; r < height / 2; ++r) {
            int src_u_row = r * u_stride;
            int src_v_row = r * v_stride;
            for (int c = 0; c < width / 2; ++c) {
                int dst_idx = c * (height / 2) + ((height / 2) - 1 - r);
                dst_u[dst_idx] = u_data[src_u_row + c * u_pix_stride];
                dst_v[dst_idx] = v_data[src_v_row + c * v_pix_stride];
            }
        }
    } 
    else if (rotation == 180) {
        for (int r = 0; r < height; ++r) {
            int src_row = r * y_stride;
            int dst_row = (height - 1 - r) * width;
            for (int c = 0; c < width; ++c) {
                dst_y[dst_row + (width - 1 - c)] = y_data[src_row + c];
            }
        }
        for (int r = 0; r < height / 2; ++r) {
            int src_u_row = r * u_stride;
            int src_v_row = r * v_stride;
            int dst_row = ((height / 2) - 1 - r) * (width / 2);
            for (int c = 0; c < width / 2; ++c) {
                int dst_idx = dst_row + ((width / 2) - 1 - c);
                dst_u[dst_idx] = u_data[src_u_row + c * u_pix_stride];
                dst_v[dst_idx] = v_data[src_v_row + c * v_pix_stride];
            }
        }
    } 
    else if (rotation == 270) {
        for (int r = 0; r < height; ++r) {
            int src_row = r * y_stride;
            for (int c = 0; c < width; ++c) {
                dst_y[(width - 1 - c) * height + r] = y_data[src_row + c];
            }
        }
        for (int r = 0; r < height / 2; ++r) {
            int src_u_row = r * u_stride;
            int src_v_row = r * v_stride;
            for (int c = 0; c < width / 2; ++c) {
                int dst_idx = ((width / 2) - 1 - c) * (height / 2) + r;
                dst_u[dst_idx] = u_data[src_u_row + c * u_pix_stride];
                dst_v[dst_idx] = v_data[src_v_row + c * v_pix_stride];
            }
        }
    }
    else {
        for (int r = 0; r < height; ++r) {
            memcpy(dst_y + r * width, y_data + r * y_stride, width);
        }
        for (int r = 0; r < height / 2; ++r) {
            int dst_offset = r * (width / 2);
            int src_u_row = r * u_stride;
            int src_v_row = r * v_stride;
            for (int c = 0; c < width / 2; ++c) {
                dst_u[dst_offset + c] = u_data[src_u_row + c * u_pix_stride];
                dst_v[dst_offset + c] = v_data[src_v_row + c * v_pix_stride];
            }
        }
    }

    TT_PushInternalVideo(inst, reinterpret_cast<const char*>(dst_y), total_size, out_width, out_height, 100, JFALSE);
}
    
    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setVoiceActivationLevel(JNIEnv* env,
                                                                                     jobject thiz,
                                                                                     jint nLevel)
    {
        return TT_SetVoiceActivationLevel(GetTTInstance(env, thiz),
                                          nLevel);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getVoiceActivationLevel(JNIEnv* env,
                                                                                 jobject thiz)
    {
        return TT_GetVoiceActivationLevel(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setVoiceActivationStopDelay(JNIEnv* env,
                                                                                         jobject thiz,
                                                                                         jint nDelayMSec)
    {
        return TT_SetVoiceActivationStopDelay(GetTTInstance(env, thiz),
                                              nDelayMSec);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getVoiceActivationStopDelay(JNIEnv* env,
                                                                                     jobject thiz)
    {
        return TT_GetVoiceActivationStopDelay(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startRecordingMuxedAudioFile(JNIEnv* env,
                                                                                          jobject thiz,
                                                                                          jobject lpAudioCodec,
                                                                                          jstring szAudioFileName,
                                                                                          jint uAFF)
    {
        THROW_NULLEX(env, lpAudioCodec, false);
        THROW_NULLEX(env, szAudioFileName, false);

        AudioCodec audcodec = {};
        setAudioCodec(env, audcodec, lpAudioCodec, J2N);

        return TT_StartRecordingMuxedAudioFile(GetTTInstance(env, thiz),
                                               &audcodec, ttstr(env, szAudioFileName), (AudioFileFormat)uAFF);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startRecordingMuxedAudioFileEx(JNIEnv* env,
                                                                                            jobject thiz,
                                                                                            jint nChannelID,
                                                                                            jstring szAudioFileName,
                                                                                            jint uAFF)
    {
        THROW_NULLEX(env, szAudioFileName, false);

        return TT_StartRecordingMuxedAudioFileEx(GetTTInstance(env, thiz),
                                                 nChannelID, ttstr(env, szAudioFileName), (AudioFileFormat)uAFF);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startRecordingMuxedStreams(JNIEnv* env,
                                                                                        jobject thiz,
                                                                                        jint uStreamTypes,
                                                                                        jobject lpAudioCodec,
                                                                                        jstring szAudioFileName,
                                                                                        jint uAFF)
    {
        THROW_NULLEX(env, lpAudioCodec, false);
        THROW_NULLEX(env, szAudioFileName, false);

        AudioCodec audcodec = {};
        setAudioCodec(env, audcodec, lpAudioCodec, J2N);
        
        return TT_StartRecordingMuxedStreams(GetTTInstance(env, thiz),
                                             uStreamTypes, &audcodec,
                                             ttstr(env, szAudioFileName), (AudioFileFormat)uAFF);
    }


    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_stopRecordingMuxedAudioFile(JNIEnv* env,
                                                                                         jobject thiz)
    {
        return TT_StopRecordingMuxedAudioFile(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_stopRecordingMuxedAudioFileEx(JNIEnv* env,
                                                                                           jobject thiz,
                                                                                           jint nChannelID)
    {
        return TT_StopRecordingMuxedAudioFileEx(GetTTInstance(env, thiz), nChannelID);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startVideoCaptureTransmission(JNIEnv* env,
                                                                                           jobject thiz,
                                                                                           jobject lpVideoCodec)
    {
        THROW_NULLEX(env, lpVideoCodec, false);

        VideoCodec vidcodec = {};
        setVideoCodec(env, vidcodec, lpVideoCodec, J2N);
        return TT_StartVideoCaptureTransmission(GetTTInstance(env, thiz),
                                                &vidcodec);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_stopVideoCaptureTransmission(JNIEnv* env,
                                                                                          jobject thiz)
    {
        return TT_StopVideoCaptureTransmission(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getVideoCaptureDevices(JNIEnv* env,
                                                                                    jclass /*unused*/,
                                                                                    jobjectArray lpVideoDevices,
                                                                                    jobject lpnHowMany)
    {
        THROW_NULLEX(env, lpnHowMany, false);

        int howmany = 0;
        if(lpVideoDevices == nullptr)
        {
            if(TT_GetVideoCaptureDevices(nullptr, &howmany) == 0)
                return JFALSE;
            setIntPtr(env, lpnHowMany, howmany);
            return JTRUE;
        }

        INT32 arr_size = env->GetArrayLength(lpVideoDevices);
        std::vector<VideoCaptureDevice> devs((size_t)arr_size);
        if((arr_size != 0) && (TT_GetVideoCaptureDevices(devs.data(), &arr_size) == 0))
            return JFALSE;

        for(jsize i=0;i<arr_size;i++)
            env->SetObjectArrayElement(lpVideoDevices, i, newVideoDevice(env, devs[i]));

        setIntPtr(env, lpnHowMany, arr_size);

        return JTRUE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_initVideoCaptureDevice(JNIEnv* env,
                                                                                   jobject thiz,
                                                                                   jstring szDeviceID,
                                                                                   jobject lpVideoFormat)
    {
        VideoFormat fmt{};
        setVideoFormat(env, fmt, lpVideoFormat, J2N);

        return TT_InitVideoCaptureDevice(GetTTInstance(env, thiz),
                                         ttstr(env, szDeviceID), &fmt);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_closeVideoCaptureDevice(JNIEnv* env,
                                                                                    jobject thiz)
    {
        return TT_CloseVideoCaptureDevice(GetTTInstance(env, thiz));
    }

    JNIEXPORT jobject JNICALL Java_dk_bearware_TeamTalkBase_acquireUserVideoCaptureFrame(JNIEnv* env,
                                                                                         jobject thiz,
                                                                                         jint nUserID)
    {
        VideoFrame* vidframe = TT_AcquireUserVideoCaptureFrame(GetTTInstance(env, thiz),
                                                               nUserID);
        if(vidframe == nullptr)
            return nullptr;

        jclass cls = env->FindClass("dk/bearware/VideoFrame");
        jobject vidframe_obj = newObject(env, cls);
        setVideoFrame(env, *vidframe, vidframe_obj);

        TT_ReleaseUserVideoCaptureFrame(GetTTInstance(env, thiz), vidframe);
        return vidframe_obj;
    }

// ReleaseUserVideoCaptureFrame()

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startStreamingMediaFileToChannel(JNIEnv* env,
                                                                                              jobject thiz,
                                                                                              jstring szMediaFilePath,
                                                                                              jobject lpVideoCodec)
    {
        THROW_NULLEX(env, szMediaFilePath, false);

        VideoCodec vidcodec{};
        vidcodec.nCodec = NO_CODEC;
        if(lpVideoCodec != nullptr)
            setVideoCodec(env, vidcodec, lpVideoCodec, J2N);
        return TT_StartStreamingMediaFileToChannel(GetTTInstance(env, thiz),
                                                   ttstr(env, szMediaFilePath), &vidcodec);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_startStreamingMediaFileToChannelEx(JNIEnv* env,
                                                                                                jobject thiz,
                                                                                                jstring szMediaFilePath,
                                                                                                jobject lpMediaFilePlayback,
                                                                                                jobject lpVideoCodec)
    {
        THROW_NULLEX(env, szMediaFilePath, false);
        THROW_NULLEX(env, lpMediaFilePlayback, false);

        MediaFilePlayback mfp{};
        if (lpMediaFilePlayback != nullptr)
            setMediaFilePlayback(env, mfp, lpMediaFilePlayback, J2N);

        VideoCodec vidcodec{};
        vidcodec.nCodec = NO_CODEC;
        if(lpVideoCodec != nullptr)
            setVideoCodec(env, vidcodec, lpVideoCodec, J2N);
        return TT_StartStreamingMediaFileToChannelEx(GetTTInstance(env, thiz),
                                                     ttstr(env, szMediaFilePath), &mfp, &vidcodec);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_updateStreamingMediaFileToChannel(JNIEnv* env,
                                                                                               jobject thiz,
                                                                                               jobject lpMediaFilePlayback,
                                                                                               jobject lpVideoCodec)
    {
        THROW_NULLEX(env, lpMediaFilePlayback, false);

        MediaFilePlayback mfp{};
        if (lpMediaFilePlayback != nullptr)
            setMediaFilePlayback(env, mfp, lpMediaFilePlayback, J2N);

        VideoCodec vidcodec{};
        vidcodec.nCodec = NO_CODEC;
        if (lpVideoCodec != nullptr)
            setVideoCodec(env, vidcodec, lpVideoCodec, J2N);

        return TT_UpdateStreamingMediaFileToChannel(GetTTInstance(env, thiz),
                                                    &mfp, &vidcodec);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_stopStreamingMediaFileToChannel(JNIEnv* env,
                                                                                             jobject thiz)
    {
        return TT_StopStreamingMediaFileToChannel(GetTTInstance(env, thiz));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_initLocalPlayback(JNIEnv* env, jobject thiz,
                                                                           jstring szMediaFilePath,
                                                                           jobject lpMediaFilePlayback) {
        THROW_NULLEX(env, szMediaFilePath, 0);
        THROW_NULLEX(env, lpMediaFilePlayback, 0);

        MediaFilePlayback playback{};
        setMediaFilePlayback(env, playback, lpMediaFilePlayback, J2N);

        return TT_InitLocalPlayback(GetTTInstance(env, thiz),
                                    ttstr(env, szMediaFilePath), &playback);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_updateLocalPlayback(JNIEnv* env, jobject thiz,
                                                                                 jint nPlaybackSessionID,
                                                                                 jobject lpMediaFilePlayback) {

        THROW_NULLEX(env, lpMediaFilePlayback, false);

        MediaFilePlayback playback{};
        setMediaFilePlayback(env, playback, lpMediaFilePlayback, J2N);

        return TT_UpdateLocalPlayback(GetTTInstance(env, thiz),
                                      nPlaybackSessionID, &playback);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_stopLocalPlayback(JNIEnv* env, jobject thiz,
                                                                               jint nPlaybackSessionID) {
        return TT_StopLocalPlayback(GetTTInstance(env, thiz),
                                    nPlaybackSessionID);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getMediaFileInfo(JNIEnv* env,
                                                                              jclass /*unused*/,
                                                                              jstring szMediaFilePath,
                                                                              jobject lpMediaFileInfo)
    {
        THROW_NULLEX(env, szMediaFilePath, false);
        THROW_NULLEX(env, lpMediaFileInfo, false);

        MediaFileInfo mfi;
        if(TT_GetMediaFileInfo(ttstr(env, szMediaFilePath), &mfi) != 0)
        {
            setMediaFileInfo(env, mfi, lpMediaFileInfo, N2J);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jobject JNICALL Java_dk_bearware_TeamTalkBase_acquireUserMediaVideoFrame(JNIEnv* env,
                                                                                       jobject thiz,
                                                                                       jint nUserID)
    {
        VideoFrame* vidframe = TT_AcquireUserMediaVideoFrame(GetTTInstance(env, thiz),
                                                             nUserID);

        if(vidframe == nullptr)
            return nullptr;

        jclass cls = env->FindClass("dk/bearware/VideoFrame");
        jobject vidframe_obj = newObject(env, cls);
        setVideoFrame(env, *vidframe, vidframe_obj);

        TT_ReleaseUserMediaVideoFrame(GetTTInstance(env, thiz), vidframe);
        return vidframe_obj;

    }

/*
    JNIEXPORT jobject JNICALL Java_dk_bearware_TeamTalkBase_releaseUserMediaVideoFrame(JNIEnv* env,
                                                                                       jobject thiz,
                                                                                       jlong lpTTInstance,
                                                                                       jobject lpVideoFrame)
    {
        return NULL;
    }
*/
    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_sendDesktopWindow(JNIEnv* env,
                                                                           jobject thiz,
                                                                           jobject lpDesktopWindow,
                                                                           jint nConvertBmpFormat)
    {
        DesktopWindow wnd;
        setDesktopWindow(env, wnd, lpDesktopWindow, J2N);

        jclass cls = env->GetObjectClass(lpDesktopWindow);
        jfieldID fid_frmbuf = env->GetFieldID(cls, "frameBuffer", "[B");
        assert(fid_frmbuf);

        auto buf = (jbyteArray)env->GetObjectField(lpDesktopWindow, fid_frmbuf);
        jbyte* bufptr = env->GetByteArrayElements(buf, nullptr);
        if(bufptr == nullptr)
            return -1;

        wnd.frameBuffer = bufptr;
        wnd.nFrameBufferSize = env->GetArrayLength(buf);

        jint const ret = TT_SendDesktopWindow(GetTTInstance(env, thiz),
                                        &wnd, (BitmapFormat)nConvertBmpFormat);

       env->ReleaseByteArrayElements(buf, bufptr, 0);
       return ret;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_closeDesktopWindow(JNIEnv* env,
                                                                                jobject thiz)
    {
        return TT_CloseDesktopWindow(GetTTInstance(env, thiz));
    }

//TODO: Palette_GetColorTable

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_sendDesktopCursorPosition(JNIEnv* env,
                                                                                       jobject thiz,
                                                                                       jint nPosX,
                                                                                       jint nPosY)
    {
        return TT_SendDesktopCursorPosition(GetTTInstance(env, thiz),
                                            (UINT16)nPosX, (UINT16)nPosY);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_sendDesktopInput(JNIEnv* env,
                                                                              jobject thiz,
                                                                              jint nUserID,
                                                                              jobjectArray lpDesktopInputs)
    {
        THROW_NULLEX(env, lpDesktopInputs, false);

        jsize const len = env->GetArrayLength(lpDesktopInputs);
        std::vector<DesktopInput> inputs(len);
        for(jsize i=0;i<len;i++)
            setDesktopInput(env, inputs[i], env->GetObjectArrayElement(lpDesktopInputs, i), J2N);
        return TT_SendDesktopInput(GetTTInstance(env, thiz), nUserID,
                                   inputs.data(), len);
    }

    JNIEXPORT jobject JNICALL Java_dk_bearware_TeamTalkBase_acquireUserDesktopWindow(JNIEnv* env,
                                                                                     jobject thiz,
                                                                                     jint nUserID) {
        DesktopWindow* deskwnd = TT_AcquireUserDesktopWindow(GetTTInstance(env, thiz),
                                                             nUserID);
        if(deskwnd == nullptr)
            return nullptr;

        jclass cls = env->FindClass("dk/bearware/DesktopWindow");
        jobject deskwnd_obj = newObject(env, cls);
        setDesktopWindow(env, *deskwnd, deskwnd_obj, N2J);

        TT_ReleaseUserDesktopWindow(GetTTInstance(env, thiz), deskwnd);
        return deskwnd_obj;
    }

    JNIEXPORT jobject JNICALL Java_dk_bearware_TeamTalkBase_acquireUserDesktopWindowEx(JNIEnv* env,
                                                                                       jobject thiz,
                                                                                       jint nUserID,
                                                                                       jint nBitmapFormat) {
        DesktopWindow* deskwnd = TT_AcquireUserDesktopWindowEx(GetTTInstance(env, thiz),
                                                               nUserID, (BitmapFormat)nBitmapFormat);
        if(deskwnd == nullptr)
            return nullptr;

        jclass cls = env->FindClass("dk/bearware/DesktopWindow");
        jobject deskwnd_obj = newObject(env, cls);
        setDesktopWindow(env, *deskwnd, deskwnd_obj, N2J);

        TT_ReleaseUserDesktopWindow(GetTTInstance(env, thiz), deskwnd);
        return deskwnd_obj;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setEncryptionContext(JNIEnv* env,
                                                                                  jobject thiz,
                                                                                  jobject lpEncryptionContext) {
        THROW_NULLEX(env, lpEncryptionContext, false);

        EncryptionContext context{};
        setEncryptionContext(env, context, lpEncryptionContext, J2N);

        return TT_SetEncryptionContext(GetTTInstance(env, thiz),
                                       &context);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_connect(JNIEnv* env,
                                                                     jobject thiz,
                                                                     jstring szHostAddress,
                                                                     jint nTcpPort,
                                                                     jint nUdpPort,
                                                                     jint nLocalTcpPort,
                                                                     jint nLocalUdpPort,
                                                                     jboolean bEncrypted)
    {
        THROW_NULLEX(env, szHostAddress, false);

        return TT_Connect(GetTTInstance(env, thiz),
                          ttstr(env,szHostAddress), nTcpPort, nUdpPort,
                          nLocalTcpPort, nLocalUdpPort, bEncrypted);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_connectSysID(JNIEnv* env,
                                                                          jobject thiz,
                                                                          jstring szHostAddress,
                                                                          jint nTcpPort,
                                                                          jint nUdpPort,
                                                                          jint nLocalTcpPort,
                                                                          jint nLocalUdpPort,
                                                                          jboolean bEncrypted,
                                                                          jstring szSystemID)
    {
        THROW_NULLEX(env, szHostAddress, false);
        THROW_NULLEX(env, szSystemID, false);

        return TT_ConnectSysID(GetTTInstance(env, thiz),
                               ttstr(env,szHostAddress), nTcpPort, nUdpPort,
                               nLocalTcpPort, nLocalUdpPort, bEncrypted,
                               ttstr(env, szSystemID));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_connectEx(JNIEnv* env,
                                                                       jobject thiz,
                                                                       jstring szHostAddress,
                                                                       jint nTcpPort,
                                                                       jint nUdpPort,
                                                                       jstring szBindIPAddr,
                                                                       jint nLocalTcpPort,
                                                                       jint nLocalUdpPort,
                                                                       jboolean bEncrypted)
    {
        THROW_NULLEX(env, szHostAddress, false);
        THROW_NULLEX(env, szBindIPAddr, false);

        return TT_ConnectEx(GetTTInstance(env, thiz),
                            ttstr(env, szHostAddress), nTcpPort, nUdpPort,
                            ttstr(env, szBindIPAddr), nLocalTcpPort, nLocalUdpPort,
                            bEncrypted);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_disconnect(JNIEnv* env,
                                                                        jobject thiz)

    {
        return TT_Disconnect(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_queryMaxPayload(JNIEnv* env,
                                                                             jobject thiz,
                                                                             jint nUserID)
    {
        return TT_QueryMaxPayload(GetTTInstance(env, thiz), nUserID);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getClientStatistics(JNIEnv* env,
                                                                                 jobject thiz,
                                                                                 jobject lpClientStatistics)
    {
        THROW_NULLEX(env, lpClientStatistics, false);

        ClientStatistics stats;
        if(TT_GetClientStatistics(GetTTInstance(env, thiz),
                                  &stats) != 0)
        {
            setClientStatistics(env, stats, lpClientStatistics);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setClientKeepAlive(JNIEnv* env,
                                                                                jobject thiz,
                                                                                jobject lpClientKeepAlive)
    {
        THROW_NULLEX(env, lpClientKeepAlive, false);

        ClientKeepAlive ka;
        setClientKeepAlive(env, ka, lpClientKeepAlive, J2N);

        return TT_SetClientKeepAlive(GetTTInstance(env, thiz), &ka);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getClientKeepAlive(JNIEnv* env,
                                                                                jobject thiz,
                                                                                jobject lpClientKeepAlive)
    {
        THROW_NULLEX(env, lpClientKeepAlive, false);

        ClientKeepAlive ka{};
        if (TT_GetClientKeepAlive(GetTTInstance(env, thiz), &ka) != 0) {
            setClientKeepAlive(env, ka, lpClientKeepAlive, N2J);
            return JTRUE;
        }

        return JFALSE;
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doPing(JNIEnv* env,
                                                                jobject thiz)
    {
        return TT_DoPing(GetTTInstance(env, thiz));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doLogin(JNIEnv* env,
                                                                 jobject thiz,
                                                                 jstring szNickname,
                                                                 jstring szUsername,
                                                                 jstring szPassword)
    {
        THROW_NULLEX(env, szNickname, -1);
        THROW_NULLEX(env, szUsername, -1);
        THROW_NULLEX(env, szPassword, -1);

        return TT_DoLogin(GetTTInstance(env, thiz),
                          ttstr(env, szNickname), ttstr(env, szUsername),
                          ttstr(env, szPassword));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doLoginEx(JNIEnv* env,
                                                                   jobject thiz,
                                                                   jstring szNickname,
                                                                   jstring szUsername,
                                                                   jstring szPassword,
                                                                   jstring szClientName)
    {
        THROW_NULLEX(env, szNickname, -1);
        THROW_NULLEX(env, szUsername, -1);
        THROW_NULLEX(env, szPassword, -1);
        THROW_NULLEX(env, szClientName, -1);

        return TT_DoLoginEx(GetTTInstance(env, thiz),
                            ttstr(env, szNickname), ttstr(env, szUsername),
                            ttstr(env, szPassword), ttstr(env, szClientName));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doLogout(JNIEnv* env,
                                                                  jobject thiz)
    {
        return TT_DoLogout(GetTTInstance(env, thiz));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doJoinChannel(JNIEnv* env,
                                                                       jobject thiz,
                                                                       jobject lpChannel)
    {
        THROW_NULLEX(env, lpChannel, -1);

        Channel chan{};
        setChannel(env, chan, lpChannel, J2N);
        return TT_DoJoinChannel(GetTTInstance(env, thiz), &chan);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doJoinChannelByID(JNIEnv* env,
                                                                           jobject thiz,
                                                                           jint nChannelID,
                                                                           jstring szPassword)
    {
        THROW_NULLEX(env, szPassword, -1);

        return TT_DoJoinChannelByID(GetTTInstance(env, thiz),
                                    nChannelID, ttstr(env, szPassword));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doLeaveChannel(JNIEnv* env,
                                                                        jobject thiz)
    {
        return TT_DoLeaveChannel(GetTTInstance(env, thiz));

    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doChangeNickname(JNIEnv* env,
                                                                          jobject thiz,
                                                                          jstring szNewNick)
    {
        THROW_NULLEX(env, szNewNick, -1);

        return TT_DoChangeNickname(GetTTInstance(env, thiz),
                                   ttstr(env, szNewNick));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doChangeStatus(JNIEnv* env,
                                                                        jobject thiz,
                                                                        jint nStatusMode,
                                                                        jstring szStatusMessage)
    {
        THROW_NULLEX(env, szStatusMessage, -1);

        return TT_DoChangeStatus(GetTTInstance(env, thiz),
                                 nStatusMode, ttstr(env, szStatusMessage));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doTextMessage(JNIEnv* env,
                                                                       jobject thiz,
                                                                       jobject lpTextMessage)
    {
        THROW_NULLEX(env, lpTextMessage, -1);

        TextMessage msg;
        setTextMessage(env, msg, lpTextMessage, J2N);
        return TT_DoTextMessage(GetTTInstance(env, thiz), &msg);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doChannelOp(JNIEnv* env,
                                                                     jobject thiz,
                                                                     jint nUserID,
                                                                     jint nChannelID,
                                                                     jboolean bMakeOperator)
    {
        return TT_DoChannelOp(GetTTInstance(env, thiz), nUserID, nChannelID, bMakeOperator);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doChannelOpEx(JNIEnv* env,
                                                                       jobject thiz,
                                                                       jint nUserID,
                                                                       jint nChannelID,
                                                                       jstring szOpPassword,
                                                                       jboolean bMakeOperator)
    {
        THROW_NULLEX(env, szOpPassword, -1);

        return TT_DoChannelOpEx(GetTTInstance(env, thiz), nUserID,
                                nChannelID, ttstr(env, szOpPassword), bMakeOperator);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doKickUser(JNIEnv* env,
                                                                    jobject thiz,
                                                                    jint nUserID,
                                                                    jint nChannelID)
    {
        return TT_DoKickUser(GetTTInstance(env, thiz), nUserID,
                             nChannelID);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doSendFile(JNIEnv* env,
                                                                    jobject thiz,
                                                                    jint nChannelID,
                                                                    jstring szLocalFilePath)
    {
        THROW_NULLEX(env, szLocalFilePath, -1);

        return TT_DoSendFile(GetTTInstance(env, thiz), nChannelID,
                             ttstr(env, szLocalFilePath));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doRecvFile(JNIEnv* env,
                                                                    jobject thiz,
                                                                    jint nChannelID,
                                                                    jint nFileID,
                                                                    jstring szLocalFilePath)
    {
        THROW_NULLEX(env, szLocalFilePath, -1);

        return TT_DoRecvFile(GetTTInstance(env, thiz), nChannelID, nFileID,
                             ttstr(env, szLocalFilePath));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doDeleteFile(JNIEnv* env,
                                                                      jobject thiz,
                                                                      jint nChannelID,
                                                                      jint nFileID)
    {
        return TT_DoDeleteFile(GetTTInstance(env, thiz), nChannelID, nFileID);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doSubscribe(JNIEnv* env,
                                                                     jobject thiz,
                                                                     jint nUserID,
                                                                     jint uSubscriptions)
    {
        return TT_DoSubscribe(GetTTInstance(env, thiz), nUserID, uSubscriptions);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doUnsubscribe(JNIEnv* env,
                                                                       jobject thiz,
                                                                       jint nUserID,
                                                                       jint uSubscriptions)
    {
        return TT_DoUnsubscribe(GetTTInstance(env, thiz), nUserID, uSubscriptions);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doMakeChannel(JNIEnv* env,
                                                                       jobject thiz,
                                                                       jobject lpChannel)
    {
        THROW_NULLEX(env, lpChannel, -1);

        Channel chan;
        setChannel(env, chan, lpChannel, J2N);
        return TT_DoMakeChannel(GetTTInstance(env, thiz), &chan);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doUpdateChannel(JNIEnv* env,
                                                                         jobject thiz,
                                                                         jobject lpChannel)
    {
        THROW_NULLEX(env, lpChannel, -1);

        Channel chan;
        setChannel(env, chan, lpChannel, J2N);
        return TT_DoUpdateChannel(GetTTInstance(env, thiz), &chan);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doRemoveChannel(JNIEnv* env,
                                                                         jobject thiz,
                                                                         jint nChannelID)
    {
        return TT_DoRemoveChannel(GetTTInstance(env, thiz), nChannelID);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doMoveUser(JNIEnv* env,
                                                                    jobject thiz,
                                                                    jint nUserID,
                                                                    jint nChannelID)
    {
        return TT_DoMoveUser(GetTTInstance(env, thiz), nUserID, nChannelID);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doUpdateServer(JNIEnv* env,
                                                                        jobject thiz,
                                                                        jobject lpServerProperties)
    {
        THROW_NULLEX(env, lpServerProperties, -1);

        ServerProperties srvprop;
        setServerProperties(env, srvprop, lpServerProperties, J2N);
        return TT_DoUpdateServer(GetTTInstance(env, thiz), &srvprop);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doListUserAccounts(JNIEnv* env,
                                                                            jobject thiz,
                                                                            jint nIndex,
                                                                            jint nCount)
    {
        return TT_DoListUserAccounts(GetTTInstance(env, thiz), nIndex, nCount);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doNewUserAccount(JNIEnv* env,
                                                                          jobject thiz,
                                                                          jobject lpUserAccount)
    {
        THROW_NULLEX(env, lpUserAccount, -1);

        UserAccount account;
        setUserAccount(env, account, lpUserAccount, J2N);
        return TT_DoNewUserAccount(GetTTInstance(env, thiz), &account);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doDeleteUserAccount(JNIEnv* env,
                                                                             jobject thiz,
                                                                             jstring szUsername)
    {
        THROW_NULLEX(env, szUsername, -1);

        return TT_DoDeleteUserAccount(GetTTInstance(env, thiz), ttstr(env, szUsername));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doBanUser(JNIEnv* env,
                                                                   jobject thiz,
                                                                   jint nUserID,
                                                                   jint nChannelID)
    {
        return TT_DoBanUser(GetTTInstance(env, thiz), nUserID, nChannelID);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doBanUserEx(JNIEnv* env,
                                                                     jobject thiz,
                                                                     jint nUserID,
                                                                     jint uBanTypes)
    {
        return TT_DoBanUserEx(GetTTInstance(env, thiz), nUserID, uBanTypes);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doBan(JNIEnv* env,
                                                               jobject thiz,
                                                               jobject lpBannedUser)
    {
        BannedUser ban;
        setBannedUser(env, ban, lpBannedUser, J2N);
        return TT_DoBan(GetTTInstance(env, thiz), &ban);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doBanIPAddress(JNIEnv* env,
                                                                        jobject thiz,
                                                                        jstring szIPAddress,
                                                                        jint nChannelID)
    {
        THROW_NULLEX(env, szIPAddress, -1);

        return TT_DoBanIPAddress(GetTTInstance(env, thiz),
                                 ttstr(env, szIPAddress), nChannelID);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doUnBanUser(JNIEnv* env,
                                                                     jobject thiz,
                                                                     jstring szIPAddress,
                                                                     jint nChannelID)
    {
        THROW_NULLEX(env, szIPAddress, -1);

        return TT_DoUnBanUser(GetTTInstance(env, thiz),
                              ttstr(env, szIPAddress), nChannelID);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doUnBanUserEx(JNIEnv* env,
                                                                       jobject thiz,
                                                                       jobject lpBannedUser)
    {
        THROW_NULLEX(env, lpBannedUser, -1);
        BannedUser ban;
        setBannedUser(env, ban, lpBannedUser, J2N);
        return TT_DoUnBanUserEx(GetTTInstance(env, thiz), &ban);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doListBans(JNIEnv* env,
                                                                    jobject thiz,
                                                                    jint nChannelID,
                                                                    jint nIndex,
                                                                    jint nCount)
    {
        return TT_DoListBans(GetTTInstance(env, thiz), nChannelID,
                             nIndex, nCount);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doSaveConfig(JNIEnv* env,
                                                                      jobject thiz)
    {
        return TT_DoSaveConfig(GetTTInstance(env, thiz));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doQueryServerStats(JNIEnv* env,
                                                                            jobject thiz)
    {
        return TT_DoQueryServerStats(GetTTInstance(env, thiz));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_doQuit(JNIEnv* env,
                                                                jobject thiz)
    {
        return TT_DoQuit(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getServerProperties(JNIEnv* env,
                                                                                 jobject thiz,
                                                                                 jobject lpServerProperties)
    {
        THROW_NULLEX(env, lpServerProperties, false);

        ServerProperties srvprop;
        if(TT_GetServerProperties(GetTTInstance(env, thiz),
                                  &srvprop) != 0)
        {
            setServerProperties(env, srvprop, lpServerProperties, N2J);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getServerUsers(JNIEnv* env,
                                                                            jobject thiz,
                                                                            jobjectArray lpUsers,
                                                                            jobject lpnHowMany)
    {
        THROW_NULLEX(env, lpnHowMany, false);

        INT32 n_users = 0;
        if(lpUsers == nullptr)
        {
            if(TT_GetServerUsers(GetTTInstance(env, thiz),
                                 nullptr, &n_users) != 0)
            {
                setIntPtr(env, lpnHowMany, n_users);
                return JTRUE;
            }
            return JFALSE;
        }

        n_users = getIntPtr(env, lpnHowMany);
        std::vector<User> users(n_users);

        if(n_users>0 &&
           (TT_GetServerUsers(GetTTInstance(env, thiz), users.data(),
                             &n_users) != 0))
        {
            n_users = std::min(n_users, (INT32)getIntPtr(env, lpnHowMany));
            setIntPtr(env, lpnHowMany, n_users);

            for(jsize i=0; i < jsize(n_users); i++)
            {
                jclass cls = env->FindClass("dk/bearware/User");
                jobject user_obj = newObject(env, cls);
                setUser(env, users[i], user_obj);
                env->SetObjectArrayElement(lpUsers, i, user_obj);
            }
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getRootChannelID(JNIEnv* env,
                                                                          jobject thiz)
    {
        return TT_GetRootChannelID(GetTTInstance(env, thiz));
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getMyChannelID(JNIEnv* env,
                                                                        jobject thiz)
    {
        return TT_GetMyChannelID(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getChannel(JNIEnv* env,
                                                                        jobject thiz,
                                                                        jint nChannelID,
                                                                        jobject lpChannel)
    {
        THROW_NULLEX(env, lpChannel, false);

        Channel chan;
        if(TT_GetChannel(GetTTInstance(env, thiz),
                         nChannelID, &chan) != 0)
        {
            setChannel(env, chan, lpChannel, N2J);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jstring JNICALL Java_dk_bearware_TeamTalkBase_getChannelPath(JNIEnv* env,
                                                                           jobject thiz,
                                                                           jint nChannelID)
    {
        TTCHAR channel[TT_STRLEN] = {};
        TT_GetChannelPath(GetTTInstance(env, thiz),
                          nChannelID, channel);
        return NEW_JSTRING(env, channel);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getChannelIDFromPath(JNIEnv* env,
                                                                              jobject thiz,
                                                                              jstring szChannelPath)
    {
        THROW_NULLEX(env, szChannelPath, -1);

        ttstr channel(env, szChannelPath);
        return TT_GetChannelIDFromPath(GetTTInstance(env, thiz),
                                       channel);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getChannelUsers(JNIEnv* env,
                                                                             jobject thiz,
                                                                             jint nChannelID,
                                                                             jobjectArray lpUsers,
                                                                             jobject lpnHowMany)
    {
        THROW_NULLEX(env, lpnHowMany, false);

        INT32 n_users = 0;
        if(lpUsers == nullptr)
        {
            if(TT_GetChannelUsers(GetTTInstance(env, thiz),
                                  nChannelID, nullptr, &n_users) != 0)
            {
                setIntPtr(env, lpnHowMany, n_users);
                return JTRUE;
            }
            return JFALSE;
        }

        n_users = getIntPtr(env, lpnHowMany);
        std::vector<User> users(n_users);

        if(n_users>0 &&
           (TT_GetChannelUsers(GetTTInstance(env, thiz),
                              nChannelID, users.data(), &n_users) != 0))
        {
            n_users = std::min(n_users, (INT32)getIntPtr(env, lpnHowMany));
            setIntPtr(env, lpnHowMany, n_users);
            for(jsize i=0; i < jsize(n_users); i++)
            {
                jclass cls = env->FindClass("dk/bearware/User");
                jobject user_obj = newObject(env, cls);
                setUser(env, users[i], user_obj);
                env->SetObjectArrayElement(lpUsers, i, user_obj);
            }

            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getChannelFiles(JNIEnv* env,
                                                                             jobject thiz,
                                                                             jint nChannelID,
                                                                             jobjectArray lpRemoteFiles,
                                                                             jobject lpnHowMany)
    {
        THROW_NULLEX(env, lpnHowMany, false);

        INT32 n_files = 0;
        if(lpRemoteFiles == nullptr)
        {
            if(TT_GetChannelFiles(GetTTInstance(env, thiz),
                                  nChannelID, nullptr, &n_files) != 0)
            {
                setIntPtr(env, lpnHowMany, n_files);
                return JTRUE;
            }
            return JFALSE;
        }

        n_files = getIntPtr(env, lpnHowMany);
        std::vector<RemoteFile> files(n_files);

        if(n_files>0 &&
           (TT_GetChannelFiles(GetTTInstance(env, thiz),
                              nChannelID, files.data(), &n_files) != 0))
        {
            n_files = std::min(n_files, (INT32)getIntPtr(env, lpnHowMany));
            if(n_files>0)
            {
                std::vector<jobject> const jfiles(n_files);
                jclass cls = env->FindClass("dk/bearware/RemoteFile");
                for(jsize i=0; i < jsize(n_files); i++)
                {
                    jobject file_obj = newObject(env, cls);
                    setRemoteFile(env, files[i], file_obj, N2J);
                    env->SetObjectArrayElement(lpRemoteFiles, i, file_obj);
                }
            }
            setIntPtr(env, lpnHowMany, n_files);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getChannelRemoteFile(JNIEnv* env,
                                                                                  jobject thiz,
                                                                                  jint nChannelID,
                                                                                  jint nFileID,
                                                                                  jobject lpRemoteFile)
    {
        THROW_NULLEX(env, lpRemoteFile, false);

        RemoteFile finfo;
        if(TT_GetChannelFile(GetTTInstance(env, thiz),
                             nChannelID, nFileID, &finfo) != 0)
        {
            setRemoteFile(env, finfo, lpRemoteFile, N2J);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_isChannelOperator(JNIEnv* env,
                                                                               jobject thiz,
                                                                               jint nUserID,
                                                                               jint nChannelID)
    {
        return TT_IsChannelOperator(GetTTInstance(env, thiz),
                                    nUserID, nChannelID);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getServerChannels(JNIEnv* env,
                                                                               jobject thiz,
                                                                               jobjectArray lpChannels,
                                                                               jobject lpnHowMany)
    {
        THROW_NULLEX(env, lpnHowMany, false);

        int n_channels = 0;
        if(lpChannels == nullptr)
        {
            if(TT_GetServerChannels(GetTTInstance(env, thiz),
                                    nullptr, &n_channels) != 0)
            {
                setIntPtr(env, lpnHowMany, n_channels);
                return JTRUE;
            }
            return JFALSE;
        }

        n_channels = getIntPtr(env, lpnHowMany);
        std::vector<Channel> channels(n_channels);

        if(n_channels>0 &&
           (TT_GetServerChannels(GetTTInstance(env, thiz), channels.data(),
                                &n_channels) != 0))
        {
            n_channels = std::min(n_channels, (INT32)getIntPtr(env, lpnHowMany));
            setIntPtr(env, lpnHowMany, n_channels);
            jclass cls = env->FindClass("dk/bearware/Channel");
            for(jsize i=0;i<jsize(n_channels);i++)
            {
                jobject chan_obj = newObject(env, cls);
                setChannel(env, channels[i], chan_obj, N2J);
                env->SetObjectArrayElement(lpChannels, i, chan_obj);
            }
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_TeamTalkBase_getMyUserID(JNIEnv* env,
                                                                     jobject thiz)
    {
        return TT_GetMyUserID(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getMyUserAccount(JNIEnv* env,
                                                                              jobject thiz,
                                                                              jobject lpUserAccount)
    {
        THROW_NULLEX(env, lpUserAccount, false);

        UserAccount account;
        if(TT_GetMyUserAccount(GetTTInstance(env, thiz), &account) != 0)
        {
            setUserAccount(env, account, lpUserAccount, N2J);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getMyUserType(JNIEnv* env,
                                                                           jobject thiz)
    {
        return TT_GetMyUserType(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getMyUserData(JNIEnv* env,
                                                                           jobject thiz)
    {
        return TT_GetMyUserData(GetTTInstance(env, thiz));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getUser(JNIEnv* env,
                                                                     jobject thiz,
                                                                     jint nUserID,
                                                                     jobject lpUser)
    {
        THROW_NULLEX(env, lpUser, false);

        User user;
        if(TT_GetUser(GetTTInstance(env, thiz),
                      nUserID, &user) != 0)
        {
            setUser(env, user, lpUser);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getUserStatistics(JNIEnv* env,
                                                                               jobject thiz,
                                                                               jint nUserID,
                                                                               jobject lpUserStatistics)
    {
        THROW_NULLEX(env, lpUserStatistics, false);

        UserStatistics stats;
        if(TT_GetUserStatistics(GetTTInstance(env, thiz),
                                nUserID, &stats) != 0)
        {
            setUserStatistics(env, stats, lpUserStatistics);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getUserByUsername(JNIEnv* env,
                                                                               jobject thiz,
                                                                               jstring szUsername,
                                                                               jobject lpUser)
    {
        THROW_NULLEX(env, lpUser, false);

        User user;
        if(TT_GetUserByUsername(GetTTInstance(env, thiz),
                                ttstr(env, szUsername), &user) != 0)
        {
            setUser(env, user, lpUser);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserVolume(JNIEnv* env,
                                                                           jobject thiz,
                                                                           jint nUserID,
                                                                           jint nStreamType,
                                                                           jint nVolume)
    {
        return TT_SetUserVolume(GetTTInstance(env, thiz),
                                nUserID, (StreamType)nStreamType, nVolume);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserMute(JNIEnv* env,
                                                                         jobject thiz,
                                                                         jint nUserID,
                                                                         jint nStreamType,
                                                                         jboolean bMute)
    {
        return TT_SetUserMute(GetTTInstance(env, thiz),
                              nUserID, (StreamType)nStreamType, bMute);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserStoppedPlaybackDelay(JNIEnv* env,
                                                                                         jobject thiz,
                                                                                         jint nUserID,
                                                                                         jint nStreamType,
                                                                                         jint nDelayMSec)
    {
        return TT_SetUserStoppedPlaybackDelay(GetTTInstance(env, thiz),
                                              nUserID, (StreamType)nStreamType, nDelayMSec);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserJitterControl(JNIEnv* env,
                                                                                         jobject thiz,
                                                                                         jint nUserID,
                                                                                         jint nStreamType,
                                                                                         jobject lpJitterConfig)
    {

        JitterConfig config = {};

        if (lpJitterConfig != nullptr)
            setJitterConfig(env, config, lpJitterConfig);

        return TT_SetUserJitterControl(GetTTInstance(env, thiz), nUserID, (StreamType)nStreamType,
                                        ((lpJitterConfig != nullptr) ? &config : nullptr));
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getUserJitterControl(JNIEnv* env,
                                                                                        jobject thiz,
                                                                                        jint nUserID,
                                                                                        jint nStreamType,
                                                                                        jobject lpJitterConfig)
    {
        THROW_NULLEX(env, lpJitterConfig, false);

        JitterConfig config = {};

        if (TT_GetUserJitterControl(GetTTInstance(env, thiz), nUserID, (StreamType)nStreamType,
            ((lpJitterConfig != nullptr) ? &config : nullptr)) != 0)
        {
            setJitterConfig(env, lpJitterConfig, config);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserPosition(JNIEnv* env,
                                                                             jobject thiz,
                                                                             jint nUserID,
                                                                             jint nStreamType,
                                                                             jfloat x,
                                                                             jfloat y,
                                                                             jfloat z)
    {
        return TT_SetUserPosition(GetTTInstance(env, thiz),
                                  nUserID, (StreamType)nStreamType, x, y, z);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserStereo(JNIEnv* env,
                                                                           jobject thiz,
                                                                           jint nUserID,
                                                                           jint nStreamType,
                                                                           jboolean bLeftSpeaker,
                                                                           jboolean bRightSpeaker)
    {
        return TT_SetUserStereo(GetTTInstance(env, thiz),
                                nUserID, (StreamType)nStreamType, bLeftSpeaker, bRightSpeaker);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserMediaStorageDir(JNIEnv* env,
                                                                                    jobject thiz,
                                                                                    jint nUserID,
                                                                                    jstring szFolderPath,
                                                                                    jstring szFileNameVars,
                                                                                    jint uAFF)
    {
        THROW_NULLEX(env, szFolderPath, false);
        THROW_NULLEX(env, szFileNameVars, false);

        return TT_SetUserMediaStorageDir(GetTTInstance(env, thiz),
                                         nUserID, ttstr(env, szFolderPath),
                                         ttstr(env, szFileNameVars),
                                         (AudioFileFormat)uAFF);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserMediaStorageDirEx(JNIEnv* env,
                                                                                    jobject thiz,
                                                                                    jint nUserID,
                                                                                    jstring szFolderPath,
                                                                                    jstring szFileNameVars,
                                                                                    jint uAFF,
                                                                                    jint nStopRecordingExtraDelayMSec)
    {
        THROW_NULLEX(env, szFolderPath, false);
        THROW_NULLEX(env, szFileNameVars, false);

        return TT_SetUserMediaStorageDirEx(GetTTInstance(env, thiz),
                                         nUserID, ttstr(env, szFolderPath),
                                         ttstr(env, szFileNameVars),
                                         (AudioFileFormat)uAFF,
                                         nStopRecordingExtraDelayMSec);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserAudioStreamBufferSize(JNIEnv* env,
                                                                                          jobject thiz,
                                                                                          jint nUserID,
                                                                                          jint uStreamType,
                                                                                          jint nMSec)
    {
        return TT_SetUserAudioStreamBufferSize(GetTTInstance(env, thiz),
                                               nUserID, (StreamType)uStreamType, nMSec);
    }

    JNIEXPORT jobject JNICALL Java_dk_bearware_TeamTalkBase_acquireUserAudioBlock(JNIEnv* env,
                                                                                  jobject thiz,
                                                                                  jint nStreamType,
                                                                                  jint nUserID)
    {
        AudioBlock* audblock = TT_AcquireUserAudioBlock(GetTTInstance(env, thiz),
                                                        (StreamType)nStreamType, nUserID);
        if(audblock == nullptr)
            return nullptr;
        jclass cls = env->FindClass("dk/bearware/AudioBlock");
        jobject audblk_obj = newObject(env, cls);
        setAudioBlock(env, *audblock, audblk_obj, N2J);
        TT_ReleaseUserAudioBlock(GetTTInstance(env, thiz), audblock);
        return audblk_obj;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_getFileTransferInfo(JNIEnv* env,
                                                                                 jobject thiz,
                                                                                 jint nTransferID,
                                                                                 jobject lpFileTransfer)
    {
        THROW_NULLEX(env, lpFileTransfer, false);

        FileTransfer filetx;
        if(TT_GetFileTransferInfo(GetTTInstance(env, thiz),
                                  nTransferID, &filetx) != 0)
        {
            setFileTransfer(env, filetx, lpFileTransfer);
            return JTRUE;
        }
        return JFALSE;
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_cancelFileTransfer(JNIEnv* env,
                                                                                jobject thiz,
                                                                                jint nTransferID)
    {
        return TT_CancelFileTransfer(GetTTInstance(env, thiz),
                                     nTransferID);
    }

    JNIEXPORT jstring JNICALL Java_dk_bearware_TeamTalkBase_getErrorMessage(JNIEnv* env,
                                                                            jclass /*unused*/,
                                                                            jint nError)
    {
        TTCHAR szError[TT_STRLEN] = {};
        TT_GetErrorMessage(nError, szError);
        return NEW_JSTRING(env, szError);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_DBG_1SetSoundInputTone(JNIEnv* env,
                                                                                    jobject thiz,
                                                                                    jint uStreamTypes,
                                                                                    jint nFrequency) {
        return TT_DBG_SetSoundInputTone(GetTTInstance(env, thiz),
                                        uStreamTypes, nFrequency);
    }

    JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_DBG_1WriteAudioFileTone(JNIEnv* env,
                                                                                     jclass /*unused*/,
                                                                                     jobject lpMediaFileInfo,
                                                                                     jint nFrequency)
    {
        MediaFileInfo mfi = {};
        setMediaFileInfo(env, mfi, lpMediaFileInfo, J2N);
        return TT_DBG_WriteAudioFileTone(&mfi, nFrequency);
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_PlatformHelper_desktopInputKeyTranslate(JNIEnv* env,
                                                                                    jclass /*unused*/,
                                                                                    jint nTranslate,
                                                                                    jobjectArray lpDesktopInputs,
                                                                                    jobjectArray lpTranslatedDesktopInputs)
    {
        if (env->GetArrayLength(lpDesktopInputs) != env->GetArrayLength(lpTranslatedDesktopInputs))
            return -1;

        jsize const len = env->GetArrayLength(lpDesktopInputs);
        std::vector<DesktopInput> inputs(len);
        std::vector<DesktopInput> outputs(len);
        for(jsize i=0;i<len;i++) {
            setDesktopInput(env, inputs[i], env->GetObjectArrayElement(lpDesktopInputs, i), J2N);
        }
        jint const ret = TT_DesktopInput_KeyTranslate(TTKeyTranslate(nTranslate), inputs.data(), outputs.data(), len);
        for (jsize i=0;i<len;++i) {
            setDesktopInput(env, outputs[i], env->GetObjectArrayElement(lpTranslatedDesktopInputs, i), N2J);
        }
        return ret;
    }

    JNIEXPORT jint JNICALL Java_dk_bearware_PlatformHelper_desktopInputExecute(JNIEnv* env,
                                                                               jclass /*unused*/,
                                                                               jobjectArray lpDesktopInputs)
    {
        jsize const len = env->GetArrayLength(lpDesktopInputs);
        std::vector<DesktopInput> inputs(len);
        for(jsize i=0;i<len;i++) {
            setDesktopInput(env, inputs[i], env->GetObjectArrayElement(lpDesktopInputs, i), J2N);
        }
        return TT_DesktopInput_Execute(inputs.data(), len);
    }

//TODO: TT_HotKey_*

JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setUserSoundFilter(JNIEnv* env,
                                                                                   jobject thiz,
                                                                                   jint nUserID,
                                                                                   jstring filter)
    {
        TTInstance* ttInst = GetTTInstance(env, thiz);
        if (!ttInst)
            return JNI_FALSE;

        if (filter == NULL)
            return TT_SetUserSoundFilter(ttInst, nUserID, "");

        return TT_SetUserSoundFilter(ttInst, nUserID, ttstr(env, filter));
    }

JNIEXPORT jboolean JNICALL Java_dk_bearware_TeamTalkBase_setSoundInputFilter(JNIEnv* env,
                                                                                jobject thiz,
                                                                                jstring filter)
    {
        TTInstance* ttInst = GetTTInstance(env, thiz);
        if (!ttInst)
            return JNI_FALSE;

        if (filter == NULL)
            return TT_SetSoundInputFilter(ttInst, "");

        return TT_SetSoundInputFilter(ttInst, ttstr(env, filter));
    }

} //extern "C"
