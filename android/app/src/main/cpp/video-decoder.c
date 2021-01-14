// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "video-decoder.h"

#include <jni.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window_jni.h>

#include <string.h>

#define INPUT_BUFFER_TIMEOUT_US 1000 * 1000

static void *android_chiaki_video_decoder_input_thread_func(void *user);
static void *android_chiaki_video_decoder_output_thread_func(void *user);

ChiakiErrorCode android_chiaki_video_decoder_init(AndroidChiakiVideoDecoder *decoder, ChiakiLog *log, int32_t target_width, int32_t target_height, ChiakiCodec codec)
{
	decoder->log = log;
	decoder->codec = NULL;
	decoder->timestamp_cur = 0;
	decoder->target_width = target_width;
	decoder->target_height = target_height;
	decoder->target_codec = codec;
	decoder->shutdown = false;
	decoder->bufs_count = 0;
	ChiakiErrorCode err = chiaki_mutex_init(&decoder->codec_mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	err = chiaki_cond_init(&decoder->bufs_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		chiaki_mutex_fini(&decoder->codec_mutex);
	return err;
}

void android_chiaki_video_decoder_fini(AndroidChiakiVideoDecoder *decoder)
{
	if(decoder->codec)
	{
		chiaki_mutex_lock(&decoder->codec_mutex);
		decoder->shutdown = true;
		ssize_t codec_buf_index = AMediaCodec_dequeueInputBuffer(decoder->codec, INPUT_BUFFER_TIMEOUT_US);
		if(codec_buf_index >= 0)
		{
			CHIAKI_LOGI(decoder->log, "Video Decoder sending EOS buffer");
			AMediaCodec_queueInputBuffer(decoder->codec, (size_t)codec_buf_index, 0, 0, decoder->timestamp_cur++, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
		}
		else
			CHIAKI_LOGE(decoder->log, "Failed to get input buffer for shutting down Video Decoder!");
		AMediaCodec_stop(decoder->codec);
		chiaki_mutex_unlock(&decoder->codec_mutex);
		chiaki_cond_signal(&decoder->bufs_cond);
		chiaki_thread_join(&decoder->output_thread, NULL);
		chiaki_thread_join(&decoder->input_thread, NULL);
		AMediaCodec_delete(decoder->codec);
	}
	chiaki_mutex_fini(&decoder->codec_mutex);
}

void android_chiaki_video_decoder_set_surface(AndroidChiakiVideoDecoder *decoder, JNIEnv *env, jobject surface)
{
	chiaki_mutex_lock(&decoder->codec_mutex);

	if(decoder->codec)
	{
#if __ANDROID_API__ >= 23
		CHIAKI_LOGI(decoder->log, "Video decoder already initialized, swapping surface");
		ANativeWindow *new_window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
		AMediaCodec_setOutputSurface(decoder->codec, new_window);
		ANativeWindow_release(decoder->window);
		decoder->window = new_window;
#else
		CHIAKI_LOGE(decoder->log, "Video Decoder already initialized");
#endif
		goto beach;
	}

	decoder->window = ANativeWindow_fromSurface(env, surface);

	const char *mime = chiaki_codec_is_h265(decoder->target_codec) ? "video/hevc" : "video/avc";
	CHIAKI_LOGI(decoder->log, "Initializing decoder with mime %s", mime);

	decoder->codec = AMediaCodec_createDecoderByType(mime);
	if(!decoder->codec)
	{
		CHIAKI_LOGE(decoder->log, "Failed to create AMediaCodec for mime type %s", mime);
		goto error_surface;
	}

	AMediaFormat *format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, decoder->target_width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, decoder->target_height);

	media_status_t r = AMediaCodec_configure(decoder->codec, format, decoder->window, NULL, 0);
	if(r != AMEDIA_OK)
	{
		CHIAKI_LOGE(decoder->log, "AMediaCodec_configure() failed: %d", (int)r);
		AMediaFormat_delete(format);
		goto error_codec;
	}

	r = AMediaCodec_start(decoder->codec);
	AMediaFormat_delete(format);
	if(r != AMEDIA_OK)
	{
		CHIAKI_LOGE(decoder->log, "AMediaCodec_start() failed: %d", (int)r);
		goto error_codec;
	}

	ChiakiErrorCode err = chiaki_thread_create(&decoder->input_thread, android_chiaki_video_decoder_input_thread_func, decoder);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(decoder->log, "Failed to create input thread for AMediaCodec");
		goto error_codec;
	}

	err = chiaki_thread_create(&decoder->output_thread, android_chiaki_video_decoder_output_thread_func, decoder);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(decoder->log, "Failed to create output thread for AMediaCodec");
		goto error_input_thread;
	}

	goto beach;

error_input_thread:
	decoder->shutdown = true;

error_codec:
	AMediaCodec_delete(decoder->codec);
	decoder->codec = NULL;

error_surface:
	ANativeWindow_release(decoder->window);
	decoder->window = NULL;

beach:
	chiaki_mutex_unlock(&decoder->codec_mutex);
}

bool android_chiaki_video_decoder_video_sample(uint8_t *buf, size_t buf_size, void *user)
{
	bool r = false;
	AndroidChiakiVideoDecoder *decoder = user;
	chiaki_mutex_lock(&decoder->codec_mutex);
	if(decoder->bufs_count >= ANDROID_CHIAKI_VIDEO_DECODER_FRAME_BUFFER_SIZE)
	{
		CHIAKI_LOGE(decoder->log, "All bufs full in video decoder");
		goto beach;
	}
	uint8_t *buf_copy = malloc(buf_size);
	if(!buf_copy)
		goto beach;
	memcpy(buf_copy, buf, buf_size);
	decoder->bufs[decoder->bufs_count] = buf_copy;
	decoder->bufs_sizes[decoder->bufs_count++] = buf_size;
	chiaki_cond_signal(&decoder->bufs_cond);
	r = true;
beach:
	chiaki_mutex_unlock(&decoder->codec_mutex);
	return r;
}

static void *android_chiaki_video_decoder_input_thread_func(void *user)
{
	AndroidChiakiVideoDecoder *decoder = user;
	chiaki_mutex_lock(&decoder->codec_mutex);

	if(!decoder->codec) // special case when init of the output thread fails but creating input thread succeeds
		goto beach;

	while(1)
	{
		while(!decoder->shutdown && !decoder->bufs_count)
			chiaki_cond_wait(&decoder->bufs_cond, &decoder->codec_mutex);
		if(decoder->shutdown)
			break;
		uint8_t *buf_start = decoder->bufs[0];
		size_t buf_size = decoder->bufs_sizes[0];
		for(size_t i = 0; i < decoder->bufs_count - 1; i++)
		{
			decoder->bufs[i] = decoder->bufs[i + 1];
			decoder->bufs_sizes[i] = decoder->bufs_sizes[i + 1];
		}
		decoder->bufs_count--;
		uint64_t timestamp = decoder->timestamp_cur++; // timestamp just raised by 1 for maximum realtime

		chiaki_mutex_unlock(&decoder->codec_mutex); // unlock here because below code can block
		uint8_t *buf = buf_start;
		while(buf_size > 0)
		{
			ssize_t codec_buf_index = AMediaCodec_dequeueInputBuffer(decoder->codec, INPUT_BUFFER_TIMEOUT_US);
			if(codec_buf_index < 0)
			{
				CHIAKI_LOGE(decoder->log, "Failed to get input buffer: %d", (int)codec_buf_index);
				break;
				// TODO: report somehow that there was a broken frame
			}
			size_t codec_buf_size;
			uint8_t *codec_buf = AMediaCodec_getInputBuffer(decoder->codec, (size_t)codec_buf_index, &codec_buf_size);
			size_t codec_sample_size = buf_size;
			if(codec_sample_size > codec_buf_size)
			{
				//CHIAKI_LOGD(decoder->log, "Sample is bigger than buffer, splitting");
				codec_sample_size = codec_buf_size;
			}
			memcpy(codec_buf, buf, codec_sample_size);
			media_status_t r = AMediaCodec_queueInputBuffer(decoder->codec, (size_t)codec_buf_index, 0, codec_sample_size, timestamp, 0);
			if(r != AMEDIA_OK)
			{
				CHIAKI_LOGE(decoder->log, "AMediaCodec_queueInputBuffer() failed: %d", (int)r);
				// TODO: report here too
			}
			buf += codec_sample_size;
			buf_size -= codec_sample_size;
		}
		free(buf_start);
		chiaki_mutex_lock(&decoder->codec_mutex);
	}

beach:
	chiaki_mutex_unlock(&decoder->codec_mutex);
	CHIAKI_LOGI(decoder->log, "Video Decoder Input Thread exiting");
	return NULL;
}

static void *android_chiaki_video_decoder_output_thread_func(void *user)
{
	AndroidChiakiVideoDecoder *decoder = user;

	while(1)
	{
		AMediaCodecBufferInfo info;
		ssize_t status = AMediaCodec_dequeueOutputBuffer(decoder->codec, &info, 100000);
		if(status >= 0)
		{
			AMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)status, info.size != 0);
			if(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
			{
				CHIAKI_LOGI(decoder->log, "AMediaCodec reported EOS");
				break;
			}
		}
		else
		{
			chiaki_mutex_lock(&decoder->codec_mutex);
			bool shutdown = decoder->shutdown;
			chiaki_mutex_unlock(&decoder->codec_mutex);
			if(shutdown)
			{
				CHIAKI_LOGI(decoder->log, "Video Decoder Output Thread detected shutdown after reported error");
				break;
			}
			else if(status != AMEDIACODEC_INFO_TRY_AGAIN_LATER)
				CHIAKI_LOGE(decoder->log, "Video Decoder Output dequeue error: %d", (int)status);
		}
	}

	CHIAKI_LOGI(decoder->log, "Video Decoder Output Thread exiting");

	return NULL;
}