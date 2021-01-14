// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_JNI_VIDEO_DECODER_H
#define CHIAKI_JNI_VIDEO_DECODER_H

#include <jni.h>

#include <chiaki/thread.h>
#include <chiaki/log.h>

typedef struct AMediaCodec AMediaCodec;
typedef struct ANativeWindow ANativeWindow;

#define ANDROID_CHIAKI_VIDEO_DECODER_FRAME_BUFFER_SIZE 4

typedef struct android_chiaki_video_decoder_t
{
	ChiakiLog *log;
	ChiakiMutex codec_mutex;
	uint8_t *bufs[ANDROID_CHIAKI_VIDEO_DECODER_FRAME_BUFFER_SIZE];
	size_t bufs_sizes[ANDROID_CHIAKI_VIDEO_DECODER_FRAME_BUFFER_SIZE];
	size_t bufs_count;
	ChiakiCond bufs_cond;
	AMediaCodec *codec;
	ANativeWindow *window;
	uint64_t timestamp_cur;
	ChiakiThread input_thread;
	ChiakiThread output_thread;
	bool shutdown;
	int32_t target_width;
	int32_t target_height;
	ChiakiCodec target_codec;
} AndroidChiakiVideoDecoder;

ChiakiErrorCode android_chiaki_video_decoder_init(AndroidChiakiVideoDecoder *decoder, ChiakiLog *log, int32_t target_width, int32_t target_height, ChiakiCodec codec);
void android_chiaki_video_decoder_fini(AndroidChiakiVideoDecoder *decoder);
void android_chiaki_video_decoder_set_surface(AndroidChiakiVideoDecoder *decoder, JNIEnv *env, jobject surface);
bool android_chiaki_video_decoder_video_sample(uint8_t *buf, size_t buf_size, void *user);

#endif