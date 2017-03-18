#ifndef _DSPD_CHMAP_H_
#define _DSPD_CHMAP_H_

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
  uint8_t pos[DSPD_CHMAP_LAST+1];
};
#define DSPD_CHMAP_MAXSIZE sizeof(struct dspd_fchmap)
#define DSPD_CHMAP_MAXCHAN (DSPD_CHMAP_LAST-DSPD_CHMAP_FL+1)

int32_t dspd_chmap_index(const struct dspd_chmap *map, 
			 uint32_t pos);
bool dspd_chmap_getconv(const struct dspd_chmap *from,
			const struct dspd_chmap *to,
			struct dspd_chmap *cmap);


void dspd_chmap_getdefault(struct dspd_chmap *map, unsigned int channels);
void dspd_chmap_add_route(struct dspd_chmap *map, uint32_t in, uint32_t out);
int dspd_chmap_create_generic(const struct dspd_chmap *devmap, 
			      struct dspd_chmap *climap);

bool dspd_chmap_test(const struct dspd_chmap *out,
		     const struct dspd_chmap *in,
		     uint32_t actual_channels);

void dspd_chmap_dump(const struct dspd_chmap *map);

//The stream can be 0 for standard (not multi) channel maps
size_t dspd_chmap_bufsize(uint8_t channels, uint8_t stream);
size_t dspd_chmap_sizeof(const struct dspd_chmap *map);
size_t dspd_fchmap_sizeof(const struct dspd_fchmap *map);




#endif


