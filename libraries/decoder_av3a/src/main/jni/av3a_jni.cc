/*
 * Dedicated AV3A JNI bridge. This file intentionally has no FFmpeg includes or calls.
 *
 * 与 sggc/media (codec-support) 分支原版的差异：
 *   + av3aSetWorkDir()：由 Java 传入 app filesDir，av3aInit() 前 chdir。
 *     原因：底层 libavs3a_decoder 内部 Avs3InitDecoder(..., "model.bin")
 *     是【相对路径】fopen。mpv 路径靠 FFmpeg wrapper av3a_dec.c 的
 *     av3a_ensure_model_bin() 兜底 chdir；ExoPlayer 路径没有 wrapper，
 *     纯 exo 会话 CWD="/" 必然找不到 model.bin。chdir 是进程级的，
 *     即使之前 mpv 播过也无害（幂等）。
 */
#include <jni.h>
#include <android/log.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

extern "C" {
#include "avs3_decoder_interface.h"
}

#define LOG_TAG "Av3aJNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kInvalidData = -1;
constexpr int kOtherError = -2;
constexpr int kMinimumOutputCapacity = 64 * 1024;

struct DecoderContext {
  AVS3DecoderHandle decoder = nullptr;
  bool first_frame = true;
};

std::string g_work_dir;

bool CreateDecoder(DecoderContext* context) {
  // model.bin 相对路径依赖：初始化前确保 CWD 已切到 Java 指定的目录
  if (!g_work_dir.empty()) {
    if (chdir(g_work_dir.c_str()) != 0) {
      LOGE("chdir(%s) failed: %s", g_work_dir.c_str(), strerror(errno));
    } else {
      LOGI("CWD switched to %s (model.bin reachable)", g_work_dir.c_str());
    }
  }
  context->decoder = avs3_create_decoder();
  context->first_frame = true;
  return context->decoder != nullptr;
}

void DestroyDecoder(DecoderContext* context) {
  if (context->decoder != nullptr) {
    avs3_destroy_decoder(context->decoder);
    context->decoder = nullptr;
  }
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_androidx_media3_decoder_av3a_Av3aLibrary_av3aGetVersion(JNIEnv* env, jclass) {
  return env->NewStringUTF("VividLib-10a5c714-direct");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_androidx_media3_decoder_av3a_Av3aLibrary_av3aSetWorkDir(JNIEnv* env, jclass,
                                                             jstring path) {
  if (path == nullptr) {
    return JNI_FALSE;
  }
  const char* chars = env->GetStringUTFChars(path, nullptr);
  if (chars == nullptr) {
    return JNI_FALSE;
  }
  g_work_dir = chars;
  env->ReleaseStringUTFChars(path, chars);
  LOGI("work dir set to %s", g_work_dir.c_str());
  return JNI_TRUE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_androidx_media3_decoder_av3a_Av3aDecoder_av3aInit(JNIEnv*, jclass) {
  auto* context = new (std::nothrow) DecoderContext();
  if (context == nullptr || !CreateDecoder(context)) {
    delete context;
    return 0;
  }
  LOGI("Created direct AV3A decoder");
  return reinterpret_cast<jlong>(context);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_androidx_media3_decoder_av3a_Av3aDecoder_av3aReset(JNIEnv*, jclass, jlong handle) {
  auto* context = reinterpret_cast<DecoderContext*>(handle);
  if (context == nullptr) {
    return JNI_FALSE;
  }
  DestroyDecoder(context);
  return CreateDecoder(context) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_av3a_Av3aDecoder_av3aDecode(
    JNIEnv* env,
    jclass,
    jlong handle,
    jobject input_buffer,
    jint input_size,
    jobject output_buffer,
    jint output_size) {
  auto* context = reinterpret_cast<DecoderContext*>(handle);
  auto* input = static_cast<unsigned char*>(env->GetDirectBufferAddress(input_buffer));
  auto* output = static_cast<unsigned char*>(env->GetDirectBufferAddress(output_buffer));
  if (context == nullptr || context->decoder == nullptr || input == nullptr || output == nullptr
      || input_size <= 0 || output_size < kMinimumOutputCapacity) {
    return kOtherError;
  }

  int header_consumed = 0;
  int result =
      parse_header(
          context->decoder,
          input,
          input_size,
          context->first_frame ? 1 : 0,
          &header_consumed,
          nullptr);
  if (result != AVS3_TRUE || header_consumed <= 0 || header_consumed >= input_size) {
    return kInvalidData;
  }

  int output_bytes = 0;
  int payload_consumed = 0;
  result =
      avs3_decode(
          context->decoder,
          input + header_consumed,
          input_size - header_consumed,
          output,
          &output_bytes,
          &payload_consumed);
  if (result != AVS3_TRUE || output_bytes <= 0) {
    LOGE("Decode failed: result=%d input=%d header=%d payload=%d", result, input_size,
         header_consumed, payload_consumed);
    return result == AVS3_DATA_NOT_ENOUGH ? kInvalidData : kOtherError;
  }
  if (output_bytes > output_size) {
    LOGE("Decoder output overflow: %d > %d", output_bytes, output_size);
    return kOtherError;
  }
  context->first_frame = false;
  return output_bytes;
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_av3a_Av3aDecoder_av3aGetChannelCount(
    JNIEnv*, jclass, jlong handle) {
  auto* context = reinterpret_cast<DecoderContext*>(handle);
  return context != nullptr && context->decoder != nullptr
      ? context->decoder->numChansOutput
      : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_androidx_media3_decoder_av3a_Av3aDecoder_av3aGetSampleRate(
    JNIEnv*, jclass, jlong handle) {
  auto* context = reinterpret_cast<DecoderContext*>(handle);
  return context != nullptr && context->decoder != nullptr ? context->decoder->outputFs : 0;
}

extern "C" JNIEXPORT void JNICALL
Java_androidx_media3_decoder_av3a_Av3aDecoder_av3aRelease(
    JNIEnv*, jclass, jlong handle) {
  auto* context = reinterpret_cast<DecoderContext*>(handle);
  if (context == nullptr) {
    return;
  }
  DestroyDecoder(context);
  delete context;
}
