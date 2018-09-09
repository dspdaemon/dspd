#ifndef _DSPD_PCM_H_
#define _DSPD_PCM_H_
#include <stdbool.h>
#define INT24_MAX 8388607
#define INT24_MIN -8388608
#define UINT24_MAX 16777215
#define UINT18_MAX (1 << 18)
#define UINT20_MAX (1 << 20)
#define INT20_MAX (INT32_MAX / (UINT32_MAX / UINT20_MAX))
#define INT18_MAX (INT32_MAX / (UINT32_MAX / UINT18_MAX))



typedef enum _dspd_pcm_format {
	/** Unknown */
	DSPD_PCM_FORMAT_UNKNOWN = -1,
	/** Signed 8 bit */
	DSPD_PCM_FORMAT_S8 = 0,
	/** Unsigned 8 bit */
	DSPD_PCM_FORMAT_U8,
	/** Signed 16 bit Little Endian */
	DSPD_PCM_FORMAT_S16_LE,
	/** Signed 16 bit Big Endian */
	DSPD_PCM_FORMAT_S16_BE,
	/** Unsigned 16 bit Little Endian */
	DSPD_PCM_FORMAT_U16_LE,
	/** Unsigned 16 bit Big Endian */
	DSPD_PCM_FORMAT_U16_BE,
	/** Signed 24 bit Little Endian using low three bytes in 32-bit word */
	DSPD_PCM_FORMAT_S24_LE,
	/** Signed 24 bit Big Endian using low three bytes in 32-bit word */
	DSPD_PCM_FORMAT_S24_BE,
	/** Unsigned 24 bit Little Endian using low three bytes in 32-bit word */
	DSPD_PCM_FORMAT_U24_LE,
	/** Unsigned 24 bit Big Endian using low three bytes in 32-bit word */
	DSPD_PCM_FORMAT_U24_BE,
	/** Signed 32 bit Little Endian */
	DSPD_PCM_FORMAT_S32_LE,
	/** Signed 32 bit Big Endian */
	DSPD_PCM_FORMAT_S32_BE,
	/** Unsigned 32 bit Little Endian */
	DSPD_PCM_FORMAT_U32_LE,
	/** Unsigned 32 bit Big Endian */
	DSPD_PCM_FORMAT_U32_BE,
	/** Float 32 bit Little Endian, Range -1.0 to 1.0 */
	DSPD_PCM_FORMAT_FLOAT_LE,
	/** Float 32 bit Big Endian, Range -1.0 to 1.0 */
	DSPD_PCM_FORMAT_FLOAT_BE,
	/** Float 64 bit Little Endian, Range -1.0 to 1.0 */
	DSPD_PCM_FORMAT_FLOAT64_LE,
	/** Float 64 bit Big Endian, Range -1.0 to 1.0 */
	DSPD_PCM_FORMAT_FLOAT64_BE,
	/** IEC-958 Little Endian */
	DSPD_PCM_FORMAT_IEC958_SUBFRAME_LE,
	/** IEC-958 Big Endian */
	DSPD_PCM_FORMAT_IEC958_SUBFRAME_BE,
	/** Mu-Law */
	DSPD_PCM_FORMAT_MU_LAW,
	/** A-Law */
	DSPD_PCM_FORMAT_A_LAW,
	/** Ima-ADPCM */
	DSPD_PCM_FORMAT_IMA_ADPCM,
	/** MPEG */
	DSPD_PCM_FORMAT_MPEG,
	/** GSM */
	DSPD_PCM_FORMAT_GSM,
	/** Special */
	DSPD_PCM_FORMAT_SPECIAL = 31,
	/** Signed 24bit Little Endian in 3bytes format */
	DSPD_PCM_FORMAT_S24_3LE = 32,
	/** Signed 24bit Big Endian in 3bytes format */
	DSPD_PCM_FORMAT_S24_3BE,
	/** Unsigned 24bit Little Endian in 3bytes format */
	DSPD_PCM_FORMAT_U24_3LE,
	/** Unsigned 24bit Big Endian in 3bytes format */
	DSPD_PCM_FORMAT_U24_3BE,
	/** Signed 20bit Little Endian in 3bytes format */
	DSPD_PCM_FORMAT_S20_3LE,
	/** Signed 20bit Big Endian in 3bytes format */
	DSPD_PCM_FORMAT_S20_3BE,
	/** Unsigned 20bit Little Endian in 3bytes format */
	DSPD_PCM_FORMAT_U20_3LE,
	/** Unsigned 20bit Big Endian in 3bytes format */
	DSPD_PCM_FORMAT_U20_3BE,
	/** Signed 18bit Little Endian in 3bytes format */
	DSPD_PCM_FORMAT_S18_3LE,
	/** Signed 18bit Big Endian in 3bytes format */
	DSPD_PCM_FORMAT_S18_3BE,
	/** Unsigned 18bit Little Endian in 3bytes format */
	DSPD_PCM_FORMAT_U18_3LE,
	/** Unsigned 18bit Big Endian in 3bytes format */
	DSPD_PCM_FORMAT_U18_3BE,
	/* G.723 (ADPCM) 24 kbit/s, 8 samples in 3 bytes */
	DSPD_PCM_FORMAT_G723_24,
	/* G.723 (ADPCM) 24 kbit/s, 1 sample in 1 byte */
	DSPD_PCM_FORMAT_G723_24_1B,
	/* G.723 (ADPCM) 40 kbit/s, 8 samples in 3 bytes */
	DSPD_PCM_FORMAT_G723_40,
	/* G.723 (ADPCM) 40 kbit/s, 1 sample in 1 byte */
	DSPD_PCM_FORMAT_G723_40_1B,
	/* Direct Stream Digital (DSD) in 1-byte samples (x8) */
	DSPD_PCM_FORMAT_DSD_U8,
	/* Direct Stream Digital (DSD) in 2-byte samples (x16) */
	DSPD_PCM_FORMAT_DSD_U16_LE,
	DSPD_PCM_FORMAT_LAST = DSPD_PCM_FORMAT_DSD_U16_LE,

#if __BYTE_ORDER == __LITTLE_ENDIAN
	/** Signed 16 bit CPU endian */
	DSPD_PCM_FORMAT_S16_NE = DSPD_PCM_FORMAT_S16_LE,
	/** Unsigned 16 bit CPU endian */
	DSPD_PCM_FORMAT_U16_NE = DSPD_PCM_FORMAT_U16_LE,
	/** Signed 24 bit CPU endian */
	DSPD_PCM_FORMAT_S24_NE = DSPD_PCM_FORMAT_S24_LE,
	/** Unsigned 24 bit CPU endian */
	DSPD_PCM_FORMAT_U24_NE = DSPD_PCM_FORMAT_U24_LE,
	/** Signed 32 bit CPU endian */
	DSPD_PCM_FORMAT_S32_NE = DSPD_PCM_FORMAT_S32_LE,
	/** Unsigned 32 bit CPU endian */
	DSPD_PCM_FORMAT_U32_NE = DSPD_PCM_FORMAT_U32_LE,
	/** Float 32 bit CPU endian */
	DSPD_PCM_FORMAT_FLOAT_NE = DSPD_PCM_FORMAT_FLOAT_LE,
	/** Float 64 bit CPU endian */
	DSPD_PCM_FORMAT_FLOAT64_NE = DSPD_PCM_FORMAT_FLOAT64_LE,
	/** IEC-958 CPU Endian */
	DSPD_PCM_FORMAT_IEC958_SUBFRAME_NE = DSPD_PCM_FORMAT_IEC958_SUBFRAME_LE,

	DSPD_PCM_FORMAT_S24_3NE = DSPD_PCM_FORMAT_S24_3LE,
	DSPD_PCM_FORMAT_S24_3OE = DSPD_PCM_FORMAT_S24_3BE,

	/** Signed 16 bit CPU endian */
	DSPD_PCM_FORMAT_S16_OE = DSPD_PCM_FORMAT_S16_BE,
	/** Unsigned 16 bit CPU endian */
	DSPD_PCM_FORMAT_U16_OE = DSPD_PCM_FORMAT_U16_BE,
	/** Signed 24 bit CPU endian */
	DSPD_PCM_FORMAT_S24_OE = DSPD_PCM_FORMAT_S24_BE,
	/** Unsigned 24 bit CPU endian */
	DSPD_PCM_FORMAT_U24_OE = DSPD_PCM_FORMAT_U24_BE,
	/** Signed 32 bit CPU endian */
	DSPD_PCM_FORMAT_S32_OE = DSPD_PCM_FORMAT_S32_BE,
	/** Unsigned 32 bit CPU endian */
	DSPD_PCM_FORMAT_U32_OE = DSPD_PCM_FORMAT_U32_BE,
	/** Float 32 bit CPU endian */
	DSPD_PCM_FORMAT_FLOAT_OE = DSPD_PCM_FORMAT_FLOAT_BE,
	/** Float 64 bit CPU endian */
	DSPD_PCM_FORMAT_FLOAT64_OE = DSPD_PCM_FORMAT_FLOAT64_BE,
	/** IEC-958 CPU Endian */
	DSPD_PCM_FORMAT_IEC958_SUBFRAME_OE = DSPD_PCM_FORMAT_IEC958_SUBFRAME_BE

#elif __BYTE_ORDER == __BIG_ENDIAN
	/** Signed 16 bit CPU endian */
	DSPD_PCM_FORMAT_S16_NE = DSPD_PCM_FORMAT_S16_BE,
	/** Unsigned 16 bit CPU endian */
	DSPD_PCM_FORMAT_U16_NE = DSPD_PCM_FORMAT_U16_BE,
	/** Signed 24 bit CPU endian */
	DSPD_PCM_FORMAT_S24_NE = DSPD_PCM_FORMAT_S24_BE,
	/** Unsigned 24 bit CPU endian */
	DSPD_PCM_FORMAT_U24_NE = DSPD_PCM_FORMAT_U24_BE,
	/** Signed 32 bit CPU endian */
	DSPD_PCM_FORMAT_S32_NE = DSPD_PCM_FORMAT_S32_BE,
	/** Unsigned 32 bit CPU endian */
	DSPD_PCM_FORMAT_U32_NE = DSPD_PCM_FORMAT_U32_BE,
	/** Float 32 bit CPU endian */
	DSPD_PCM_FORMAT_FLOAT_NE = DSPD_PCM_FORMAT_FLOAT_BE,
	/** Float 64 bit CPU endian */
	DSPD_PCM_FORMAT_FLOAT64_NE = DSPD_PCM_FORMAT_FLOAT64_BE,
	/** IEC-958 CPU Endian */
	DSPD_PCM_FORMAT_IEC958_SUBFRAME_NE = DSPD_PCM_FORMAT_IEC958_SUBFRAME_BE

	DSPD_PCM_FORMAT_S24_3NE = DSPD_PCM_FORMAT_S24_3BE,
	DSPD_PCM_FORMAT_S24_3OE = DSPD_PCM_FORMAT_S24_3LE,

	/** Signed 16 bit CPU endian */
	DSPD_PCM_FORMAT_S16_OE = DSPD_PCM_FORMAT_S16_LE,
	/** Unsigned 16 bit CPU endian */
	DSPD_PCM_FORMAT_U16_OE = DSPD_PCM_FORMAT_U16_LE,
	/** Signed 24 bit CPU endian */
	DSPD_PCM_FORMAT_S24_OE = DSPD_PCM_FORMAT_S24_LE,
	/** Unsigned 24 bit CPU endian */
	DSPD_PCM_FORMAT_U24_OE = DSPD_PCM_FORMAT_U24_LE,
	/** Signed 32 bit CPU endian */
	DSPD_PCM_FORMAT_S32_OE = DSPD_PCM_FORMAT_S32_LE,
	/** Unsigned 32 bit CPU endian */
	DSPD_PCM_FORMAT_U32_OE = DSPD_PCM_FORMAT_U32_LE,
	/** Float 32 bit CPU endian */
	DSPD_PCM_FORMAT_FLOAT_OE = DSPD_PCM_FORMAT_FLOAT_LE,
	/** Float 64 bit CPU endian */
	DSPD_PCM_FORMAT_FLOAT64_OE = DSPD_PCM_FORMAT_FLOAT64_LE,
	/** IEC-958 CPU Endian */
	DSPD_PCM_FORMAT_IEC958_SUBFRAME_OE = DSPD_PCM_FORMAT_IEC958_SUBFRAME_LE
	
#else
#error "Unknown endian"
#endif
} dspd_pcm_format_t;

typedef enum _dspd_pcm_state {
	/** Open */
  DSPD_PCM_STATE_OPEN = 0,
	/** Setup installed */ 
  DSPD_PCM_STATE_SETUP,
	/** Ready to start */
  DSPD_PCM_STATE_PREPARED,
	/** Running */
  DSPD_PCM_STATE_RUNNING,
	/** Stopped: underrun (playback) or overrun (capture) detected */
  DSPD_PCM_STATE_XRUN,
	/** Draining: running (playback) or stopped (capture) */
  DSPD_PCM_STATE_DRAINING,
	/** Paused */
  DSPD_PCM_STATE_PAUSED,
	/** Hardware is suspended */
  DSPD_PCM_STATE_SUSPENDED,
	/** Hardware is disconnected */
  DSPD_PCM_STATE_DISCONNECTED,
  DSPD_PCM_STATE_LAST = DSPD_PCM_STATE_DISCONNECTED
} dspd_pcm_state_t;

typedef float  float32;
typedef double float64;

//The pointers are not supposed to overlap.  This could serve as a hint to static analyzers.
#define _RESTRICT __restrict

typedef void (*dspd_tofloat32_t)(const void * _RESTRICT in, float32 * _RESTRICT out, size_t len);
typedef void (*dspd_tofloat32wv_t)(const void * _RESTRICT in, float32 * _RESTRICT out, size_t len, float64 volume);
typedef void (*dspd_tofloat64_t)(const void * _RESTRICT in, float64 * _RESTRICT out, size_t len);
typedef void (*dspd_tofloat64wv_t)(const void * _RESTRICT in, float64 * _RESTRICT out, size_t len, float64 volume);

typedef void (*dspd_fromfloat32_t)(const float32 * _RESTRICT in, void * _RESTRICT out, size_t len);
typedef void (*dspd_fromfloat32wv_t)(const float32 * _RESTRICT in, void * _RESTRICT out, size_t len, float64 volume);
typedef void (*dspd_fromfloat64_t)(const float64 * _RESTRICT in, void * _RESTRICT out, size_t len);
typedef void (*dspd_fromfloat64wv_t)(const float64 * _RESTRICT in, void * _RESTRICT out, size_t len, float64 volume);

struct pcm_conv {
  dspd_tofloat32_t     tofloat32;
  dspd_tofloat64_t     tofloat64;
  dspd_tofloat64wv_t   tofloat64wv;
  dspd_tofloat32wv_t   tofloat32wv;
  dspd_fromfloat32_t   fromfloat32;
  dspd_fromfloat64_t   fromfloat64;
  dspd_fromfloat32wv_t fromfloat32wv;
  dspd_fromfloat64wv_t fromfloat64wv;
};

#define DSPD_PCM_STREAM_PLAYBACK 0
#define DSPD_PCM_STREAM_CAPTURE  1
#define DSPD_PCM_STREAM_FULLDUPLEX 3
#define DSPD_PCM_STREAM_CTL 4
#define DSPD_PCM_SBIT(_b) (1<<(_b))
#define DSPD_PCM_SBIT_TRIGGER_BOTH (1<<5)
#define DSPD_PCM_SBIT_PLAYBACK DSPD_PCM_SBIT(DSPD_PCM_STREAM_PLAYBACK)
#define DSPD_PCM_SBIT_CAPTURE DSPD_PCM_SBIT(DSPD_PCM_STREAM_CAPTURE)
#define DSPD_PCM_SBIT_FULLDUPLEX (DSPD_PCM_SBIT_CAPTURE|DSPD_PCM_SBIT_PLAYBACK)
#define DSPD_PCM_SBIT_CTL DSPD_PCM_SBIT(DSPD_PCM_STREAM_CTL)
const struct pcm_conv *dspd_getconv(int format);
size_t dspd_get_pcm_format_size(int format);
int dspd_pcm_build_format(unsigned int bits, unsigned int length, unsigned int usig, unsigned int big_endian, bool isfloat);
bool dspd_pcm_format_info(int format, unsigned int *bits, unsigned int *length, unsigned int *usig, unsigned int *big_endian, bool *isfloat);


bool dspd_pcm_format_is_integer(int format);

int32_t dspd_pcm_format_from_name(const char *name);
const char *dspd_pcm_name_from_format(int32_t format);
const char *dspd_pcm_stream_bit_name(int32_t sbits);
ssize_t dspd_pcm_fill_silence(int32_t format, void *addr, size_t samples);
#endif
