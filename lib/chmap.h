#ifndef _DSPD_CHMAP_H_
#define _DSPD_CHMAP_H_
#include <sys/types.h>

enum dspd_pcm_chmap_positions {
  	DSPD_CHMAP_UNKNOWN = 0,	/**< unspecified */
	DSPD_CHMAP_NA,		/**< N/A, silent */
	DSPD_CHMAP_MONO,		/**< mono stream */
	DSPD_CHMAP_FL,		/**< front left */
	DSPD_CHMAP_FR,		/**< front right */
	DSPD_CHMAP_RL,		/**< rear left */
	DSPD_CHMAP_RR,		/**< rear right */
	DSPD_CHMAP_FC,		/**< front center */
	DSPD_CHMAP_LFE,		/**< LFE */
	DSPD_CHMAP_SL,		/**< side left */
	DSPD_CHMAP_SR,		/**< side right */
	DSPD_CHMAP_RC,		/**< rear center */
	DSPD_CHMAP_FLC,		/**< front left center */
	DSPD_CHMAP_FRC,		/**< front right center */
	DSPD_CHMAP_RLC,		/**< rear left center */
	DSPD_CHMAP_RRC,		/**< rear right center */
	DSPD_CHMAP_FLW,		/**< front left wide */
	DSPD_CHMAP_FRW,		/**< front right wide */
	DSPD_CHMAP_FLH,		/**< front left high */
	DSPD_CHMAP_FCH,		/**< front center high */
	DSPD_CHMAP_FRH,		/**< front right high */
	DSPD_CHMAP_TC,		/**< top center */
	DSPD_CHMAP_TFL,		/**< top front left */
	DSPD_CHMAP_TFR,		/**< top front right */
	DSPD_CHMAP_TFC,		/**< top front center */
	DSPD_CHMAP_TRL,		/**< top rear left */
	DSPD_CHMAP_TRR,		/**< top rear right */
	DSPD_CHMAP_TRC,		/**< top rear center */
	DSPD_CHMAP_TFLC,		/**< top front left center */
	DSPD_CHMAP_TFRC,		/**< top front right center */
	DSPD_CHMAP_TSL,		/**< top side left */
	DSPD_CHMAP_TSR,		/**< top side right */
	DSPD_CHMAP_LLFE,		/**< left LFE */
	DSPD_CHMAP_RLFE,		/**< right LFE */
	DSPD_CHMAP_BC,		/**< bottom center */
	DSPD_CHMAP_BLC,		/**< bottom left center */
	DSPD_CHMAP_BRC,		/**< bottom right center */
	DSPD_CHMAP_LAST = DSPD_CHMAP_BRC,

};



#define DSPD_CHMAP_MULTI (1<<7)

/*
  A channel map can really have 3 meanings:

  STANDARD: Each position (pos[]) is an index into the input stream and the integer
            in that position is the output index.
  MULTI: Interleaved IN,OUT,IN,OUT,... (map input channel to an output channel, possibly
         more than once per channel)
  DEVICE MAP: Each position index represents an index in a device PCM frame.  The
              integer in that index is the actual channel (DSPD_CHMAP_xxx)
  
  

 */

struct dspd_chmap {
  uint8_t channels;
  uint8_t stream;
  uint8_t pos[0];
};



/*Full sized channel map.  This is the most space that a channel map will ever need.*/
struct dspd_fchmap {
  struct dspd_chmap map;
  uint8_t pos[(DSPD_CHMAP_LAST+1)*2UL];
};
#define DSPD_CHMAP_MAXSIZE sizeof(struct dspd_fchmap)
#define DSPD_CHMAP_MAXCHAN (DSPD_CHMAP_LAST-DSPD_CHMAP_FL+1)


//Map is a matrix (each position is an index, not an enumerated position)
#define DSPD_CHMAP_MATRIX (1U<<6U)
#define DSPD_CHMAP_SIMPLE (1U<<5U)
#define DSPD_CHMAP_POSITION_MASK 0xFFFFU
#define DSPD_CHMAP_DRIVER_SPEC (1U << 8U)
#define DSPD_CHMAP_PHASE_INVERSE (1U << 9U)



//standard channel map
struct dspd_pcm_chmap {
  uint16_t ichan; //Input channels (matrix)
  uint16_t ochan; //Output channels (matrix)
  uint16_t count; //size of pos[]
  uint16_t flags;
  uint32_t pos[0];
};


struct dspd_pcm_chmap_container {
  struct dspd_pcm_chmap map;
  uint32_t              pos[(DSPD_CHMAP_LAST+1UL)*2UL];
};
//Get one of the default channel maps
const struct dspd_pcm_chmap *dspd_pcm_chmap_get_default(size_t channels);

//Create a translation between 2 channel maps
//Should be channel (right, left, etc) values in each map.
//The result is an output map with indexes.
//There are going to be two hard coded maps: 2=>1 and 1=>2
//It will support stereo<>mono conversion
int32_t dspd_pcm_chmap_translate(const struct dspd_pcm_chmap *in, 
				 const struct dspd_pcm_chmap *out,
				 struct dspd_pcm_chmap *map);

int32_t dspd_pcm_chmap_test(const struct dspd_pcm_chmap *in,  
			    const struct dspd_pcm_chmap *out);
int32_t dspd_pcm_chmap_test_channels(const struct dspd_pcm_chmap *map, size_t channels_in, size_t channels_out);

size_t dspd_pcm_chmap_sizeof(size_t count, int32_t flags);
const char *dspd_pcm_chmap_channel_name(size_t channel, bool abbrev);
ssize_t dspd_pcm_chmap_index(const char *name);
int32_t dspd_pcm_chmap_from_string(const char *str, struct dspd_pcm_chmap_container *map);
ssize_t dspd_pcm_chmap_to_string(const struct dspd_pcm_chmap *map, char *buf, size_t len);

void dspd_pcm_chmap_write_buf(const struct dspd_pcm_chmap * __restrict map, 
			      const float                 * __restrict inbuf,
			      double                      * __restrict outbuf,
			      size_t                                   frames,
			      double                                   volume);
void dspd_pcm_chmap_write_buf_multi(const struct dspd_pcm_chmap * __restrict map, 
				    const float                 * __restrict inbuf,
				    double                      * __restrict outbuf,
				    size_t                                   frames,
				    double                                   volume);
void dspd_pcm_chmap_write_buf_simple(const struct dspd_pcm_chmap * __restrict map, 
				     const float                 * __restrict inbuf,
				     double                      * __restrict outbuf,
				     size_t                                   frames,
				     double                                   volume);
void dspd_pcm_chmap_read_buf(const struct dspd_pcm_chmap * __restrict map, 
			     const float                 * __restrict inbuf,
			     float                       * __restrict outbuf,
			     size_t                                   frames,
			     float                                    volume);
void dspd_pcm_chmap_read_buf_multi(const struct dspd_pcm_chmap * __restrict map, 
				   const float                 * __restrict inbuf,
				   float                       * __restrict outbuf,
				   size_t                                   frames,
				   float                                    volume);
void dspd_pcm_chmap_read_buf_simple(const struct dspd_pcm_chmap * __restrict map, 
				    const float                 * __restrict inbuf,
				    float                       * __restrict outbuf,
				    size_t                                   frames,
				    float                                    volume);

int32_t dspd_pcm_chmap_any(const struct dspd_pcm_chmap *map, struct dspd_pcm_chmap *result);
#endif


