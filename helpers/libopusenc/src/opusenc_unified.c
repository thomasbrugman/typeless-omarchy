/*
 * opusenc_unified.c -- clean-room re-implementation of the "libopusenc-unified
 * 1.0.0" convenience layer that Typeless bundles inside its libopusenc.so.
 *
 * Behaviour (argument order, struct layouts, return codes, ctl calls, Vorbis
 * comments, output semantics) was reconstructed from the x64 original's
 * machine code (opus_parse_wav, opus_encode_wav, opus_convert_simple,
 * opus_convert_advanced, opus_estimate_output_size, opus_convert_buffer,
 * opus_unified_version) so that the app's koffi declarations work unchanged.
 *
 * Return codes:
 *    0 ok
 *   -1 cannot open input file
 *   -2 invalid WAV header
 *   -3 failed to create encoder
 *   -4 ope_encoder_write failed
 *   -5 output buffer too small (buffer mode)
 *   -6 invalid arguments (buffer mode)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <opus.h>
#include <opusenc.h>

#define UNIFIED_EXPORT __attribute__((visibility("default")))

#define WAV_HEADER_SIZE 44
#define PCM_CHUNK_BYTES 8192 /* 4096 16-bit samples */

typedef struct {
  int sample_rate;
  int channels;
} OpusWavInfo;

typedef struct {
  int sample_rate;        /* 0x00 */
  int channels;           /* 0x04 */
  int bitrate;            /* 0x08 */
  float frame_size_ms;    /* 0x0c */
  int vbr;                /* 0x10 */
  const char *signal;     /* 0x18: "voice" / "music" / "auto" */
} OpusEncodeConfig;       /* size 0x20 */

typedef struct {
  int success;              /* 0x00 */
  int error_code;           /* 0x04 */
  uint64_t input_size;      /* 0x08 */
  uint64_t output_size;     /* 0x10 */
  double encoding_time_ms;  /* 0x18 */
  int actual_bitrate;       /* 0x20 */
  float compression_ratio;  /* 0x24 */
  char error_message[256];  /* 0x28 */
} OpusEncodeResult;         /* size 0x128 */

typedef struct {
  unsigned char *buf;
  uint64_t capacity;
  uint64_t pos;
  int overflow;
} BufferSink;

UNIFIED_EXPORT int opus_parse_wav(const char *path, OpusWavInfo *info);
UNIFIED_EXPORT int opus_encode_wav(const char *in_path, const char *out_path,
                                   const OpusEncodeConfig *cfg, OpusEncodeResult *res);
UNIFIED_EXPORT int opus_convert_simple(const char *in_path, const char *out_path, int bitrate);
UNIFIED_EXPORT int opus_convert_advanced(const char *in_path, const char *out_path, int bitrate,
                                         float frame_size_ms, int vbr, int signal,
                                         double *encoding_time_ms, uint64_t *output_size,
                                         int *actual_bitrate, float *compression_ratio);
UNIFIED_EXPORT uint64_t opus_estimate_output_size(const void *wav, uint64_t wav_len, int bitrate);
UNIFIED_EXPORT int opus_convert_buffer(const void *wav, uint64_t wav_len, int bitrate,
                                       float frame_size_ms, int vbr, int signal,
                                       void *out, uint64_t out_capacity, uint64_t *out_size,
                                       double *encoding_time_ms, int *actual_bitrate,
                                       float *compression_ratio);
UNIFIED_EXPORT const char *opus_unified_version(void);

static double ts_ms(const struct timespec *ts) {
  return (double)ts->tv_sec * 1000.0 + (double)ts->tv_nsec / 1000000.0;
}

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int frame_duration_ctl(float ms) {
  if (ms == 2.5f) return OPUS_FRAMESIZE_2_5_MS;
  if (ms == 5.0f) return OPUS_FRAMESIZE_5_MS;
  if (ms == 10.0f) return OPUS_FRAMESIZE_10_MS;
  if (ms == 20.0f) return OPUS_FRAMESIZE_20_MS;
  if (ms == 40.0f) return OPUS_FRAMESIZE_40_MS;
  if (ms == 60.0f) return OPUS_FRAMESIZE_60_MS;
  return OPUS_FRAMESIZE_20_MS;
}

static int buffer_write_callback(void *user_data, const unsigned char *ptr, opus_int32 len) {
  BufferSink *s = (BufferSink *)user_data;
  if (s->overflow) return 1;
  if (s->pos + (uint64_t)(int64_t)len > s->capacity) {
    s->overflow = 1;
    return 1;
  }
  memcpy(s->buf + s->pos, ptr, (size_t)len);
  s->pos += (uint64_t)(int64_t)len;
  return 0;
}

static int buffer_close_callback(void *user_data) {
  (void)user_data;
  return 0;
}

int opus_parse_wav(const char *path, OpusWavInfo *info) {
  unsigned char h[WAV_HEADER_SIZE];
  FILE *f = fopen(path, "rb");
  size_t n;
  if (!f) return -1;
  n = fread(h, WAV_HEADER_SIZE, 1, f);
  fclose(f);
  if (n != 1) return -2;
  if (strncmp((const char *)h, "RIFF", 4) || strncmp((const char *)h + 8, "WAVE", 4) ||
      strncmp((const char *)h + 12, "fmt ", 4) || strncmp((const char *)h + 36, "data", 4))
    return -2;
  info->sample_rate = (int)rd32(h + 24);
  info->channels = (int)rd16(h + 22);
  return 0;
}

int opus_encode_wav(const char *in_path, const char *out_path,
                    const OpusEncodeConfig *cfg, OpusEncodeResult *res) {
  struct timespec t0, t1;
  unsigned char hdr[WAV_HEADER_SIZE];
  unsigned char pcm[PCM_CHUNK_BYTES];
  char tmp[256];
  FILE *in, *outf;
  OggOpusComments *comments;
  OggOpusEnc *enc;
  int err = 0;
  opus_int32 got_bitrate;
  size_t frames_per_read;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  memset(res, 0, sizeof(*res));

  in = fopen(in_path, "rb");
  if (!in) {
    res->error_code = -1;
    snprintf(res->error_message, sizeof(res->error_message), "Cannot open input file: %s", in_path);
    return res->error_code;
  }
  if (fread(hdr, WAV_HEADER_SIZE, 1, in) != 1) {
    fclose(in);
    res->error_code = -2;
    strcpy(res->error_message, "Invalid WAV header");
    return -2;
  }
  fseek(in, 0, SEEK_END);
  res->input_size = (uint64_t)ftell(in);
  fseek(in, WAV_HEADER_SIZE, SEEK_SET);

  comments = ope_comments_create();
  ope_comments_add(comments, "ENCODER", "libopusenc-unified");
  snprintf(tmp, sizeof(tmp), "%d bps", cfg->bitrate);
  ope_comments_add(comments, "OPUS_BITRATE", tmp);
  snprintf(tmp, sizeof(tmp), "%.1f ms", (double)cfg->frame_size_ms);
  ope_comments_add(comments, "OPUS_FRAME_SIZE", tmp);

  enc = ope_encoder_create_file(out_path, comments, cfg->sample_rate, cfg->channels, 0, &err);
  if (!enc) {
    fclose(in);
    ope_comments_destroy(comments);
    res->error_code = -3;
    snprintf(res->error_message, sizeof(res->error_message), "Failed to create encoder: %s",
             ope_strerror(err));
    return res->error_code;
  }

  ope_encoder_ctl(enc, OPUS_SET_BITRATE(cfg->bitrate));
  ope_encoder_ctl(enc, OPUS_SET_EXPERT_FRAME_DURATION(frame_duration_ctl(cfg->frame_size_ms)));
  if (cfg->vbr) ope_encoder_ctl(enc, OPUS_SET_VBR(1));
  if (cfg->signal) {
    if (strcmp(cfg->signal, "voice") == 0)
      ope_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    else if (strcmp(cfg->signal, "music") == 0)
      ope_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
  }

  frames_per_read = cfg->channels > 0 ? (size_t)(4096 / cfg->channels) : 0;
  while (frames_per_read && !feof(in)) {
    size_t n = fread(pcm, (size_t)cfg->channels * 2, frames_per_read, in);
    if (n == 0) {
      if (ferror(in)) break; /* original would spin forever here */
      continue;
    }
    if (ope_encoder_write(enc, (const opus_int16 *)pcm, (int)n) != 0) {
      res->error_code = -4;
      strcpy(res->error_message, "Failed to write audio data");
      break;
    }
  }

  ope_encoder_drain(enc);
  if (ope_encoder_ctl(enc, OPUS_GET_BITRATE(&got_bitrate)) == 0)
    res->actual_bitrate = got_bitrate;
  else
    res->actual_bitrate = cfg->bitrate;
  ope_encoder_destroy(enc);
  ope_comments_destroy(comments);
  fclose(in);

  outf = fopen(out_path, "rb");
  if (outf) {
    fseek(outf, 0, SEEK_END);
    res->output_size = (uint64_t)ftell(outf);
    fclose(outf);
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  res->encoding_time_ms = ts_ms(&t1) - ts_ms(&t0);
  res->compression_ratio =
      res->input_size ? (float)((double)res->input_size / (double)res->output_size) : 0.0f;
  res->success = (res->error_code == 0);
  return res->error_code;
}

int opus_convert_simple(const char *in_path, const char *out_path, int bitrate) {
  OpusEncodeConfig cfg;
  OpusEncodeResult res;
  int ret;
  memset(&cfg, 0, sizeof(cfg));
  memset(&res, 0, sizeof(res));
  ret = opus_parse_wav(in_path, (OpusWavInfo *)&cfg);
  if (ret != 0) return ret;
  if (bitrate <= 0) bitrate = (cfg.channels == 1) ? 32000 : 64000;
  cfg.bitrate = bitrate;
  cfg.frame_size_ms = 20.0f;
  cfg.vbr = 1;
  cfg.signal = "auto";
  return opus_encode_wav(in_path, out_path, &cfg, &res);
}

int opus_convert_advanced(const char *in_path, const char *out_path, int bitrate,
                          float frame_size_ms, int vbr, int signal,
                          double *encoding_time_ms, uint64_t *output_size,
                          int *actual_bitrate, float *compression_ratio) {
  OpusEncodeConfig cfg;
  OpusEncodeResult res;
  int ret;
  memset(&cfg, 0, sizeof(cfg));
  memset(&res, 0, sizeof(res));
  ret = opus_parse_wav(in_path, (OpusWavInfo *)&cfg);
  if (ret != 0) return ret;
  cfg.bitrate = bitrate;
  cfg.frame_size_ms = frame_size_ms;
  cfg.vbr = vbr;
  cfg.signal = signal == 1 ? "voice" : signal == 2 ? "music" : "auto";
  ret = opus_encode_wav(in_path, out_path, &cfg, &res);
  if (encoding_time_ms) *encoding_time_ms = res.encoding_time_ms;
  if (output_size) *output_size = res.output_size;
  if (actual_bitrate) *actual_bitrate = res.actual_bitrate;
  if (compression_ratio) *compression_ratio = res.compression_ratio;
  return ret;
}

uint64_t opus_estimate_output_size(const void *wav, uint64_t wav_len, int bitrate) {
  const unsigned char *h = (const unsigned char *)wav;
  uint32_t sr, data_size;
  unsigned ch, bps;
  double duration, est;
  uint64_t r;
  if (wav_len < WAV_HEADER_SIZE || bitrate <= 0 || !wav) return 8192;
  sr = rd32(h + 24);
  ch = rd16(h + 22);
  bps = rd16(h + 34);
  if (!sr || !ch || !bps) return 8192;
  data_size = rd32(h + 40);
  duration = (double)data_size / ((double)(int)(ch * bps) * 0.125 * (double)sr);
  est = (double)bitrate * duration * 0.125 * 1.2 + 512.0;
  r = (uint64_t)est;
  return r < 8192 ? 8192 : r;
}

int opus_convert_buffer(const void *wav, uint64_t wav_len, int bitrate,
                        float frame_size_ms, int vbr, int signal,
                        void *out, uint64_t out_capacity, uint64_t *out_size,
                        double *encoding_time_ms, int *actual_bitrate,
                        float *compression_ratio) {
  const unsigned char *src = (const unsigned char *)wav;
  unsigned char h[WAV_HEADER_SIZE];
  unsigned char pcm[PCM_CHUNK_BYTES];
  struct timespec t0, t1;
  BufferSink sink;
  OpusEncCallbacks cbs;
  OggOpusComments *comments;
  OggOpusEnc *enc;
  int err = 0, ret = 0;
  opus_int32 got_bitrate;
  unsigned channels;
  int sample_rate;
  uint64_t bpf, frames_per_chunk, chunk_bytes, data_len, pos;

  if (wav_len < WAV_HEADER_SIZE || !out_size || !out || !wav) return -6;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  memcpy(h, src, WAV_HEADER_SIZE);
  if (strncmp((const char *)h, "RIFF", 4) || strncmp((const char *)h + 8, "WAVE", 4) ||
      strncmp((const char *)h + 12, "fmt ", 4) || strncmp((const char *)h + 36, "data", 4))
    return -2;
  if (rd16(h + 20) != 1) return -2;          /* PCM */
  channels = rd16(h + 22);
  if (!channels) return -2;
  sample_rate = (int)rd32(h + 24);
  if (!sample_rate) return -2;
  if (rd16(h + 34) != 16) return -2;          /* 16-bit only */

  sink.buf = (unsigned char *)out;
  sink.capacity = out_capacity;
  sink.pos = 0;
  sink.overflow = 0;
  cbs.write = buffer_write_callback;
  cbs.close = buffer_close_callback;

  comments = ope_comments_create();
  ope_comments_add(comments, "ENCODER", "libopusenc-unified-buffer");
  enc = ope_encoder_create_callbacks(&cbs, &sink, comments, sample_rate, (int)channels, 0, &err);
  if (!enc) {
    ope_comments_destroy(comments);
    return -3;
  }

  ope_encoder_ctl(enc, OPE_SET_COMMENT_PADDING(0));
  ope_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
  ope_encoder_ctl(enc, OPUS_SET_EXPERT_FRAME_DURATION(frame_duration_ctl(frame_size_ms)));
  ope_encoder_ctl(enc, OPUS_SET_VBR(vbr ? 1 : 0));
  if (signal == 1)
    ope_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  else if (signal == 2)
    ope_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

  bpf = (uint64_t)channels * 2;
  frames_per_chunk = (uint64_t)(4096 / (int)channels);
  chunk_bytes = frames_per_chunk * bpf;
  data_len = wav_len - WAV_HEADER_SIZE; /* data chunk size field is ignored, as in the original */
  pos = 0;
  while (pos < data_len) {
    uint64_t frames, bytes;
    if (data_len - pos < chunk_bytes) {
      frames = (data_len - pos) / bpf;
      bytes = frames * bpf;
    } else {
      frames = frames_per_chunk;
      bytes = chunk_bytes;
    }
    if (frames == 0) break;
    memcpy(pcm, src + WAV_HEADER_SIZE + pos, (size_t)bytes);
    pos += bytes;
    if (ope_encoder_write(enc, (const opus_int16 *)pcm, (int)frames) != 0) {
      ope_encoder_drain(enc);
      ope_encoder_destroy(enc);
      ope_comments_destroy(comments);
      return sink.overflow ? -5 : -4;
    }
  }

  ope_encoder_drain(enc);
  if (ope_encoder_ctl(enc, OPUS_GET_BITRATE(&got_bitrate)) == 0) {
    if (actual_bitrate) *actual_bitrate = got_bitrate;
  } else if (actual_bitrate) {
    *actual_bitrate = bitrate;
  }
  ope_encoder_destroy(enc);
  ope_comments_destroy(comments);
  if (sink.overflow) return -5;

  *out_size = sink.pos;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (encoding_time_ms) *encoding_time_ms = ts_ms(&t1) - ts_ms(&t0);
  if (compression_ratio)
    *compression_ratio = sink.pos ? (float)((double)wav_len / (double)sink.pos) : 0.0f;
  return ret;
}

const char *opus_unified_version(void) { return "libopusenc-unified 1.0.0"; }
