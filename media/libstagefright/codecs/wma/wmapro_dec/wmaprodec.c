/*
* Wmapro compatible decoder
* Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion
* Copyright (c) 2008 - 2011 Sascha Sommer, Benjamin Larsson
*
* This file is part of FFmpeg.
*
* FFmpeg is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* FFmpeg is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with FFmpeg; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

/**
* @file
* @brief wmapro decoder implementation
* Wmapro is an MDCT based codec comparable to wma standard or AAC.
* The decoding therefore consists of the following steps:
* - bitstream decoding
* - reconstruction of per-channel data
* - rescaling and inverse quantization
* - IMDCT
* - windowing and overlapp-add
*
* The compressed wmapro bitstream is split into individual packets.
* Every such packet contains one or more wma frames.
* The compressed frames may have a variable length and frames may
* cross packet boundaries.
* Common to all wmapro frames is the number of samples that are stored in
* a frame.
* The number of samples and a few other decode flags are stored
* as extradata that has to be passed to the decoder.
*
* The wmapro frames themselves are again split into a variable number of
* subframes. Every subframe contains the data for 2^N time domain samples
* where N varies between 7 and 12.
*
* Example wmapro bitstream (in samples):
*
* ||   packet 0           || packet 1 || packet 2      packets
* ---------------------------------------------------
* || frame 0      || frame 1       || frame 2    ||    frames
* ---------------------------------------------------
* ||   |      |   ||   |   |   |   ||            ||    subframes of channel 0
* ---------------------------------------------------
* ||      |   |   ||   |   |   |   ||            ||    subframes of channel 1
* ---------------------------------------------------
*
* The frame layouts for the individual channels of a wma frame does not need
* to be the same.
*
* However, if the offsets and lengths of several subframes of a frame are the
* same, the subframes of the channels can be grouped.
* Every group may then use special coding techniques like M/S stereo coding
* to improve the compression ratio. These channel transformations do not
* need to be applied to a whole subframe. Instead, they can also work on
* individual scale factor bands (see below).
* The coefficients that carry the audio signal in the frequency domain
* are transmitted as huffman-coded vectors with 4, 2 and 1 elements.
* In addition to that, the encoder can switch to a runlevel coding scheme
* by transmitting subframe_length / 128 zero coefficients.
*
* Before the audio signal can be converted to the time domain, the
* coefficients have to be rescaled and inverse quantized.
* A subframe is therefore split into several scale factor bands that get
* scaled individually.
* Scale factors are submitted for every frame but they might be shared
* between the subframes of a channel. Scale factors are initially DPCM-coded.
* Once scale factors are shared, the differences are transmitted as runlevel
* codes.
* Every subframe length and offset combination in the frame layout shares a
* common quantization factor that can be adjusted for every channel by a
* modifier.
* After the inverse quantization, the coefficients get processed by an IMDCT.
* The resulting values are then windowed with a sine window and the first half
* of the values are added to the second half of the output from the previous
* subframe in order to reconstruct the output samples.
*/

#include "get_bits.h"
#include "put_bits.h"
#include "wma-fixed-debug.h"
#include "wmaprodata.h"
#include "wmapro_dec_api.h"
#include "common.h"
#define LOG_TAG "SoftWmapro"
#include <utils/Log.h>



//#define dprintf(...)

/* size of blocks */
#define BLOCK_MAX_BITS 11
#define BLOCK_MAX_SIZE (1 << BLOCK_MAX_BITS)

#define VLCBITS            9
#define VLCMAX ((22+VLCBITS-1)/VLCBITS)

/** current decoder limitations */
#define WMAPRO_MAX_CHANNELS    8                             ///< max number of handled channels
#define MAX_SUBFRAMES  32                                    ///< max number of subframes per channel
#define MAX_BANDS      29                                    ///< max number of scale factor bands
#define MAX_FRAMESIZE  32768                                 ///< maximum compressed frame size

#define WMAPRO_BLOCK_MIN_BITS  6                                           ///< log2 of min block size
#define WMAPRO_BLOCK_MAX_BITS 12                                           ///< log2 of max block size
#define WMAPRO_BLOCK_MAX_SIZE (1 << WMAPRO_BLOCK_MAX_BITS)                 ///< maximum block size
#define WMAPRO_BLOCK_SIZES    (WMAPRO_BLOCK_MAX_BITS - WMAPRO_BLOCK_MIN_BITS + 1) ///< possible block sizes


#define SCALEVLCBITS       8
#define VEC4MAXDEPTH    ((HUFF_VEC4_MAXBITS+VLCBITS-1)/VLCBITS)
#define VEC2MAXDEPTH    ((HUFF_VEC2_MAXBITS+VLCBITS-1)/VLCBITS)
#define VEC1MAXDEPTH    ((HUFF_VEC1_MAXBITS+VLCBITS-1)/VLCBITS)
#define SCALEMAXDEPTH   ((HUFF_SCALE_MAXBITS+SCALEVLCBITS-1)/SCALEVLCBITS)
#define SCALERLMAXDEPTH ((HUFF_SCALE_RL_MAXBITS+VLCBITS-1)/VLCBITS)

static VLC              sf_vlc;           ///< scale factor DPCM vlc
static VLC              sf_rl_vlc;        ///< scale factor run length vlc
static VLC              vec4_vlc;         ///< 4 coefficients per symbol
static VLC              vec2_vlc;         ///< 2 coefficients per symbol
static VLC              vec1_vlc;         ///< 1 coefficient per symbol
static VLC              coef_vlc[2];      ///< coefficient run length vlc codes
static int32_t          sin64[33];        ///< sinus table for decorrelation

typedef int32_t FixFFTSample;
typedef struct FixFFTComplex {
    FixFFTSample re, im;
} FixFFTComplex;

typedef struct FixFFTContext {
    int nbits;
    int inverse;
    uint16_t *revtab;
    FixFFTComplex *tmp_buf;
    int mdct_size; /* size of MDCT (i.e. number of input data * 2) */
    int mdct_bits; /* n = 2^nbits */
    /* pre/post rotation tables */
    FixFFTSample *tcos;
    FixFFTSample *tsin;
}FixFFTContext;


/**
* @brief frame specific decoder context for a single channel
*/
typedef struct{
    int16_t  prev_block_len;                          ///< length of the previous block
    uint8_t  transmit_coefs;
    uint8_t  num_subframes;
    uint16_t subframe_len[MAX_SUBFRAMES];             ///< subframe length in samples
    uint16_t subframe_offset[MAX_SUBFRAMES];          ///< subframe positions in the current frame
    uint8_t  cur_subframe;                            ///< current subframe number
    uint16_t decoded_samples;                         ///< number of already processed samples
    uint8_t  grouped;                                 ///< channel is part of a group
    int      quant_step;                              ///< quantization step for the current subframe
    int8_t   reuse_sf;                                ///< share scale factors between subframes
    int8_t   scale_factor_step;                       ///< scaling step for the current subframe
    int      max_scale_factor;                        ///< maximum scale factor for the current subframe
    int      saved_scale_factors[2][MAX_BANDS];       ///< resampled and (previously) transmitted scale factor values
    int8_t   scale_factor_idx;                        ///< index for the transmitted scale factor values (used for resampling)
    int*     scale_factors;                           ///< pointer to the scale factor values used for decoding
    uint8_t  table_idx;                               ///< index in sf_offsets for the scale factor reference block
    int64_t* fixcoeffs;                                  ///< pointer to the subframe decode buffer //gas add
    int32_t* fixcoeffs32;                                  ///< pointer to the subframe decode buffer //gas add for reuse fixout
    uint16_t num_vec_coeffs;                          ///< number of vector coded coefficients
    DECLARE_ALIGNED(16, int64_t, fixout)[WMAPRO_BLOCK_MAX_SIZE + WMAPRO_BLOCK_MAX_SIZE / 2]; ///< output buffer //gas add
    DECLARE_ALIGNED(16, int32_t, fixout32)[WMAPRO_BLOCK_MAX_SIZE + WMAPRO_BLOCK_MAX_SIZE / 2]; ///< output buffer //gas add
} WMAProChannelCtx;

/**
* @brief channel group for channel transformations
*/
typedef struct {
    uint8_t num_channels;                                     ///< number of channels in the group
    int8_t  transform;                                        ///< transform on / off
    int8_t  transform_band[MAX_BANDS];                        ///< controls if the transform is enabled for a certain band
    int32_t   decorrelation_matrix[WMAPRO_MAX_CHANNELS*WMAPRO_MAX_CHANNELS];
    int64_t*  fixchannel_data[WMAPRO_MAX_CHANNELS];                ///< transformation coefficients //gas add
} WMAProChannelGrp;

/**
* @brief main decoder context
*/
typedef struct WMAProDecodeCtx {
    /* generic decoder variables */
    AudioContext*  avctx;                         ///< codec context for av_log
    uint8_t          frame_data[MAX_FRAMESIZE +
		FF_INPUT_BUFFER_PADDING_SIZE];///< compressed frame data
    PutBitContext    pb;                            ///< context for filling the frame_data buffer
    FixFFTContext    fixmdct_ctx[WMAPRO_BLOCK_SIZES];  ///< MDCT context per block size gas add
    DECLARE_ALIGNED(16, int32_t, fixtmp)[WMAPRO_BLOCK_MAX_SIZE]; ///< IMDCT output buffer gas add
    int32_t*         windows[WMAPRO_BLOCK_SIZES];   ///< windows for the different block sizes
	
    /* frame size dependent frame information (set during initialization) */
    uint32_t         decode_flags;                  ///< used compression features
    uint8_t          len_prefix;                    ///< frame is prefixed with its length
    uint8_t          dynamic_range_compression;     ///< frame contains DRC data
    uint8_t          bits_per_sample;               ///< integer audio sample size for the unscaled IMDCT output (used to scale to [-1.0, 1.0])
    uint16_t         samples_per_frame;             ///< number of samples to output
    uint16_t         log2_frame_size;
    int8_t           num_channels;                  ///< number of channels in the stream (same as AVCodecContext.num_channels)
    int8_t           lfe_channel;                   ///< lfe channel index
    uint8_t          max_num_subframes;
    uint8_t          subframe_len_bits;             ///< number of bits used for the subframe length
    uint8_t          max_subframe_len_bit;          ///< flag indicating that the subframe is of maximum size when the first subframe length bit is 1
    uint16_t         min_samples_per_subframe;
    int8_t           num_sfb[WMAPRO_BLOCK_SIZES];   ///< scale factor bands per block size
    int16_t          sfb_offsets[WMAPRO_BLOCK_SIZES][MAX_BANDS];                    ///< scale factor band offsets (multiples of 4)
    int8_t           sf_offsets[WMAPRO_BLOCK_SIZES][WMAPRO_BLOCK_SIZES][MAX_BANDS]; ///< scale factor resample matrix
    int16_t          subwoofer_cutoffs[WMAPRO_BLOCK_SIZES]; ///< subwoofer cutoff values
	
    /* packet decode state */
    GetBitContext    pgb;                           ///< bitstream reader context for the packet
    int              next_packet_start;             ///< start offset of the next wma packet in the demuxer packet
    uint8_t          packet_offset;                 ///< frame offset in the packet
    uint8_t          packet_sequence_number;        ///< current packet number
    int              num_saved_bits;                ///< saved number of bits
    int              frame_offset;                  ///< frame offset in the bit reservoir
    int              subframe_offset;               ///< subframe offset in the bit reservoir
    uint8_t          packet_loss;                   ///< set in case of bitstream error
    uint8_t          packet_done;                   ///< set when a packet is fully decoded
	
    /* frame decode state */
    uint32_t         frame_num;                     ///< current frame number (not used for decoding)
    GetBitContext    gb;                            ///< bitstream reader context
    int              buf_bit_size;                  ///< buffer size in bits
    short*           samples;                       ///< current samplebuffer pointer
    short*           samples_end;                   ///< maximum samplebuffer pointer
    uint8_t          drc_gain;                      ///< gain for the DRC tool
    int8_t           skip_frame;                    ///< skip output step
    int8_t           parsed_all_subframes;          ///< all subframes decoded?
	
    /* subframe/block decode state */
    int16_t          subframe_len;                  ///< current subframe length
    int8_t           channels_for_cur_subframe;     ///< number of channels that contain the subframe
    int8_t           channel_indexes_for_cur_subframe[WMAPRO_MAX_CHANNELS];
    int8_t           num_bands;                     ///< number of scale factor bands
    int8_t           transmit_num_vec_coeffs;       ///< number of vector coded coefficients is part of the bitstream
    int16_t*         cur_sfb_offsets;               ///< sfb offsets for the current block
    uint8_t          table_idx;                     ///< index for the num_sfb, sfb_offsets, sf_offsets and subwoofer_cutoffs tables
    int8_t           esc_len;                       ///< length of escaped coefficients
	
    uint8_t          num_chgroups;                  ///< number of channel groups
    WMAProChannelGrp chgroup[WMAPRO_MAX_CHANNELS];  ///< channel group information
	
    WMAProChannelCtx channel[WMAPRO_MAX_CHANNELS];  ///< per channel data
} WMAProDecodeCtx;

static const long cordic_circular_gain = 0xb2458939; /* 0.607252929 */  

/* Table of values of atan(2^-i) in 0.32 format fractions of pi where pi = 0xffffffff / 2 */    
static const unsigned long atan_table[] = {     
	0x1fffffff, /* +0.785398163 (or pi/4) */    
	0x12e4051d, /* +0.463647609 */  
	0x09fb385b, /* +0.244978663 */  
	0x051111d4, /* +0.124354995 */  
	0x028b0d43, /* +0.062418810 */  
	0x0145d7e1, /* +0.031239833 */  
	0x00a2f61e, /* +0.015623729 */  
	0x00517c55, /* +0.007812341 */  
	0x0028be53, /* +0.003906230 */  
	0x00145f2e, /* +0.001953123 */  
	0x000a2f98, /* +0.000976562 */  
	0x000517cc, /* +0.000488281 */  
	0x00028be6, /* +0.000244141 */  
	0x000145f3, /* +0.000122070 */  
	0x0000a2f9, /* +0.000061035 */  
	0x0000517c, /* +0.000030518 */  
	0x000028be, /* +0.000015259 */  
	0x0000145f, /* +0.000007629 */  
	0x00000a2f, /* +0.000003815 */  
	0x00000517, /* +0.000001907 */  
	0x0000028b, /* +0.000000954 */  
	0x00000145, /* +0.000000477 */  
	0x000000a2, /* +0.000000238 */  
	0x00000051, /* +0.000000119 */  
	0x00000028, /* +0.000000060 */  
	0x00000014, /* +0.000000030 */  
	0x0000000a, /* +0.000000015 */  
	0x00000005, /* +0.000000007 */  
	0x00000002, /* +0.000000004 */  
	0x00000001, /* +0.000000002 */  
	0x00000000, /* +0.000000001 */  
	0x00000000, /* +0.000000000 */  
};  

/**     
* Implements sin and cos using CORDIC rotation.    
*  
* @param phase has range from 0 to 0xffffffff, representing 0 and  
*        2*pi respectively.    
* @param cos return address for cos    
* @return sin of phase, value is a signed value from LONG_MIN to LONG_MAX,     
*         representing -1 and 1 respectively.  
*  
*        Gives at least 24 bits precision (last 2-8 bits or so are probably off)   
*/     
static int fsincos(uint32_t phase, int *cosval)     
{   
	int32_t x, x1, y, y1;   
	unsigned long z, z1;    
	int i;  
	
	/* Setup initial vector */  
	x = cordic_circular_gain;   
	y = 0;  
	z = phase;  
	
	/* The phase has to be somewhere between 0..pi for this to work right */    
	if (z < 0xffffffff / 4) {   
		/* z in first quadrant, z += pi/2 to correct */     
		x = -x;     
		z += 0xffffffff / 4;    
	} else if (z < 3 * (0xffffffff / 4)) {  
		/* z in third quadrant, z -= pi/2 to correct */     
		z -= 0xffffffff / 4;    
	} else {    
		/* z in fourth quadrant, z -= 3pi/2 to correct */   
		x = -x;     
		z -= 3 * (0xffffffff / 4);  
	}   
	
	/* Each iteration adds roughly 1-bit of extra precision */  
	for (i = 0; i < 31; i++) {  
		x1 = x >> i;    
		y1 = y >> i;    
		z1 = atan_table[i];     
		
		/* Decided which direction to rotate vector. Pivot point is pi/2 */     
		if (z >= 0xffffffff / 4) {  
			x -= y1;    
			y += x1;    
			z -= z1;    
		} else {    
			x += y1;    
			y -= x1;    
			z += z1;    
		}   
	}   
	
	if (cosval)    
		*cosval = x;   
	
	return y;   
}

typedef int32_t fixed32; 
typedef int64_t fixed64;

static const int32_t sincos_lookup0[1026] = {
	0x00000000, 0x7fffffff, 0x003243f5, 0x7ffff621,
		0x006487e3, 0x7fffd886, 0x0096cbc1, 0x7fffa72c,
		0x00c90f88, 0x7fff6216, 0x00fb5330, 0x7fff0943,
		0x012d96b1, 0x7ffe9cb2, 0x015fda03, 0x7ffe1c65,
		0x01921d20, 0x7ffd885a, 0x01c45ffe, 0x7ffce093,
		0x01f6a297, 0x7ffc250f, 0x0228e4e2, 0x7ffb55ce,
		0x025b26d7, 0x7ffa72d1, 0x028d6870, 0x7ff97c18,
		0x02bfa9a4, 0x7ff871a2, 0x02f1ea6c, 0x7ff75370,
		0x03242abf, 0x7ff62182, 0x03566a96, 0x7ff4dbd9,
		0x0388a9ea, 0x7ff38274, 0x03bae8b2, 0x7ff21553,
		0x03ed26e6, 0x7ff09478, 0x041f6480, 0x7feeffe1,
		0x0451a177, 0x7fed5791, 0x0483ddc3, 0x7feb9b85,
		0x04b6195d, 0x7fe9cbc0, 0x04e8543e, 0x7fe7e841,
		0x051a8e5c, 0x7fe5f108, 0x054cc7b1, 0x7fe3e616,
		0x057f0035, 0x7fe1c76b, 0x05b137df, 0x7fdf9508,
		0x05e36ea9, 0x7fdd4eec, 0x0615a48b, 0x7fdaf519,
		0x0647d97c, 0x7fd8878e, 0x067a0d76, 0x7fd6064c,
		0x06ac406f, 0x7fd37153, 0x06de7262, 0x7fd0c8a3,
		0x0710a345, 0x7fce0c3e, 0x0742d311, 0x7fcb3c23,
		0x077501be, 0x7fc85854, 0x07a72f45, 0x7fc560cf,
		0x07d95b9e, 0x7fc25596, 0x080b86c2, 0x7fbf36aa,
		0x083db0a7, 0x7fbc040a, 0x086fd947, 0x7fb8bdb8,
		0x08a2009a, 0x7fb563b3, 0x08d42699, 0x7fb1f5fc,
		0x09064b3a, 0x7fae7495, 0x09386e78, 0x7faadf7c,
		0x096a9049, 0x7fa736b4, 0x099cb0a7, 0x7fa37a3c,
		0x09cecf89, 0x7f9faa15, 0x0a00ece8, 0x7f9bc640,
		0x0a3308bd, 0x7f97cebd, 0x0a6522fe, 0x7f93c38c,
		0x0a973ba5, 0x7f8fa4b0, 0x0ac952aa, 0x7f8b7227,
		0x0afb6805, 0x7f872bf3, 0x0b2d7baf, 0x7f82d214,
		0x0b5f8d9f, 0x7f7e648c, 0x0b919dcf, 0x7f79e35a,
		0x0bc3ac35, 0x7f754e80, 0x0bf5b8cb, 0x7f70a5fe,
		0x0c27c389, 0x7f6be9d4, 0x0c59cc68, 0x7f671a05,
		0x0c8bd35e, 0x7f62368f, 0x0cbdd865, 0x7f5d3f75,
		0x0cefdb76, 0x7f5834b7, 0x0d21dc87, 0x7f531655,
		0x0d53db92, 0x7f4de451, 0x0d85d88f, 0x7f489eaa,
		0x0db7d376, 0x7f434563, 0x0de9cc40, 0x7f3dd87c,
		0x0e1bc2e4, 0x7f3857f6, 0x0e4db75b, 0x7f32c3d1,
		0x0e7fa99e, 0x7f2d1c0e, 0x0eb199a4, 0x7f2760af,
		0x0ee38766, 0x7f2191b4, 0x0f1572dc, 0x7f1baf1e,
		0x0f475bff, 0x7f15b8ee, 0x0f7942c7, 0x7f0faf25,
		0x0fab272b, 0x7f0991c4, 0x0fdd0926, 0x7f0360cb,
		0x100ee8ad, 0x7efd1c3c, 0x1040c5bb, 0x7ef6c418,
		0x1072a048, 0x7ef05860, 0x10a4784b, 0x7ee9d914,
		0x10d64dbd, 0x7ee34636, 0x11082096, 0x7edc9fc6,
		0x1139f0cf, 0x7ed5e5c6, 0x116bbe60, 0x7ecf1837,
		0x119d8941, 0x7ec8371a, 0x11cf516a, 0x7ec14270,
		0x120116d5, 0x7eba3a39, 0x1232d979, 0x7eb31e78,
		0x1264994e, 0x7eabef2c, 0x1296564d, 0x7ea4ac58,
		0x12c8106f, 0x7e9d55fc, 0x12f9c7aa, 0x7e95ec1a,
		0x132b7bf9, 0x7e8e6eb2, 0x135d2d53, 0x7e86ddc6,
		0x138edbb1, 0x7e7f3957, 0x13c0870a, 0x7e778166,
		0x13f22f58, 0x7e6fb5f4, 0x1423d492, 0x7e67d703,
		0x145576b1, 0x7e5fe493, 0x148715ae, 0x7e57dea7,
		0x14b8b17f, 0x7e4fc53e, 0x14ea4a1f, 0x7e47985b,
		0x151bdf86, 0x7e3f57ff, 0x154d71aa, 0x7e37042a,
		0x157f0086, 0x7e2e9cdf, 0x15b08c12, 0x7e26221f,
		0x15e21445, 0x7e1d93ea, 0x16139918, 0x7e14f242,
		0x16451a83, 0x7e0c3d29, 0x1676987f, 0x7e0374a0,
		0x16a81305, 0x7dfa98a8, 0x16d98a0c, 0x7df1a942,
		0x170afd8d, 0x7de8a670, 0x173c6d80, 0x7ddf9034,
		0x176dd9de, 0x7dd6668f, 0x179f429f, 0x7dcd2981,
		0x17d0a7bc, 0x7dc3d90d, 0x1802092c, 0x7dba7534,
		0x183366e9, 0x7db0fdf8, 0x1864c0ea, 0x7da77359,
		0x18961728, 0x7d9dd55a, 0x18c7699b, 0x7d9423fc,
		0x18f8b83c, 0x7d8a5f40, 0x192a0304, 0x7d808728,
		0x195b49ea, 0x7d769bb5, 0x198c8ce7, 0x7d6c9ce9,
		0x19bdcbf3, 0x7d628ac6, 0x19ef0707, 0x7d58654d,
		0x1a203e1b, 0x7d4e2c7f, 0x1a517128, 0x7d43e05e,
		0x1a82a026, 0x7d3980ec, 0x1ab3cb0d, 0x7d2f0e2b,
		0x1ae4f1d6, 0x7d24881b, 0x1b161479, 0x7d19eebf,
		0x1b4732ef, 0x7d0f4218, 0x1b784d30, 0x7d048228,
		0x1ba96335, 0x7cf9aef0, 0x1bda74f6, 0x7ceec873,
		0x1c0b826a, 0x7ce3ceb2, 0x1c3c8b8c, 0x7cd8c1ae,
		0x1c6d9053, 0x7ccda169, 0x1c9e90b8, 0x7cc26de5,
		0x1ccf8cb3, 0x7cb72724, 0x1d00843d, 0x7cabcd28,
		0x1d31774d, 0x7ca05ff1, 0x1d6265dd, 0x7c94df83,
		0x1d934fe5, 0x7c894bde, 0x1dc4355e, 0x7c7da505,
		0x1df5163f, 0x7c71eaf9, 0x1e25f282, 0x7c661dbc,
		0x1e56ca1e, 0x7c5a3d50, 0x1e879d0d, 0x7c4e49b7,
		0x1eb86b46, 0x7c4242f2, 0x1ee934c3, 0x7c362904,
		0x1f19f97b, 0x7c29fbee, 0x1f4ab968, 0x7c1dbbb3,
		0x1f7b7481, 0x7c116853, 0x1fac2abf, 0x7c0501d2,
		0x1fdcdc1b, 0x7bf88830, 0x200d888d, 0x7bebfb70,
		0x203e300d, 0x7bdf5b94, 0x206ed295, 0x7bd2a89e,
		0x209f701c, 0x7bc5e290, 0x20d0089c, 0x7bb9096b,
		0x21009c0c, 0x7bac1d31, 0x21312a65, 0x7b9f1de6,
		0x2161b3a0, 0x7b920b89, 0x219237b5, 0x7b84e61f,
		0x21c2b69c, 0x7b77ada8, 0x21f3304f, 0x7b6a6227,
		0x2223a4c5, 0x7b5d039e, 0x225413f8, 0x7b4f920e,
		0x22847de0, 0x7b420d7a, 0x22b4e274, 0x7b3475e5,
		0x22e541af, 0x7b26cb4f, 0x23159b88, 0x7b190dbc,
		0x2345eff8, 0x7b0b3d2c, 0x23763ef7, 0x7afd59a4,
		0x23a6887f, 0x7aef6323, 0x23d6cc87, 0x7ae159ae,
		0x24070b08, 0x7ad33d45, 0x243743fa, 0x7ac50dec,
		0x24677758, 0x7ab6cba4, 0x2497a517, 0x7aa8766f,
		0x24c7cd33, 0x7a9a0e50, 0x24f7efa2, 0x7a8b9348,
		0x25280c5e, 0x7a7d055b, 0x2558235f, 0x7a6e648a,
		0x2588349d, 0x7a5fb0d8, 0x25b84012, 0x7a50ea47,
		0x25e845b6, 0x7a4210d8, 0x26184581, 0x7a332490,
		0x26483f6c, 0x7a24256f, 0x26783370, 0x7a151378,
		0x26a82186, 0x7a05eead, 0x26d809a5, 0x79f6b711,
		0x2707ebc7, 0x79e76ca7, 0x2737c7e3, 0x79d80f6f,
		0x27679df4, 0x79c89f6e, 0x27976df1, 0x79b91ca4,
		0x27c737d3, 0x79a98715, 0x27f6fb92, 0x7999dec4,
		0x2826b928, 0x798a23b1, 0x2856708d, 0x797a55e0,
		0x288621b9, 0x796a7554, 0x28b5cca5, 0x795a820e,
		0x28e5714b, 0x794a7c12, 0x29150fa1, 0x793a6361,
		0x2944a7a2, 0x792a37fe, 0x29743946, 0x7919f9ec,
		0x29a3c485, 0x7909a92d, 0x29d34958, 0x78f945c3,
		0x2a02c7b8, 0x78e8cfb2, 0x2a323f9e, 0x78d846fb,
		0x2a61b101, 0x78c7aba2, 0x2a911bdc, 0x78b6fda8,
		0x2ac08026, 0x78a63d11, 0x2aefddd8, 0x789569df,
		0x2b1f34eb, 0x78848414, 0x2b4e8558, 0x78738bb3,
		0x2b7dcf17, 0x786280bf, 0x2bad1221, 0x7851633b,
		0x2bdc4e6f, 0x78403329, 0x2c0b83fa, 0x782ef08b,
		0x2c3ab2b9, 0x781d9b65, 0x2c69daa6, 0x780c33b8,
		0x2c98fbba, 0x77fab989, 0x2cc815ee, 0x77e92cd9,
		0x2cf72939, 0x77d78daa, 0x2d263596, 0x77c5dc01,
		0x2d553afc, 0x77b417df, 0x2d843964, 0x77a24148,
		0x2db330c7, 0x7790583e, 0x2de2211e, 0x777e5cc3,
		0x2e110a62, 0x776c4edb, 0x2e3fec8b, 0x775a2e89,
		0x2e6ec792, 0x7747fbce, 0x2e9d9b70, 0x7735b6af,
		0x2ecc681e, 0x77235f2d, 0x2efb2d95, 0x7710f54c,
		0x2f29ebcc, 0x76fe790e, 0x2f58a2be, 0x76ebea77,
		0x2f875262, 0x76d94989, 0x2fb5fab2, 0x76c69647,
		0x2fe49ba7, 0x76b3d0b4, 0x30133539, 0x76a0f8d2,
		0x3041c761, 0x768e0ea6, 0x30705217, 0x767b1231,
		0x309ed556, 0x76680376, 0x30cd5115, 0x7654e279,
		0x30fbc54d, 0x7641af3d, 0x312a31f8, 0x762e69c4,
		0x3158970e, 0x761b1211, 0x3186f487, 0x7607a828,
		0x31b54a5e, 0x75f42c0b, 0x31e39889, 0x75e09dbd,
		0x3211df04, 0x75ccfd42, 0x32401dc6, 0x75b94a9c,
		0x326e54c7, 0x75a585cf, 0x329c8402, 0x7591aedd,
		0x32caab6f, 0x757dc5ca, 0x32f8cb07, 0x7569ca99,
		0x3326e2c3, 0x7555bd4c, 0x3354f29b, 0x75419de7,
		0x3382fa88, 0x752d6c6c, 0x33b0fa84, 0x751928e0,
		0x33def287, 0x7504d345, 0x340ce28b, 0x74f06b9e,
		0x343aca87, 0x74dbf1ef, 0x3468aa76, 0x74c7663a,
		0x34968250, 0x74b2c884, 0x34c4520d, 0x749e18cd,
		0x34f219a8, 0x7489571c, 0x351fd918, 0x74748371,
		0x354d9057, 0x745f9dd1, 0x357b3f5d, 0x744aa63f,
		0x35a8e625, 0x74359cbd, 0x35d684a6, 0x74208150,
		0x36041ad9, 0x740b53fb, 0x3631a8b8, 0x73f614c0,
		0x365f2e3b, 0x73e0c3a3, 0x368cab5c, 0x73cb60a8,
		0x36ba2014, 0x73b5ebd1, 0x36e78c5b, 0x73a06522,
		0x3714f02a, 0x738acc9e, 0x37424b7b, 0x73752249,
		0x376f9e46, 0x735f6626, 0x379ce885, 0x73499838,
		0x37ca2a30, 0x7333b883, 0x37f76341, 0x731dc70a,
		0x382493b0, 0x7307c3d0, 0x3851bb77, 0x72f1aed9,
		0x387eda8e, 0x72db8828, 0x38abf0ef, 0x72c54fc1,
		0x38d8fe93, 0x72af05a7, 0x39060373, 0x7298a9dd,
		0x3932ff87, 0x72823c67, 0x395ff2c9, 0x726bbd48,
		0x398cdd32, 0x72552c85, 0x39b9bebc, 0x723e8a20,
		0x39e6975e, 0x7227d61c, 0x3a136712, 0x7211107e,
		0x3a402dd2, 0x71fa3949, 0x3a6ceb96, 0x71e35080,
		0x3a99a057, 0x71cc5626, 0x3ac64c0f, 0x71b54a41,
		0x3af2eeb7, 0x719e2cd2, 0x3b1f8848, 0x7186fdde,
		0x3b4c18ba, 0x716fbd68, 0x3b78a007, 0x71586b74,
		0x3ba51e29, 0x71410805, 0x3bd19318, 0x7129931f,
		0x3bfdfecd, 0x71120cc5, 0x3c2a6142, 0x70fa74fc,
		0x3c56ba70, 0x70e2cbc6, 0x3c830a50, 0x70cb1128,
		0x3caf50da, 0x70b34525, 0x3cdb8e09, 0x709b67c0,
		0x3d07c1d6, 0x708378ff, 0x3d33ec39, 0x706b78e3,
		0x3d600d2c, 0x70536771, 0x3d8c24a8, 0x703b44ad,
		0x3db832a6, 0x7023109a, 0x3de4371f, 0x700acb3c,
		0x3e10320d, 0x6ff27497, 0x3e3c2369, 0x6fda0cae,
		0x3e680b2c, 0x6fc19385, 0x3e93e950, 0x6fa90921,
		0x3ebfbdcd, 0x6f906d84, 0x3eeb889c, 0x6f77c0b3,
		0x3f1749b8, 0x6f5f02b2, 0x3f430119, 0x6f463383,
		0x3f6eaeb8, 0x6f2d532c, 0x3f9a5290, 0x6f1461b0,
		0x3fc5ec98, 0x6efb5f12, 0x3ff17cca, 0x6ee24b57,
		0x401d0321, 0x6ec92683, 0x40487f94, 0x6eaff099,
		0x4073f21d, 0x6e96a99d, 0x409f5ab6, 0x6e7d5193,
		0x40cab958, 0x6e63e87f, 0x40f60dfb, 0x6e4a6e66,
		0x4121589b, 0x6e30e34a, 0x414c992f, 0x6e174730,
		0x4177cfb1, 0x6dfd9a1c, 0x41a2fc1a, 0x6de3dc11,
		0x41ce1e65, 0x6dca0d14, 0x41f93689, 0x6db02d29,
		0x42244481, 0x6d963c54, 0x424f4845, 0x6d7c3a98,
		0x427a41d0, 0x6d6227fa, 0x42a5311b, 0x6d48047e,
		0x42d0161e, 0x6d2dd027, 0x42faf0d4, 0x6d138afb,
		0x4325c135, 0x6cf934fc, 0x4350873c, 0x6cdece2f,
		0x437b42e1, 0x6cc45698, 0x43a5f41e, 0x6ca9ce3b,
		0x43d09aed, 0x6c8f351c, 0x43fb3746, 0x6c748b3f,
		0x4425c923, 0x6c59d0a9, 0x4450507e, 0x6c3f055d,
		0x447acd50, 0x6c242960, 0x44a53f93, 0x6c093cb6,
		0x44cfa740, 0x6bee3f62, 0x44fa0450, 0x6bd3316a,
		0x452456bd, 0x6bb812d1, 0x454e9e80, 0x6b9ce39b,
		0x4578db93, 0x6b81a3cd, 0x45a30df0, 0x6b66536b,
		0x45cd358f, 0x6b4af279, 0x45f7526b, 0x6b2f80fb,
		0x4621647d, 0x6b13fef5, 0x464b6bbe, 0x6af86c6c,
		0x46756828, 0x6adcc964, 0x469f59b4, 0x6ac115e2,
		0x46c9405c, 0x6aa551e9, 0x46f31c1a, 0x6a897d7d,
		0x471cece7, 0x6a6d98a4, 0x4746b2bc, 0x6a51a361,
		0x47706d93, 0x6a359db9, 0x479a1d67, 0x6a1987b0,
		0x47c3c22f, 0x69fd614a, 0x47ed5be6, 0x69e12a8c,
		0x4816ea86, 0x69c4e37a, 0x48406e08, 0x69a88c19,
		0x4869e665, 0x698c246c, 0x48935397, 0x696fac78,
		0x48bcb599, 0x69532442, 0x48e60c62, 0x69368bce,
		0x490f57ee, 0x6919e320, 0x49389836, 0x68fd2a3d,
		0x4961cd33, 0x68e06129, 0x498af6df, 0x68c387e9,
		0x49b41533, 0x68a69e81, 0x49dd282a, 0x6889a4f6,
		0x4a062fbd, 0x686c9b4b, 0x4a2f2be6, 0x684f8186,
		0x4a581c9e, 0x683257ab, 0x4a8101de, 0x68151dbe,
		0x4aa9dba2, 0x67f7d3c5, 0x4ad2a9e2, 0x67da79c3,
		0x4afb6c98, 0x67bd0fbd, 0x4b2423be, 0x679f95b7,
		0x4b4ccf4d, 0x67820bb7, 0x4b756f40, 0x676471c0,
		0x4b9e0390, 0x6746c7d8, 0x4bc68c36, 0x67290e02,
		0x4bef092d, 0x670b4444, 0x4c177a6e, 0x66ed6aa1,
		0x4c3fdff4, 0x66cf8120, 0x4c6839b7, 0x66b187c3,
		0x4c9087b1, 0x66937e91, 0x4cb8c9dd, 0x6675658c,
		0x4ce10034, 0x66573cbb, 0x4d092ab0, 0x66390422,
		0x4d31494b, 0x661abbc5, 0x4d595bfe, 0x65fc63a9,
		0x4d8162c4, 0x65ddfbd3, 0x4da95d96, 0x65bf8447,
		0x4dd14c6e, 0x65a0fd0b, 0x4df92f46, 0x65826622,
		0x4e210617, 0x6563bf92, 0x4e48d0dd, 0x6545095f,
		0x4e708f8f, 0x6526438f, 0x4e984229, 0x65076e25,
		0x4ebfe8a5, 0x64e88926, 0x4ee782fb, 0x64c99498,
		0x4f0f1126, 0x64aa907f, 0x4f369320, 0x648b7ce0,
		0x4f5e08e3, 0x646c59bf, 0x4f857269, 0x644d2722,
		0x4faccfab, 0x642de50d, 0x4fd420a4, 0x640e9386,
		0x4ffb654d, 0x63ef3290, 0x50229da1, 0x63cfc231,
		0x5049c999, 0x63b0426d, 0x5070e92f, 0x6390b34a,
		0x5097fc5e, 0x637114cc, 0x50bf031f, 0x635166f9,
		0x50e5fd6d, 0x6331a9d4, 0x510ceb40, 0x6311dd64,
		0x5133cc94, 0x62f201ac, 0x515aa162, 0x62d216b3,
		0x518169a5, 0x62b21c7b, 0x51a82555, 0x6292130c,
		0x51ced46e, 0x6271fa69, 0x51f576ea, 0x6251d298,
		0x521c0cc2, 0x62319b9d, 0x524295f0, 0x6211557e,
		0x5269126e, 0x61f1003f, 0x528f8238, 0x61d09be5,
		0x52b5e546, 0x61b02876, 0x52dc3b92, 0x618fa5f7,
		0x53028518, 0x616f146c, 0x5328c1d0, 0x614e73da,
		0x534ef1b5, 0x612dc447, 0x537514c2, 0x610d05b7,
		0x539b2af0, 0x60ec3830, 0x53c13439, 0x60cb5bb7,
		0x53e73097, 0x60aa7050, 0x540d2005, 0x60897601,
		0x5433027d, 0x60686ccf, 0x5458d7f9, 0x604754bf,
		0x547ea073, 0x60262dd6, 0x54a45be6, 0x6004f819,
		0x54ca0a4b, 0x5fe3b38d, 0x54efab9c, 0x5fc26038,
		0x55153fd4, 0x5fa0fe1f, 0x553ac6ee, 0x5f7f8d46,
		0x556040e2, 0x5f5e0db3, 0x5585adad, 0x5f3c7f6b,
		0x55ab0d46, 0x5f1ae274, 0x55d05faa, 0x5ef936d1,
		0x55f5a4d2, 0x5ed77c8a, 0x561adcb9, 0x5eb5b3a2,
		0x56400758, 0x5e93dc1f, 0x566524aa, 0x5e71f606,
		0x568a34a9, 0x5e50015d, 0x56af3750, 0x5e2dfe29,
		0x56d42c99, 0x5e0bec6e, 0x56f9147e, 0x5de9cc33,
		0x571deefa, 0x5dc79d7c, 0x5742bc06, 0x5da5604f,
		0x57677b9d, 0x5d8314b1, 0x578c2dba, 0x5d60baa7,
		0x57b0d256, 0x5d3e5237, 0x57d5696d, 0x5d1bdb65,
		0x57f9f2f8, 0x5cf95638, 0x581e6ef1, 0x5cd6c2b5,
		0x5842dd54, 0x5cb420e0, 0x58673e1b, 0x5c9170bf,
		0x588b9140, 0x5c6eb258, 0x58afd6bd, 0x5c4be5b0,
		0x58d40e8c, 0x5c290acc, 0x58f838a9, 0x5c0621b2,
		0x591c550e, 0x5be32a67, 0x594063b5, 0x5bc024f0,
		0x59646498, 0x5b9d1154, 0x598857b2, 0x5b79ef96,
		0x59ac3cfd, 0x5b56bfbd, 0x59d01475, 0x5b3381ce,
		0x59f3de12, 0x5b1035cf, 0x5a1799d1, 0x5aecdbc5,
		0x5a3b47ab, 0x5ac973b5, 0x5a5ee79a, 0x5aa5fda5,
		0x5a82799a, 0x5a82799a
  };
  
  static const int32_t sincos_lookup1[1024] = {
	  0x001921fb, 0x7ffffd88, 0x004b65ee, 0x7fffe9cb,
		  0x007da9d4, 0x7fffc251, 0x00afeda8, 0x7fff8719,
		  0x00e23160, 0x7fff3824, 0x011474f6, 0x7ffed572,
		  0x0146b860, 0x7ffe5f03, 0x0178fb99, 0x7ffdd4d7,
		  0x01ab3e97, 0x7ffd36ee, 0x01dd8154, 0x7ffc8549,
		  0x020fc3c6, 0x7ffbbfe6, 0x024205e8, 0x7ffae6c7,
		  0x027447b0, 0x7ff9f9ec, 0x02a68917, 0x7ff8f954,
		  0x02d8ca16, 0x7ff7e500, 0x030b0aa4, 0x7ff6bcf0,
		  0x033d4abb, 0x7ff58125, 0x036f8a51, 0x7ff4319d,
		  0x03a1c960, 0x7ff2ce5b, 0x03d407df, 0x7ff1575d,
		  0x040645c7, 0x7fefcca4, 0x04388310, 0x7fee2e30,
		  0x046abfb3, 0x7fec7c02, 0x049cfba7, 0x7feab61a,
		  0x04cf36e5, 0x7fe8dc78, 0x05017165, 0x7fe6ef1c,
		  0x0533ab20, 0x7fe4ee06, 0x0565e40d, 0x7fe2d938,
		  0x05981c26, 0x7fe0b0b1, 0x05ca5361, 0x7fde7471,
		  0x05fc89b8, 0x7fdc247a, 0x062ebf22, 0x7fd9c0ca,
		  0x0660f398, 0x7fd74964, 0x06932713, 0x7fd4be46,
		  0x06c5598a, 0x7fd21f72, 0x06f78af6, 0x7fcf6ce8,
		  0x0729bb4e, 0x7fcca6a7, 0x075bea8c, 0x7fc9ccb2,
		  0x078e18a7, 0x7fc6df08, 0x07c04598, 0x7fc3dda9,
		  0x07f27157, 0x7fc0c896, 0x08249bdd, 0x7fbd9fd0,
		  0x0856c520, 0x7fba6357, 0x0888ed1b, 0x7fb7132b,
		  0x08bb13c5, 0x7fb3af4e, 0x08ed3916, 0x7fb037bf,
		  0x091f5d06, 0x7facac7f, 0x09517f8f, 0x7fa90d8e,
		  0x0983a0a7, 0x7fa55aee, 0x09b5c048, 0x7fa1949e,
		  0x09e7de6a, 0x7f9dbaa0, 0x0a19fb04, 0x7f99ccf4,
		  0x0a4c1610, 0x7f95cb9a, 0x0a7e2f85, 0x7f91b694,
		  0x0ab0475c, 0x7f8d8de1, 0x0ae25d8d, 0x7f895182,
		  0x0b147211, 0x7f850179, 0x0b4684df, 0x7f809dc5,
		  0x0b7895f0, 0x7f7c2668, 0x0baaa53b, 0x7f779b62,
		  0x0bdcb2bb, 0x7f72fcb4, 0x0c0ebe66, 0x7f6e4a5e,
		  0x0c40c835, 0x7f698461, 0x0c72d020, 0x7f64aabf,
		  0x0ca4d620, 0x7f5fbd77, 0x0cd6da2d, 0x7f5abc8a,
		  0x0d08dc3f, 0x7f55a7fa, 0x0d3adc4e, 0x7f507fc7,
		  0x0d6cda53, 0x7f4b43f2, 0x0d9ed646, 0x7f45f47b,
		  0x0dd0d01f, 0x7f409164, 0x0e02c7d7, 0x7f3b1aad,
		  0x0e34bd66, 0x7f359057, 0x0e66b0c3, 0x7f2ff263,
		  0x0e98a1e9, 0x7f2a40d2, 0x0eca90ce, 0x7f247ba5,
		  0x0efc7d6b, 0x7f1ea2dc, 0x0f2e67b8, 0x7f18b679,
		  0x0f604faf, 0x7f12b67c, 0x0f923546, 0x7f0ca2e7,
		  0x0fc41876, 0x7f067bba, 0x0ff5f938, 0x7f0040f6,
		  0x1027d784, 0x7ef9f29d, 0x1059b352, 0x7ef390ae,
		  0x108b8c9b, 0x7eed1b2c, 0x10bd6356, 0x7ee69217,
		  0x10ef377d, 0x7edff570, 0x11210907, 0x7ed94538,
		  0x1152d7ed, 0x7ed28171, 0x1184a427, 0x7ecbaa1a,
		  0x11b66dad, 0x7ec4bf36, 0x11e83478, 0x7ebdc0c6,
		  0x1219f880, 0x7eb6aeca, 0x124bb9be, 0x7eaf8943,
		  0x127d7829, 0x7ea85033, 0x12af33ba, 0x7ea1039b,
		  0x12e0ec6a, 0x7e99a37c, 0x1312a230, 0x7e922fd6,
		  0x13445505, 0x7e8aa8ac, 0x137604e2, 0x7e830dff,
		  0x13a7b1bf, 0x7e7b5fce, 0x13d95b93, 0x7e739e1d,
		  0x140b0258, 0x7e6bc8eb, 0x143ca605, 0x7e63e03b,
		  0x146e4694, 0x7e5be40c, 0x149fe3fc, 0x7e53d462,
		  0x14d17e36, 0x7e4bb13c, 0x1503153a, 0x7e437a9c,
		  0x1534a901, 0x7e3b3083, 0x15663982, 0x7e32d2f4,
		  0x1597c6b7, 0x7e2a61ed, 0x15c95097, 0x7e21dd73,
		  0x15fad71b, 0x7e194584, 0x162c5a3b, 0x7e109a24,
		  0x165dd9f0, 0x7e07db52, 0x168f5632, 0x7dff0911,
		  0x16c0cef9, 0x7df62362, 0x16f2443e, 0x7ded2a47,
		  0x1723b5f9, 0x7de41dc0, 0x17552422, 0x7ddafdce,
		  0x17868eb3, 0x7dd1ca75, 0x17b7f5a3, 0x7dc883b4,
		  0x17e958ea, 0x7dbf298d, 0x181ab881, 0x7db5bc02,
		  0x184c1461, 0x7dac3b15, 0x187d6c82, 0x7da2a6c6,
		  0x18aec0db, 0x7d98ff17, 0x18e01167, 0x7d8f4409,
		  0x19115e1c, 0x7d85759f, 0x1942a6f3, 0x7d7b93da,
		  0x1973ebe6, 0x7d719eba, 0x19a52ceb, 0x7d679642,
		  0x19d669fc, 0x7d5d7a74, 0x1a07a311, 0x7d534b50,
		  0x1a38d823, 0x7d4908d9, 0x1a6a0929, 0x7d3eb30f,
		  0x1a9b361d, 0x7d3449f5, 0x1acc5ef6, 0x7d29cd8c,
		  0x1afd83ad, 0x7d1f3dd6, 0x1b2ea43a, 0x7d149ad5,
		  0x1b5fc097, 0x7d09e489, 0x1b90d8bb, 0x7cff1af5,
		  0x1bc1ec9e, 0x7cf43e1a, 0x1bf2fc3a, 0x7ce94dfb,
		  0x1c240786, 0x7cde4a98, 0x1c550e7c, 0x7cd333f3,
		  0x1c861113, 0x7cc80a0f, 0x1cb70f43, 0x7cbcccec,
		  0x1ce80906, 0x7cb17c8d, 0x1d18fe54, 0x7ca618f3,
		  0x1d49ef26, 0x7c9aa221, 0x1d7adb73, 0x7c8f1817,
		  0x1dabc334, 0x7c837ad8, 0x1ddca662, 0x7c77ca65,
		  0x1e0d84f5, 0x7c6c06c0, 0x1e3e5ee5, 0x7c602fec,
		  0x1e6f342c, 0x7c5445e9, 0x1ea004c1, 0x7c4848ba,
		  0x1ed0d09d, 0x7c3c3860, 0x1f0197b8, 0x7c3014de,
		  0x1f325a0b, 0x7c23de35, 0x1f63178f, 0x7c179467,
		  0x1f93d03c, 0x7c0b3777, 0x1fc4840a, 0x7bfec765,
		  0x1ff532f2, 0x7bf24434, 0x2025dcec, 0x7be5ade6,
		  0x205681f1, 0x7bd9047c, 0x208721f9, 0x7bcc47fa,
		  0x20b7bcfe, 0x7bbf7860, 0x20e852f6, 0x7bb295b0,
		  0x2118e3dc, 0x7ba59fee, 0x21496fa7, 0x7b989719,
		  0x2179f64f, 0x7b8b7b36, 0x21aa77cf, 0x7b7e4c45,
		  0x21daf41d, 0x7b710a49, 0x220b6b32, 0x7b63b543,
		  0x223bdd08, 0x7b564d36, 0x226c4996, 0x7b48d225,
		  0x229cb0d5, 0x7b3b4410, 0x22cd12bd, 0x7b2da2fa,
		  0x22fd6f48, 0x7b1feee5, 0x232dc66d, 0x7b1227d3,
		  0x235e1826, 0x7b044dc7, 0x238e646a, 0x7af660c2,
		  0x23beab33, 0x7ae860c7, 0x23eeec78, 0x7ada4dd8,
		  0x241f2833, 0x7acc27f7, 0x244f5e5c, 0x7abdef25,
		  0x247f8eec, 0x7aafa367, 0x24afb9da, 0x7aa144bc,
		  0x24dfdf20, 0x7a92d329, 0x250ffeb7, 0x7a844eae,
		  0x25401896, 0x7a75b74f, 0x25702cb7, 0x7a670d0d,
		  0x25a03b11, 0x7a584feb, 0x25d0439f, 0x7a497feb,
		  0x26004657, 0x7a3a9d0f, 0x26304333, 0x7a2ba75a,
		  0x26603a2c, 0x7a1c9ece, 0x26902b39, 0x7a0d836d,
		  0x26c01655, 0x79fe5539, 0x26effb76, 0x79ef1436,
		  0x271fda96, 0x79dfc064, 0x274fb3ae, 0x79d059c8,
		  0x277f86b5, 0x79c0e062, 0x27af53a6, 0x79b15435,
		  0x27df1a77, 0x79a1b545, 0x280edb23, 0x79920392,
		  0x283e95a1, 0x79823f20, 0x286e49ea, 0x797267f2,
		  0x289df7f8, 0x79627e08, 0x28cd9fc1, 0x79528167,
		  0x28fd4140, 0x79427210, 0x292cdc6d, 0x79325006,
		  0x295c7140, 0x79221b4b, 0x298bffb2, 0x7911d3e2,
		  0x29bb87bc, 0x790179cd, 0x29eb0957, 0x78f10d0f,
		  0x2a1a847b, 0x78e08dab, 0x2a49f920, 0x78cffba3,
		  0x2a796740, 0x78bf56f9, 0x2aa8ced3, 0x78ae9fb0,
		  0x2ad82fd2, 0x789dd5cb, 0x2b078a36, 0x788cf94c,
		  0x2b36ddf7, 0x787c0a36, 0x2b662b0e, 0x786b088c,
		  0x2b957173, 0x7859f44f, 0x2bc4b120, 0x7848cd83,
		  0x2bf3ea0d, 0x7837942b, 0x2c231c33, 0x78264849,
		  0x2c52478a, 0x7814e9df, 0x2c816c0c, 0x780378f1,
		  0x2cb089b1, 0x77f1f581, 0x2cdfa071, 0x77e05f91,
		  0x2d0eb046, 0x77ceb725, 0x2d3db928, 0x77bcfc3f,
		  0x2d6cbb10, 0x77ab2ee2, 0x2d9bb5f6, 0x77994f11,
		  0x2dcaa9d5, 0x77875cce, 0x2df996a3, 0x7775581d,
		  0x2e287c5a, 0x776340ff, 0x2e575af3, 0x77511778,
		  0x2e863267, 0x773edb8b, 0x2eb502ae, 0x772c8d3a,
		  0x2ee3cbc1, 0x771a2c88, 0x2f128d99, 0x7707b979,
		  0x2f41482e, 0x76f5340e, 0x2f6ffb7a, 0x76e29c4b,
		  0x2f9ea775, 0x76cff232, 0x2fcd4c19, 0x76bd35c7,
		  0x2ffbe95d, 0x76aa670d, 0x302a7f3a, 0x76978605,
		  0x30590dab, 0x768492b4, 0x308794a6, 0x76718d1c,
		  0x30b61426, 0x765e7540, 0x30e48c22, 0x764b4b23,
		  0x3112fc95, 0x76380ec8, 0x31416576, 0x7624c031,
		  0x316fc6be, 0x76115f63, 0x319e2067, 0x75fdec60,
		  0x31cc7269, 0x75ea672a, 0x31fabcbd, 0x75d6cfc5,
		  0x3228ff5c, 0x75c32634, 0x32573a3f, 0x75af6a7b,
		  0x32856d5e, 0x759b9c9b, 0x32b398b3, 0x7587bc98,
		  0x32e1bc36, 0x7573ca75, 0x330fd7e1, 0x755fc635,
		  0x333debab, 0x754bafdc, 0x336bf78f, 0x7537876c,
		  0x3399fb85, 0x75234ce8, 0x33c7f785, 0x750f0054,
		  0x33f5eb89, 0x74faa1b3, 0x3423d78a, 0x74e63108,
		  0x3451bb81, 0x74d1ae55, 0x347f9766, 0x74bd199f,
		  0x34ad6b32, 0x74a872e8, 0x34db36df, 0x7493ba34,
		  0x3508fa66, 0x747eef85, 0x3536b5be, 0x746a12df,
		  0x356468e2, 0x74552446, 0x359213c9, 0x744023bc,
		  0x35bfb66e, 0x742b1144, 0x35ed50c9, 0x7415ece2,
		  0x361ae2d3, 0x7400b69a, 0x36486c86, 0x73eb6e6e,
		  0x3675edd9, 0x73d61461, 0x36a366c6, 0x73c0a878,
		  0x36d0d746, 0x73ab2ab4, 0x36fe3f52, 0x73959b1b,
		  0x372b9ee3, 0x737ff9ae, 0x3758f5f2, 0x736a4671,
		  0x37864477, 0x73548168, 0x37b38a6d, 0x733eaa96,
		  0x37e0c7cc, 0x7328c1ff, 0x380dfc8d, 0x7312c7a5,
		  0x383b28a9, 0x72fcbb8c, 0x38684c19, 0x72e69db7,
		  0x389566d6, 0x72d06e2b, 0x38c278d9, 0x72ba2cea,
		  0x38ef821c, 0x72a3d9f7, 0x391c8297, 0x728d7557,
		  0x39497a43, 0x7276ff0d, 0x39766919, 0x7260771b,
		  0x39a34f13, 0x7249dd86, 0x39d02c2a, 0x72333251,
		  0x39fd0056, 0x721c7580, 0x3a29cb91, 0x7205a716,
		  0x3a568dd4, 0x71eec716, 0x3a834717, 0x71d7d585,
		  0x3aaff755, 0x71c0d265, 0x3adc9e86, 0x71a9bdba,
		  0x3b093ca3, 0x71929789, 0x3b35d1a5, 0x717b5fd3,
		  0x3b625d86, 0x7164169d, 0x3b8ee03e, 0x714cbbeb,
		  0x3bbb59c7, 0x71354fc0, 0x3be7ca1a, 0x711dd220,
		  0x3c143130, 0x7106430e, 0x3c408f03, 0x70eea28e,
		  0x3c6ce38a, 0x70d6f0a4, 0x3c992ec0, 0x70bf2d53,
		  0x3cc5709e, 0x70a7589f, 0x3cf1a91c, 0x708f728b,
		  0x3d1dd835, 0x70777b1c, 0x3d49fde1, 0x705f7255,
		  0x3d761a19, 0x70475839, 0x3da22cd7, 0x702f2ccd,
		  0x3dce3614, 0x7016f014, 0x3dfa35c8, 0x6ffea212,
		  0x3e262bee, 0x6fe642ca, 0x3e52187f, 0x6fcdd241,
		  0x3e7dfb73, 0x6fb5507a, 0x3ea9d4c3, 0x6f9cbd79,
		  0x3ed5a46b, 0x6f841942, 0x3f016a61, 0x6f6b63d8,
		  0x3f2d26a0, 0x6f529d40, 0x3f58d921, 0x6f39c57d,
		  0x3f8481dd, 0x6f20dc92, 0x3fb020ce, 0x6f07e285,
		  0x3fdbb5ec, 0x6eeed758, 0x40074132, 0x6ed5bb10,
		  0x4032c297, 0x6ebc8db0, 0x405e3a16, 0x6ea34f3d,
		  0x4089a7a8, 0x6e89ffb9, 0x40b50b46, 0x6e709f2a,
		  0x40e064ea, 0x6e572d93, 0x410bb48c, 0x6e3daaf8,
		  0x4136fa27, 0x6e24175c, 0x416235b2, 0x6e0a72c5,
		  0x418d6729, 0x6df0bd35, 0x41b88e84, 0x6dd6f6b1,
		  0x41e3abbc, 0x6dbd1f3c, 0x420ebecb, 0x6da336dc,
		  0x4239c7aa, 0x6d893d93, 0x4264c653, 0x6d6f3365,
		  0x428fbabe, 0x6d551858, 0x42baa4e6, 0x6d3aec6e,
		  0x42e584c3, 0x6d20afac, 0x43105a50, 0x6d066215,
		  0x433b2585, 0x6cec03af, 0x4365e65b, 0x6cd1947c,
		  0x43909ccd, 0x6cb71482, 0x43bb48d4, 0x6c9c83c3,
		  0x43e5ea68, 0x6c81e245, 0x44108184, 0x6c67300b,
		  0x443b0e21, 0x6c4c6d1a, 0x44659039, 0x6c319975,
		  0x449007c4, 0x6c16b521, 0x44ba74bd, 0x6bfbc021,
		  0x44e4d71c, 0x6be0ba7b, 0x450f2edb, 0x6bc5a431,
		  0x45397bf4, 0x6baa7d49, 0x4563be60, 0x6b8f45c7,
		  0x458df619, 0x6b73fdae, 0x45b82318, 0x6b58a503,
		  0x45e24556, 0x6b3d3bcb, 0x460c5cce, 0x6b21c208,
		  0x46366978, 0x6b0637c1, 0x46606b4e, 0x6aea9cf8,
		  0x468a624a, 0x6acef1b2, 0x46b44e65, 0x6ab335f4,
		  0x46de2f99, 0x6a9769c1, 0x470805df, 0x6a7b8d1e,
		  0x4731d131, 0x6a5fa010, 0x475b9188, 0x6a43a29a,
		  0x478546de, 0x6a2794c1, 0x47aef12c, 0x6a0b7689,
		  0x47d8906d, 0x69ef47f6, 0x48022499, 0x69d3090e,
		  0x482badab, 0x69b6b9d3, 0x48552b9b, 0x699a5a4c,
		  0x487e9e64, 0x697dea7b, 0x48a805ff, 0x69616a65,
		  0x48d16265, 0x6944da10, 0x48fab391, 0x6928397e,
		  0x4923f97b, 0x690b88b5, 0x494d341e, 0x68eec7b9,
		  0x49766373, 0x68d1f68f, 0x499f8774, 0x68b5153a,
		  0x49c8a01b, 0x689823bf, 0x49f1ad61, 0x687b2224,
		  0x4a1aaf3f, 0x685e106c, 0x4a43a5b0, 0x6840ee9b,
		  0x4a6c90ad, 0x6823bcb7, 0x4a957030, 0x68067ac3,
		  0x4abe4433, 0x67e928c5, 0x4ae70caf, 0x67cbc6c0,
		  0x4b0fc99d, 0x67ae54ba, 0x4b387af9, 0x6790d2b6,
		  0x4b6120bb, 0x677340ba, 0x4b89badd, 0x67559eca,
		  0x4bb24958, 0x6737ecea, 0x4bdacc28, 0x671a2b20,
		  0x4c034345, 0x66fc596f, 0x4c2baea9, 0x66de77dc,
		  0x4c540e4e, 0x66c0866d, 0x4c7c622d, 0x66a28524,
		  0x4ca4aa41, 0x66847408, 0x4ccce684, 0x6666531d,
		  0x4cf516ee, 0x66482267, 0x4d1d3b7a, 0x6629e1ec,
		  0x4d455422, 0x660b91af, 0x4d6d60df, 0x65ed31b5,
		  0x4d9561ac, 0x65cec204, 0x4dbd5682, 0x65b0429f,
		  0x4de53f5a, 0x6591b38c, 0x4e0d1c30, 0x657314cf,
		  0x4e34ecfc, 0x6554666d, 0x4e5cb1b9, 0x6535a86b,
		  0x4e846a60, 0x6516dacd, 0x4eac16eb, 0x64f7fd98,
		  0x4ed3b755, 0x64d910d1, 0x4efb4b96, 0x64ba147d,
		  0x4f22d3aa, 0x649b08a0, 0x4f4a4f89, 0x647bed3f,
		  0x4f71bf2e, 0x645cc260, 0x4f992293, 0x643d8806,
		  0x4fc079b1, 0x641e3e38, 0x4fe7c483, 0x63fee4f8,
		  0x500f0302, 0x63df7c4d, 0x50363529, 0x63c0043b,
		  0x505d5af1, 0x63a07cc7, 0x50847454, 0x6380e5f6,
		  0x50ab814d, 0x63613fcd, 0x50d281d5, 0x63418a50,
		  0x50f975e6, 0x6321c585, 0x51205d7b, 0x6301f171,
		  0x5147388c, 0x62e20e17, 0x516e0715, 0x62c21b7e,
		  0x5194c910, 0x62a219aa, 0x51bb7e75, 0x628208a1,
		  0x51e22740, 0x6261e866, 0x5208c36a, 0x6241b8ff,
		  0x522f52ee, 0x62217a72, 0x5255d5c5, 0x62012cc2,
		  0x527c4bea, 0x61e0cff5, 0x52a2b556, 0x61c06410,
		  0x52c91204, 0x619fe918, 0x52ef61ee, 0x617f5f12,
		  0x5315a50e, 0x615ec603, 0x533bdb5d, 0x613e1df0,
		  0x536204d7, 0x611d66de, 0x53882175, 0x60fca0d2,
		  0x53ae3131, 0x60dbcbd1, 0x53d43406, 0x60bae7e1,
		  0x53fa29ed, 0x6099f505, 0x542012e1, 0x6078f344,
		  0x5445eedb, 0x6057e2a2, 0x546bbdd7, 0x6036c325,
		  0x54917fce, 0x601594d1, 0x54b734ba, 0x5ff457ad,
		  0x54dcdc96, 0x5fd30bbc, 0x5502775c, 0x5fb1b104,
		  0x55280505, 0x5f90478a, 0x554d858d, 0x5f6ecf53,
		  0x5572f8ed, 0x5f4d4865, 0x55985f20, 0x5f2bb2c5,
		  0x55bdb81f, 0x5f0a0e77, 0x55e303e6, 0x5ee85b82,
		  0x5608426e, 0x5ec699e9, 0x562d73b2, 0x5ea4c9b3,
		  0x565297ab, 0x5e82eae5, 0x5677ae54, 0x5e60fd84,
		  0x569cb7a8, 0x5e3f0194, 0x56c1b3a1, 0x5e1cf71c,
		  0x56e6a239, 0x5dfade20, 0x570b8369, 0x5dd8b6a7,
		  0x5730572e, 0x5db680b4, 0x57551d80, 0x5d943c4e,
		  0x5779d65b, 0x5d71e979, 0x579e81b8, 0x5d4f883b,
		  0x57c31f92, 0x5d2d189a, 0x57e7afe4, 0x5d0a9a9a,
		  0x580c32a7, 0x5ce80e41, 0x5830a7d6, 0x5cc57394,
		  0x58550f6c, 0x5ca2ca99, 0x58796962, 0x5c801354,
		  0x589db5b3, 0x5c5d4dcc, 0x58c1f45b, 0x5c3a7a05,
		  0x58e62552, 0x5c179806, 0x590a4893, 0x5bf4a7d2,
		  0x592e5e19, 0x5bd1a971, 0x595265df, 0x5bae9ce7,
		  0x59765fde, 0x5b8b8239, 0x599a4c12, 0x5b68596d,
		  0x59be2a74, 0x5b452288, 0x59e1faff, 0x5b21dd90,
		  0x5a05bdae, 0x5afe8a8b, 0x5a29727b, 0x5adb297d,
		  0x5a4d1960, 0x5ab7ba6c, 0x5a70b258, 0x5a943d5e,
};

/*split radix bit reverse table for FFT of size up to 2048*/
static const uint16_t revtab[1<<12] = {
	0, 3072, 1536, 2816, 768, 3840, 1408, 2432, 384, 3456, 1920, 2752, 704, 
		3776, 1216, 2240, 192, 3264, 1728, 3008, 960, 4032, 1376, 2400, 352, 3424, 
		1888, 2656, 608, 3680, 1120, 2144, 96, 3168, 1632, 2912, 864, 3936, 1504, 
		2528, 480, 3552, 2016, 2736, 688, 3760, 1200, 2224, 176, 3248, 1712, 2992, 
		944, 4016, 1328, 2352, 304, 3376, 1840, 2608, 560, 3632, 1072, 2096, 48, 
		3120, 1584, 2864, 816, 3888, 1456, 2480, 432, 3504, 1968, 2800, 752, 3824, 
		1264, 2288, 240, 3312, 1776, 3056, 1008, 4080, 1368, 2392, 344, 3416, 1880, 
		2648, 600, 3672, 1112, 2136, 88, 3160, 1624, 2904, 856, 3928, 1496, 2520, 
		472, 3544, 2008, 2712, 664, 3736, 1176, 2200, 152, 3224, 1688, 2968, 920, 
		3992, 1304, 2328, 280, 3352, 1816, 2584, 536, 3608, 1048, 2072, 24, 3096, 
		1560, 2840, 792, 3864, 1432, 2456, 408, 3480, 1944, 2776, 728, 3800, 1240, 
		2264, 216, 3288, 1752, 3032, 984, 4056, 1400, 2424, 376, 3448, 1912, 2680, 
		632, 3704, 1144, 2168, 120, 3192, 1656, 2936, 888, 3960, 1528, 2552, 504, 
		3576, 2040, 2732, 684, 3756, 1196, 2220, 172, 3244, 1708, 2988, 940, 4012, 
		1324, 2348, 300, 3372, 1836, 2604, 556, 3628, 1068, 2092, 44, 3116, 1580, 
		2860, 812, 3884, 1452, 2476, 428, 3500, 1964, 2796, 748, 3820, 1260, 2284, 
		236, 3308, 1772, 3052, 1004, 4076, 1356, 2380, 332, 3404, 1868, 2636, 588, 
		3660, 1100, 2124, 76, 3148, 1612, 2892, 844, 3916, 1484, 2508, 460, 3532, 
		1996, 2700, 652, 3724, 1164, 2188, 140, 3212, 1676, 2956, 908, 3980, 1292, 
		2316, 268, 3340, 1804, 2572, 524, 3596, 1036, 2060, 12, 3084, 1548, 2828, 
		780, 3852, 1420, 2444, 396, 3468, 1932, 2764, 716, 3788, 1228, 2252, 204, 
		3276, 1740, 3020, 972, 4044, 1388, 2412, 364, 3436, 1900, 2668, 620, 3692, 
		1132, 2156, 108, 3180, 1644, 2924, 876, 3948, 1516, 2540, 492, 3564, 2028, 
		2748, 700, 3772, 1212, 2236, 188, 3260, 1724, 3004, 956, 4028, 1340, 2364, 
		316, 3388, 1852, 2620, 572, 3644, 1084, 2108, 60, 3132, 1596, 2876, 828, 
		3900, 1468, 2492, 444, 3516, 1980, 2812, 764, 3836, 1276, 2300, 252, 3324, 
		1788, 3068, 1020, 4092, 1366, 2390, 342, 3414, 1878, 2646, 598, 3670, 1110, 
		2134, 86, 3158, 1622, 2902, 854, 3926, 1494, 2518, 470, 3542, 2006, 2710, 
		662, 3734, 1174, 2198, 150, 3222, 1686, 2966, 918, 3990, 1302, 2326, 278, 
		3350, 1814, 2582, 534, 3606, 1046, 2070, 22, 3094, 1558, 2838, 790, 3862, 
		1430, 2454, 406, 3478, 1942, 2774, 726, 3798, 1238, 2262, 214, 3286, 1750, 
		3030, 982, 4054, 1398, 2422, 374, 3446, 1910, 2678, 630, 3702, 1142, 2166, 
		118, 3190, 1654, 2934, 886, 3958, 1526, 2550, 502, 3574, 2038, 2726, 678, 
		3750, 1190, 2214, 166, 3238, 1702, 2982, 934, 4006, 1318, 2342, 294, 3366, 
		1830, 2598, 550, 3622, 1062, 2086, 38, 3110, 1574, 2854, 806, 3878, 1446, 
		2470, 422, 3494, 1958, 2790, 742, 3814, 1254, 2278, 230, 3302, 1766, 3046, 
		998, 4070, 1350, 2374, 326, 3398, 1862, 2630, 582, 3654, 1094, 2118, 70, 
		3142, 1606, 2886, 838, 3910, 1478, 2502, 454, 3526, 1990, 2694, 646, 3718, 
		1158, 2182, 134, 3206, 1670, 2950, 902, 3974, 1286, 2310, 262, 3334, 1798, 
		2566, 518, 3590, 1030, 2054, 6, 3078, 1542, 2822, 774, 3846, 1414, 2438, 
		390, 3462, 1926, 2758, 710, 3782, 1222, 2246, 198, 3270, 1734, 3014, 966, 
		4038, 1382, 2406, 358, 3430, 1894, 2662, 614, 3686, 1126, 2150, 102, 3174, 
		1638, 2918, 870, 3942, 1510, 2534, 486, 3558, 2022, 2742, 694, 3766, 1206, 
		2230, 182, 3254, 1718, 2998, 950, 4022, 1334, 2358, 310, 3382, 1846, 2614, 
		566, 3638, 1078, 2102, 54, 3126, 1590, 2870, 822, 3894, 1462, 2486, 438, 
		3510, 1974, 2806, 758, 3830, 1270, 2294, 246, 3318, 1782, 3062, 1014, 4086, 
		1374, 2398, 350, 3422, 1886, 2654, 606, 3678, 1118, 2142, 94, 3166, 1630, 
		2910, 862, 3934, 1502, 2526, 478, 3550, 2014, 2718, 670, 3742, 1182, 2206, 
		158, 3230, 1694, 2974, 926, 3998, 1310, 2334, 286, 3358, 1822, 2590, 542, 
		3614, 1054, 2078, 30, 3102, 1566, 2846, 798, 3870, 1438, 2462, 414, 3486, 
		1950, 2782, 734, 3806, 1246, 2270, 222, 3294, 1758, 3038, 990, 4062, 1406, 
		2430, 382, 3454, 1918, 2686, 638, 3710, 1150, 2174, 126, 3198, 1662, 2942, 
		894, 3966, 1534, 2558, 510, 3582, 2046, 2731, 683, 3755, 1195, 2219, 171, 
		3243, 1707, 2987, 939, 4011, 1323, 2347, 299, 3371, 1835, 2603, 555, 3627, 
		1067, 2091, 43, 3115, 1579, 2859, 811, 3883, 1451, 2475, 427, 3499, 1963, 
		2795, 747, 3819, 1259, 2283, 235, 3307, 1771, 3051, 1003, 4075, 1355, 2379, 
		331, 3403, 1867, 2635, 587, 3659, 1099, 2123, 75, 3147, 1611, 2891, 843, 
		3915, 1483, 2507, 459, 3531, 1995, 2699, 651, 3723, 1163, 2187, 139, 3211, 
		1675, 2955, 907, 3979, 1291, 2315, 267, 3339, 1803, 2571, 523, 3595, 1035, 
		2059, 11, 3083, 1547, 2827, 779, 3851, 1419, 2443, 395, 3467, 1931, 2763, 
		715, 3787, 1227, 2251, 203, 3275, 1739, 3019, 971, 4043, 1387, 2411, 363, 
		3435, 1899, 2667, 619, 3691, 1131, 2155, 107, 3179, 1643, 2923, 875, 3947, 
		1515, 2539, 491, 3563, 2027, 2747, 699, 3771, 1211, 2235, 187, 3259, 1723, 
		3003, 955, 4027, 1339, 2363, 315, 3387, 1851, 2619, 571, 3643, 1083, 2107, 
		59, 3131, 1595, 2875, 827, 3899, 1467, 2491, 443, 3515, 1979, 2811, 763, 
		3835, 1275, 2299, 251, 3323, 1787, 3067, 1019, 4091, 1363, 2387, 339, 3411, 
		1875, 2643, 595, 3667, 1107, 2131, 83, 3155, 1619, 2899, 851, 3923, 1491, 
		2515, 467, 3539, 2003, 2707, 659, 3731, 1171, 2195, 147, 3219, 1683, 2963, 
		915, 3987, 1299, 2323, 275, 3347, 1811, 2579, 531, 3603, 1043, 2067, 19, 
		3091, 1555, 2835, 787, 3859, 1427, 2451, 403, 3475, 1939, 2771, 723, 3795, 
		1235, 2259, 211, 3283, 1747, 3027, 979, 4051, 1395, 2419, 371, 3443, 1907, 
		2675, 627, 3699, 1139, 2163, 115, 3187, 1651, 2931, 883, 3955, 1523, 2547, 
		499, 3571, 2035, 2723, 675, 3747, 1187, 2211, 163, 3235, 1699, 2979, 931, 
		4003, 1315, 2339, 291, 3363, 1827, 2595, 547, 3619, 1059, 2083, 35, 3107, 
		1571, 2851, 803, 3875, 1443, 2467, 419, 3491, 1955, 2787, 739, 3811, 1251, 
		2275, 227, 3299, 1763, 3043, 995, 4067, 1347, 2371, 323, 3395, 1859, 2627, 
		579, 3651, 1091, 2115, 67, 3139, 1603, 2883, 835, 3907, 1475, 2499, 451, 
		3523, 1987, 2691, 643, 3715, 1155, 2179, 131, 3203, 1667, 2947, 899, 3971, 
		1283, 2307, 259, 3331, 1795, 2563, 515, 3587, 1027, 2051, 3, 3075, 1539, 
		2819, 771, 3843, 1411, 2435, 387, 3459, 1923, 2755, 707, 3779, 1219, 2243, 
		195, 3267, 1731, 3011, 963, 4035, 1379, 2403, 355, 3427, 1891, 2659, 611, 
		3683, 1123, 2147, 99, 3171, 1635, 2915, 867, 3939, 1507, 2531, 483, 3555, 
		2019, 2739, 691, 3763, 1203, 2227, 179, 3251, 1715, 2995, 947, 4019, 1331, 
		2355, 307, 3379, 1843, 2611, 563, 3635, 1075, 2099, 51, 3123, 1587, 2867, 
		819, 3891, 1459, 2483, 435, 3507, 1971, 2803, 755, 3827, 1267, 2291, 243, 
		3315, 1779, 3059, 1011, 4083, 1371, 2395, 347, 3419, 1883, 2651, 603, 3675, 
		1115, 2139, 91, 3163, 1627, 2907, 859, 3931, 1499, 2523, 475, 3547, 2011, 
		2715, 667, 3739, 1179, 2203, 155, 3227, 1691, 2971, 923, 3995, 1307, 2331, 
		283, 3355, 1819, 2587, 539, 3611, 1051, 2075, 27, 3099, 1563, 2843, 795, 
		3867, 1435, 2459, 411, 3483, 1947, 2779, 731, 3803, 1243, 2267, 219, 3291, 
		1755, 3035, 987, 4059, 1403, 2427, 379, 3451, 1915, 2683, 635, 3707, 1147, 
		2171, 123, 3195, 1659, 2939, 891, 3963, 1531, 2555, 507, 3579, 2043, 2735, 
		687, 3759, 1199, 2223, 175, 3247, 1711, 2991, 943, 4015, 1327, 2351, 303, 
		3375, 1839, 2607, 559, 3631, 1071, 2095, 47, 3119, 1583, 2863, 815, 3887, 
		1455, 2479, 431, 3503, 1967, 2799, 751, 3823, 1263, 2287, 239, 3311, 1775, 
		3055, 1007, 4079, 1359, 2383, 335, 3407, 1871, 2639, 591, 3663, 1103, 2127, 
		79, 3151, 1615, 2895, 847, 3919, 1487, 2511, 463, 3535, 1999, 2703, 655, 
		3727, 1167, 2191, 143, 3215, 1679, 2959, 911, 3983, 1295, 2319, 271, 3343, 
		1807, 2575, 527, 3599, 1039, 2063, 15, 3087, 1551, 2831, 783, 3855, 1423, 
		2447, 399, 3471, 1935, 2767, 719, 3791, 1231, 2255, 207, 3279, 1743, 3023, 
		975, 4047, 1391, 2415, 367, 3439, 1903, 2671, 623, 3695, 1135, 2159, 111, 
		3183, 1647, 2927, 879, 3951, 1519, 2543, 495, 3567, 2031, 2751, 703, 3775, 
		1215, 2239, 191, 3263, 1727, 3007, 959, 4031, 1343, 2367, 319, 3391, 1855, 
		2623, 575, 3647, 1087, 2111, 63, 3135, 1599, 2879, 831, 3903, 1471, 2495, 
		447, 3519, 1983, 2815, 767, 3839, 1279, 2303, 255, 3327, 1791, 3071, 1023, 
		4095, 1365, 2389, 341, 3413, 1877, 2645, 597, 3669, 1109, 2133, 85, 3157, 
		1621, 2901, 853, 3925, 1493, 2517, 469, 3541, 2005, 2709, 661, 3733, 1173, 
		2197, 149, 3221, 1685, 2965, 917, 3989, 1301, 2325, 277, 3349, 1813, 2581, 
		533, 3605, 1045, 2069, 21, 3093, 1557, 2837, 789, 3861, 1429, 2453, 405, 
		3477, 1941, 2773, 725, 3797, 1237, 2261, 213, 3285, 1749, 3029, 981, 4053, 
		1397, 2421, 373, 3445, 1909, 2677, 629, 3701, 1141, 2165, 117, 3189, 1653, 
		2933, 885, 3957, 1525, 2549, 501, 3573, 2037, 2725, 677, 3749, 1189, 2213, 
		165, 3237, 1701, 2981, 933, 4005, 1317, 2341, 293, 3365, 1829, 2597, 549, 
		3621, 1061, 2085, 37, 3109, 1573, 2853, 805, 3877, 1445, 2469, 421, 3493, 
		1957, 2789, 741, 3813, 1253, 2277, 229, 3301, 1765, 3045, 997, 4069, 1349, 
		2373, 325, 3397, 1861, 2629, 581, 3653, 1093, 2117, 69, 3141, 1605, 2885, 
		837, 3909, 1477, 2501, 453, 3525, 1989, 2693, 645, 3717, 1157, 2181, 133, 
		3205, 1669, 2949, 901, 3973, 1285, 2309, 261, 3333, 1797, 2565, 517, 3589, 
		1029, 2053, 5, 3077, 1541, 2821, 773, 3845, 1413, 2437, 389, 3461, 1925, 
		2757, 709, 3781, 1221, 2245, 197, 3269, 1733, 3013, 965, 4037, 1381, 2405, 
		357, 3429, 1893, 2661, 613, 3685, 1125, 2149, 101, 3173, 1637, 2917, 869, 
		3941, 1509, 2533, 485, 3557, 2021, 2741, 693, 3765, 1205, 2229, 181, 3253, 
		1717, 2997, 949, 4021, 1333, 2357, 309, 3381, 1845, 2613, 565, 3637, 1077, 
		2101, 53, 3125, 1589, 2869, 821, 3893, 1461, 2485, 437, 3509, 1973, 2805, 
		757, 3829, 1269, 2293, 245, 3317, 1781, 3061, 1013, 4085, 1373, 2397, 349, 
		3421, 1885, 2653, 605, 3677, 1117, 2141, 93, 3165, 1629, 2909, 861, 3933, 
		1501, 2525, 477, 3549, 2013, 2717, 669, 3741, 1181, 2205, 157, 3229, 1693, 
		2973, 925, 3997, 1309, 2333, 285, 3357, 1821, 2589, 541, 3613, 1053, 2077, 
		29, 3101, 1565, 2845, 797, 3869, 1437, 2461, 413, 3485, 1949, 2781, 733, 
		3805, 1245, 2269, 221, 3293, 1757, 3037, 989, 4061, 1405, 2429, 381, 3453, 
		1917, 2685, 637, 3709, 1149, 2173, 125, 3197, 1661, 2941, 893, 3965, 1533, 
		2557, 509, 3581, 2045, 2729, 681, 3753, 1193, 2217, 169, 3241, 1705, 2985, 
		937, 4009, 1321, 2345, 297, 3369, 1833, 2601, 553, 3625, 1065, 2089, 41, 
		3113, 1577, 2857, 809, 3881, 1449, 2473, 425, 3497, 1961, 2793, 745, 3817, 
		1257, 2281, 233, 3305, 1769, 3049, 1001, 4073, 1353, 2377, 329, 3401, 1865, 
		2633, 585, 3657, 1097, 2121, 73, 3145, 1609, 2889, 841, 3913, 1481, 2505, 
		457, 3529, 1993, 2697, 649, 3721, 1161, 2185, 137, 3209, 1673, 2953, 905, 
		3977, 1289, 2313, 265, 3337, 1801, 2569, 521, 3593, 1033, 2057, 9, 3081, 
		1545, 2825, 777, 3849, 1417, 2441, 393, 3465, 1929, 2761, 713, 3785, 1225, 
		2249, 201, 3273, 1737, 3017, 969, 4041, 1385, 2409, 361, 3433, 1897, 2665, 
		617, 3689, 1129, 2153, 105, 3177, 1641, 2921, 873, 3945, 1513, 2537, 489, 
		3561, 2025, 2745, 697, 3769, 1209, 2233, 185, 3257, 1721, 3001, 953, 4025, 
		1337, 2361, 313, 3385, 1849, 2617, 569, 3641, 1081, 2105, 57, 3129, 1593, 
		2873, 825, 3897, 1465, 2489, 441, 3513, 1977, 2809, 761, 3833, 1273, 2297, 
		249, 3321, 1785, 3065, 1017, 4089, 1361, 2385, 337, 3409, 1873, 2641, 593, 
		3665, 1105, 2129, 81, 3153, 1617, 2897, 849, 3921, 1489, 2513, 465, 3537, 
		2001, 2705, 657, 3729, 1169, 2193, 145, 3217, 1681, 2961, 913, 3985, 1297, 
		2321, 273, 3345, 1809, 2577, 529, 3601, 1041, 2065, 17, 3089, 1553, 2833, 
		785, 3857, 1425, 2449, 401, 3473, 1937, 2769, 721, 3793, 1233, 2257, 209, 
		3281, 1745, 3025, 977, 4049, 1393, 2417, 369, 3441, 1905, 2673, 625, 3697, 
		1137, 2161, 113, 3185, 1649, 2929, 881, 3953, 1521, 2545, 497, 3569, 2033, 
		2721, 673, 3745, 1185, 2209, 161, 3233, 1697, 2977, 929, 4001, 1313, 2337, 
		289, 3361, 1825, 2593, 545, 3617, 1057, 2081, 33, 3105, 1569, 2849, 801, 
		3873, 1441, 2465, 417, 3489, 1953, 2785, 737, 3809, 1249, 2273, 225, 3297, 
		1761, 3041, 993, 4065, 1345, 2369, 321, 3393, 1857, 2625, 577, 3649, 1089, 
		2113, 65, 3137, 1601, 2881, 833, 3905, 1473, 2497, 449, 3521, 1985, 2689, 
		641, 3713, 1153, 2177, 129, 3201, 1665, 2945, 897, 3969, 1281, 2305, 257, 
		3329, 1793, 2561, 513, 3585, 1025, 2049, 1, 3073, 1537, 2817, 769, 3841, 
		1409, 2433, 385, 3457, 1921, 2753, 705, 3777, 1217, 2241, 193, 3265, 1729, 
		3009, 961, 4033, 1377, 2401, 353, 3425, 1889, 2657, 609, 3681, 1121, 2145, 
		97, 3169, 1633, 2913, 865, 3937, 1505, 2529, 481, 3553, 2017, 2737, 689, 
		3761, 1201, 2225, 177, 3249, 1713, 2993, 945, 4017, 1329, 2353, 305, 3377, 
		1841, 2609, 561, 3633, 1073, 2097, 49, 3121, 1585, 2865, 817, 3889, 1457, 
		2481, 433, 3505, 1969, 2801, 753, 3825, 1265, 2289, 241, 3313, 1777, 3057, 
		1009, 4081, 1369, 2393, 345, 3417, 1881, 2649, 601, 3673, 1113, 2137, 89, 
		3161, 1625, 2905, 857, 3929, 1497, 2521, 473, 3545, 2009, 2713, 665, 3737, 
		1177, 2201, 153, 3225, 1689, 2969, 921, 3993, 1305, 2329, 281, 3353, 1817, 
		2585, 537, 3609, 1049, 2073, 25, 3097, 1561, 2841, 793, 3865, 1433, 2457, 
		409, 3481, 1945, 2777, 729, 3801, 1241, 2265, 217, 3289, 1753, 3033, 985, 
		4057, 1401, 2425, 377, 3449, 1913, 2681, 633, 3705, 1145, 2169, 121, 3193, 
		1657, 2937, 889, 3961, 1529, 2553, 505, 3577, 2041, 2733, 685, 3757, 1197, 
		2221, 173, 3245, 1709, 2989, 941, 4013, 1325, 2349, 301, 3373, 1837, 2605, 
		557, 3629, 1069, 2093, 45, 3117, 1581, 2861, 813, 3885, 1453, 2477, 429, 
		3501, 1965, 2797, 749, 3821, 1261, 2285, 237, 3309, 1773, 3053, 1005, 4077, 
		1357, 2381, 333, 3405, 1869, 2637, 589, 3661, 1101, 2125, 77, 3149, 1613, 
		2893, 845, 3917, 1485, 2509, 461, 3533, 1997, 2701, 653, 3725, 1165, 2189, 
		141, 3213, 1677, 2957, 909, 3981, 1293, 2317, 269, 3341, 1805, 2573, 525, 
		3597, 1037, 2061, 13, 3085, 1549, 2829, 781, 3853, 1421, 2445, 397, 3469, 
		1933, 2765, 717, 3789, 1229, 2253, 205, 3277, 1741, 3021, 973, 4045, 1389, 
		2413, 365, 3437, 1901, 2669, 621, 3693, 1133, 2157, 109, 3181, 1645, 2925, 
		877, 3949, 1517, 2541, 493, 3565, 2029, 2749, 701, 3773, 1213, 2237, 189, 
		3261, 1725, 3005, 957, 4029, 1341, 2365, 317, 3389, 1853, 2621, 573, 3645, 
		1085, 2109, 61, 3133, 1597, 2877, 829, 3901, 1469, 2493, 445, 3517, 1981, 
		2813, 765, 3837, 1277, 2301, 253, 3325, 1789, 3069, 1021, 4093, 1367, 2391, 
		343, 3415, 1879, 2647, 599, 3671, 1111, 2135, 87, 3159, 1623, 2903, 855, 
		3927, 1495, 2519, 471, 3543, 2007, 2711, 663, 3735, 1175, 2199, 151, 3223, 
		1687, 2967, 919, 3991, 1303, 2327, 279, 3351, 1815, 2583, 535, 3607, 1047, 
		2071, 23, 3095, 1559, 2839, 791, 3863, 1431, 2455, 407, 3479, 1943, 2775, 
		727, 3799, 1239, 2263, 215, 3287, 1751, 3031, 983, 4055, 1399, 2423, 375, 
		3447, 1911, 2679, 631, 3703, 1143, 2167, 119, 3191, 1655, 2935, 887, 3959, 
		1527, 2551, 503, 3575, 2039, 2727, 679, 3751, 1191, 2215, 167, 3239, 1703, 
		2983, 935, 4007, 1319, 2343, 295, 3367, 1831, 2599, 551, 3623, 1063, 2087, 
		39, 3111, 1575, 2855, 807, 3879, 1447, 2471, 423, 3495, 1959, 2791, 743, 
		3815, 1255, 2279, 231, 3303, 1767, 3047, 999, 4071, 1351, 2375, 327, 3399, 
		1863, 2631, 583, 3655, 1095, 2119, 71, 3143, 1607, 2887, 839, 3911, 1479, 
		2503, 455, 3527, 1991, 2695, 647, 3719, 1159, 2183, 135, 3207, 1671, 2951, 
		903, 3975, 1287, 2311, 263, 3335, 1799, 2567, 519, 3591, 1031, 2055, 7, 
		3079, 1543, 2823, 775, 3847, 1415, 2439, 391, 3463, 1927, 2759, 711, 3783, 
		1223, 2247, 199, 3271, 1735, 3015, 967, 4039, 1383, 2407, 359, 3431, 1895, 
		2663, 615, 3687, 1127, 2151, 103, 3175, 1639, 2919, 871, 3943, 1511, 2535, 
		487, 3559, 2023, 2743, 695, 3767, 1207, 2231, 183, 3255, 1719, 2999, 951, 
		4023, 1335, 2359, 311, 3383, 1847, 2615, 567, 3639, 1079, 2103, 55, 3127, 
		1591, 2871, 823, 3895, 1463, 2487, 439, 3511, 1975, 2807, 759, 3831, 1271, 
		2295, 247, 3319, 1783, 3063, 1015, 4087, 1375, 2399, 351, 3423, 1887, 2655, 
		607, 3679, 1119, 2143, 95, 3167, 1631, 2911, 863, 3935, 1503, 2527, 479, 
		3551, 2015, 2719, 671, 3743, 1183, 2207, 159, 3231, 1695, 2975, 927, 3999, 
		1311, 2335, 287, 3359, 1823, 2591, 543, 3615, 1055, 2079, 31, 3103, 1567, 
		2847, 799, 3871, 1439, 2463, 415, 3487, 1951, 2783, 735, 3807, 1247, 2271, 
		223, 3295, 1759, 3039, 991, 4063, 1407, 2431, 383, 3455, 1919, 2687, 639, 
		3711, 1151, 2175, 127, 3199, 1663, 2943, 895, 3967, 1535, 2559, 511, 3583, 
		2047, 2730, 682, 3754, 1194, 2218, 170, 3242, 1706, 2986, 938, 4010, 1322, 
		2346, 298, 3370, 1834, 2602, 554, 3626, 1066, 2090, 42, 3114, 1578, 2858, 
		810, 3882, 1450, 2474, 426, 3498, 1962, 2794, 746, 3818, 1258, 2282, 234, 
		3306, 1770, 3050, 1002, 4074, 1354, 2378, 330, 3402, 1866, 2634, 586, 3658, 
		1098, 2122, 74, 3146, 1610, 2890, 842, 3914, 1482, 2506, 458, 3530, 1994, 
		2698, 650, 3722, 1162, 2186, 138, 3210, 1674, 2954, 906, 3978, 1290, 2314, 
		266, 3338, 1802, 2570, 522, 3594, 1034, 2058, 10, 3082, 1546, 2826, 778, 
		3850, 1418, 2442, 394, 3466, 1930, 2762, 714, 3786, 1226, 2250, 202, 3274, 
		1738, 3018, 970, 4042, 1386, 2410, 362, 3434, 1898, 2666, 618, 3690, 1130, 
		2154, 106, 3178, 1642, 2922, 874, 3946, 1514, 2538, 490, 3562, 2026, 2746, 
		698, 3770, 1210, 2234, 186, 3258, 1722, 3002, 954, 4026, 1338, 2362, 314, 
		3386, 1850, 2618, 570, 3642, 1082, 2106, 58, 3130, 1594, 2874, 826, 3898, 
		1466, 2490, 442, 3514, 1978, 2810, 762, 3834, 1274, 2298, 250, 3322, 1786, 
		3066, 1018, 4090, 1362, 2386, 338, 3410, 1874, 2642, 594, 3666, 1106, 2130, 
		82, 3154, 1618, 2898, 850, 3922, 1490, 2514, 466, 3538, 2002, 2706, 658, 
		3730, 1170, 2194, 146, 3218, 1682, 2962, 914, 3986, 1298, 2322, 274, 3346, 
		1810, 2578, 530, 3602, 1042, 2066, 18, 3090, 1554, 2834, 786, 3858, 1426, 
		2450, 402, 3474, 1938, 2770, 722, 3794, 1234, 2258, 210, 3282, 1746, 3026, 
		978, 4050, 1394, 2418, 370, 3442, 1906, 2674, 626, 3698, 1138, 2162, 114, 
		3186, 1650, 2930, 882, 3954, 1522, 2546, 498, 3570, 2034, 2722, 674, 3746, 
		1186, 2210, 162, 3234, 1698, 2978, 930, 4002, 1314, 2338, 290, 3362, 1826, 
		2594, 546, 3618, 1058, 2082, 34, 3106, 1570, 2850, 802, 3874, 1442, 2466, 
		418, 3490, 1954, 2786, 738, 3810, 1250, 2274, 226, 3298, 1762, 3042, 994, 
		4066, 1346, 2370, 322, 3394, 1858, 2626, 578, 3650, 1090, 2114, 66, 3138, 
		1602, 2882, 834, 3906, 1474, 2498, 450, 3522, 1986, 2690, 642, 3714, 1154, 
		2178, 130, 3202, 1666, 2946, 898, 3970, 1282, 2306, 258, 3330, 1794, 2562, 
		514, 3586, 1026, 2050, 2, 3074, 1538, 2818, 770, 3842, 1410, 2434, 386, 
		3458, 1922, 2754, 706, 3778, 1218, 2242, 194, 3266, 1730, 3010, 962, 4034, 
		1378, 2402, 354, 3426, 1890, 2658, 610, 3682, 1122, 2146, 98, 3170, 1634, 
		2914, 866, 3938, 1506, 2530, 482, 3554, 2018, 2738, 690, 3762, 1202, 2226, 
		178, 3250, 1714, 2994, 946, 4018, 1330, 2354, 306, 3378, 1842, 2610, 562, 
		3634, 1074, 2098, 50, 3122, 1586, 2866, 818, 3890, 1458, 2482, 434, 3506, 
		1970, 2802, 754, 3826, 1266, 2290, 242, 3314, 1778, 3058, 1010, 4082, 1370, 
		2394, 346, 3418, 1882, 2650, 602, 3674, 1114, 2138, 90, 3162, 1626, 2906, 
		858, 3930, 1498, 2522, 474, 3546, 2010, 2714, 666, 3738, 1178, 2202, 154, 
		3226, 1690, 2970, 922, 3994, 1306, 2330, 282, 3354, 1818, 2586, 538, 3610, 
		1050, 2074, 26, 3098, 1562, 2842, 794, 3866, 1434, 2458, 410, 3482, 1946, 
		2778, 730, 3802, 1242, 2266, 218, 3290, 1754, 3034, 986, 4058, 1402, 2426, 
		378, 3450, 1914, 2682, 634, 3706, 1146, 2170, 122, 3194, 1658, 2938, 890, 
		3962, 1530, 2554, 506, 3578, 2042, 2734, 686, 3758, 1198, 2222, 174, 3246, 
		1710, 2990, 942, 4014, 1326, 2350, 302, 3374, 1838, 2606, 558, 3630, 1070, 
		2094, 46, 3118, 1582, 2862, 814, 3886, 1454, 2478, 430, 3502, 1966, 2798, 
		750, 3822, 1262, 2286, 238, 3310, 1774, 3054, 1006, 4078, 1358, 2382, 334, 
		3406, 1870, 2638, 590, 3662, 1102, 2126, 78, 3150, 1614, 2894, 846, 3918, 
		1486, 2510, 462, 3534, 1998, 2702, 654, 3726, 1166, 2190, 142, 3214, 1678, 
		2958, 910, 3982, 1294, 2318, 270, 3342, 1806, 2574, 526, 3598, 1038, 2062, 
		14, 3086, 1550, 2830, 782, 3854, 1422, 2446, 398, 3470, 1934, 2766, 718, 
		3790, 1230, 2254, 206, 3278, 1742, 3022, 974, 4046, 1390, 2414, 366, 3438, 
		1902, 2670, 622, 3694, 1134, 2158, 110, 3182, 1646, 2926, 878, 3950, 1518, 
		2542, 494, 3566, 2030, 2750, 702, 3774, 1214, 2238, 190, 3262, 1726, 3006, 
		958, 4030, 1342, 2366, 318, 3390, 1854, 2622, 574, 3646, 1086, 2110, 62, 
		3134, 1598, 2878, 830, 3902, 1470, 2494, 446, 3518, 1982, 2814, 766, 3838, 
		1278, 2302, 254, 3326, 1790, 3070, 1022, 4094, 1364, 2388, 340, 3412, 1876, 
		2644, 596, 3668, 1108, 2132, 84, 3156, 1620, 2900, 852, 3924, 1492, 2516, 
		468, 3540, 2004, 2708, 660, 3732, 1172, 2196, 148, 3220, 1684, 2964, 916, 
		3988, 1300, 2324, 276, 3348, 1812, 2580, 532, 3604, 1044, 2068, 20, 3092, 
		1556, 2836, 788, 3860, 1428, 2452, 404, 3476, 1940, 2772, 724, 3796, 1236, 
		2260, 212, 3284, 1748, 3028, 980, 4052, 1396, 2420, 372, 3444, 1908, 2676, 
		628, 3700, 1140, 2164, 116, 3188, 1652, 2932, 884, 3956, 1524, 2548, 500, 
		3572, 2036, 2724, 676, 3748, 1188, 2212, 164, 3236, 1700, 2980, 932, 4004, 
		1316, 2340, 292, 3364, 1828, 2596, 548, 3620, 1060, 2084, 36, 3108, 1572, 
		2852, 804, 3876, 1444, 2468, 420, 3492, 1956, 2788, 740, 3812, 1252, 2276, 
		228, 3300, 1764, 3044, 996, 4068, 1348, 2372, 324, 3396, 1860, 2628, 580, 
		3652, 1092, 2116, 68, 3140, 1604, 2884, 836, 3908, 1476, 2500, 452, 3524, 
		1988, 2692, 644, 3716, 1156, 2180, 132, 3204, 1668, 2948, 900, 3972, 1284, 
		2308, 260, 3332, 1796, 2564, 516, 3588, 1028, 2052, 4, 3076, 1540, 2820, 
		772, 3844, 1412, 2436, 388, 3460, 1924, 2756, 708, 3780, 1220, 2244, 196, 
		3268, 1732, 3012, 964, 4036, 1380, 2404, 356, 3428, 1892, 2660, 612, 3684, 
		1124, 2148, 100, 3172, 1636, 2916, 868, 3940, 1508, 2532, 484, 3556, 2020, 
		2740, 692, 3764, 1204, 2228, 180, 3252, 1716, 2996, 948, 4020, 1332, 2356, 
		308, 3380, 1844, 2612, 564, 3636, 1076, 2100, 52, 3124, 1588, 2868, 820, 
		3892, 1460, 2484, 436, 3508, 1972, 2804, 756, 3828, 1268, 2292, 244, 3316, 
		1780, 3060, 1012, 4084, 1372, 2396, 348, 3420, 1884, 2652, 604, 3676, 1116, 
		2140, 92, 3164, 1628, 2908, 860, 3932, 1500, 2524, 476, 3548, 2012, 2716, 
		668, 3740, 1180, 2204, 156, 3228, 1692, 2972, 924, 3996, 1308, 2332, 284, 
		3356, 1820, 2588, 540, 3612, 1052, 2076, 28, 3100, 1564, 2844, 796, 3868, 
		1436, 2460, 412, 3484, 1948, 2780, 732, 3804, 1244, 2268, 220, 3292, 1756, 
		3036, 988, 4060, 1404, 2428, 380, 3452, 1916, 2684, 636, 3708, 1148, 2172, 
		124, 3196, 1660, 2940, 892, 3964, 1532, 2556, 508, 3580, 2044, 2728, 680, 
		3752, 1192, 2216, 168, 3240, 1704, 2984, 936, 4008, 1320, 2344, 296, 3368, 
		1832, 2600, 552, 3624, 1064, 2088, 40, 3112, 1576, 2856, 808, 3880, 1448, 
		2472, 424, 3496, 1960, 2792, 744, 3816, 1256, 2280, 232, 3304, 1768, 3048, 
		1000, 4072, 1352, 2376, 328, 3400, 1864, 2632, 584, 3656, 1096, 2120, 72, 
		3144, 1608, 2888, 840, 3912, 1480, 2504, 456, 3528, 1992, 2696, 648, 3720, 
		1160, 2184, 136, 3208, 1672, 2952, 904, 3976, 1288, 2312, 264, 3336, 1800, 
		2568, 520, 3592, 1032, 2056, 8, 3080, 1544, 2824, 776, 3848, 1416, 2440, 
		392, 3464, 1928, 2760, 712, 3784, 1224, 2248, 200, 3272, 1736, 3016, 968, 
		4040, 1384, 2408, 360, 3432, 1896, 2664, 616, 3688, 1128, 2152, 104, 3176, 
		1640, 2920, 872, 3944, 1512, 2536, 488, 3560, 2024, 2744, 696, 3768, 1208, 
		2232, 184, 3256, 1720, 3000, 952, 4024, 1336, 2360, 312, 3384, 1848, 2616, 
		568, 3640, 1080, 2104, 56, 3128, 1592, 2872, 824, 3896, 1464, 2488, 440, 
		3512, 1976, 2808, 760, 3832, 1272, 2296, 248, 3320, 1784, 3064, 1016, 4088, 
		1360, 2384, 336, 3408, 1872, 2640, 592, 3664, 1104, 2128, 80, 3152, 1616, 
		2896, 848, 3920, 1488, 2512, 464, 3536, 2000, 2704, 656, 3728, 1168, 2192, 
		144, 3216, 1680, 2960, 912, 3984, 1296, 2320, 272, 3344, 1808, 2576, 528, 
		3600, 1040, 2064, 16, 3088, 1552, 2832, 784, 3856, 1424, 2448, 400, 3472, 
		1936, 2768, 720, 3792, 1232, 2256, 208, 3280, 1744, 3024, 976, 4048, 1392, 
		2416, 368, 3440, 1904, 2672, 624, 3696, 1136, 2160, 112, 3184, 1648, 2928, 
		880, 3952, 1520, 2544, 496, 3568, 2032, 2720, 672, 3744, 1184, 2208, 160, 
		3232, 1696, 2976, 928, 4000, 1312, 2336, 288, 3360, 1824, 2592, 544, 3616, 
		1056, 2080, 32, 3104, 1568, 2848, 800, 3872, 1440, 2464, 416, 3488, 1952, 
		2784, 736, 3808, 1248, 2272, 224, 3296, 1760, 3040, 992, 4064, 1344, 2368, 
		320, 3392, 1856, 2624, 576, 3648, 1088, 2112, 64, 3136, 1600, 2880, 832, 
		3904, 1472, 2496, 448, 3520, 1984, 2688, 640, 3712, 1152, 2176, 128, 3200, 
		1664, 2944, 896, 3968, 1280, 2304, 256, 3328, 1792, 2560, 512, 3584, 1024, 
		2048};
		
		/* Use to give gcc hints on which branch is most likely taken */
#if defined(__GNUC__) && __GNUC__ >= 3
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif
		
		/* replaced XPROD32 with a macro to avoid memory reference
		_x, _y are the results (must be l-values) */
#define XPROD32(_a, _b, _t, _v, _x, _y)     \
		{ (_x)=FIX_MUL32(_a,_t)+FIX_MUL32(_b,_v);       \
		(_y)=FIX_MUL32(_b,_t)-FIX_MUL32(_a,_v); }
		
		
#if 0
#ifdef __i386__
		
#define XPROD31(_a, _b, _t, _v, _x, _y)     \
		{ *(_x)=(FIX_MUL32(_a,_t)+FIX_MUL32(_b,_v))<<1;      \
		*(_y)=(FIX_MUL32(_b,_t)-FIX_MUL32(_a,_v))<<1; }
#define XNPROD31(_a, _b, _t, _v, _x, _y)    \
		{ *(_x)=(FIX_MUL32(_a,_t)-FIX_MUL32(_b,_v))<<1;      \
		*(_y)=(FIX_MUL32(_b,_t)+FIX_MUL32(_a,_v))<<1; }		
#else
		
		static inline void XPROD31(int32_t a, int32_t b,
			int32_t t, int32_t v,
			int32_t *x, int32_t *y)
		{
			*x = (FIX_MUL32(a, t) + FIX_MUL32(b, v))<<1;
			*y = (FIX_MUL32(b, t) - FIX_MUL32(a, v))<<1;
		}
		
		static inline void XNPROD31(int32_t a, int32_t b,
			int32_t  t, int32_t  v,
			int32_t *x, int32_t *y)
		{
			*x = (FIX_MUL32(a, t) - FIX_MUL32(b, v))<<1;
			*y = (FIX_MUL32(b, t) + FIX_MUL32(a, v))<<1;
		}
#endif
#endif
		
#if 0
#define XPROD31_R(_a, _b, _t, _v, _x, _y)\
		{\
		_x = (FIX_MUL32(_a, _t) + FIX_MUL32(_b, _v))<<1;\
		_y = (FIX_MUL32(_b, _t) - FIX_MUL32(_a, _v))<<1;\
		}
		
#define XNPROD31_R(_a, _b, _t, _v, _x, _y)\
		{\
		_x = (FIX_MUL32(_a, _t) - FIX_MUL32(_b, _v))<<1;\
		_y = (FIX_MUL32(_b, _t) + FIX_MUL32(_a, _v))<<1;\
		}
#else
#define XPROD31_R(_a, _b, _t, _v, _x, _y)\
		{\
		_x = (MADD64(FIX64_MUL(_a, _t), _b, _v)>>32)<<1;\
		_y = (MSUB64(FIX64_MUL(_b, _t), _a, _v)>>32)<<1;\
		}
		
#define XNPROD31_R(_a, _b, _t, _v, _x, _y)\
		{\
		_x = (MSUB64(FIX64_MUL(_a, _t), _b, _v)>>32)<<1;\
		_y = (MADD64(FIX64_MUL(_b, _t), _a, _v)>>32)<<1;\
		}
#endif
		
		/* constants for fft_16 (same constants as in mdct_arm.S ... ) */
#define cPI1_8 (0x7641af3d) /* cos(pi/8) s.31 */
#define cPI2_8 (0x5a82799a) /* cos(2pi/8) = 1/sqrt(2) s.31 */
#define cPI3_8 (0x30fbc54d) /* cos(3pi/8) s.31 */
		
		
#define BF(x,y,a,b) {\
    x = a - b;\
    y = a + b;\
		}
		
#define BF_REV(x,y,a,b) {\
    x = a + b;\
    y = a - b;\
		}
		
#define BUTTERFLIES(a0,a1,a2,a3) {\
		{\
        FixFFTSample temp1,temp2;\
        BF(temp1, temp2, t5, t1);\
        BF(a2.re, a0.re, a0.re, temp2);\
        BF(a3.im, a1.im, a1.im, temp1);\
		}\
		{\
        FixFFTSample temp1,temp2;\
        BF(temp1, temp2, t2, t6);\
        BF(a3.re, a1.re, a1.re, temp1);\
        BF(a2.im, a0.im, a0.im, temp2);\
		}\
		}
		
		// force loading all the inputs before storing any.
		// this is slightly slower for small data, but avoids store->load aliasing
		// for addresses separated by large powers of 2.
#define BUTTERFLIES_BIG(a0,a1,a2,a3) {\
    FixFFTSample r0=a0.re, i0=a0.im, r1=a1.re, i1=a1.im;\
		{\
        FixFFTSample temp1, temp2;\
        BF(temp1, temp2, t5, t1);\
        BF(a2.re, a0.re, r0, temp2);\
        BF(a3.im, a1.im, i1, temp1);\
		}\
		{\
        FixFFTSample temp1, temp2;\
        BF(temp1, temp2, t2, t6);\
        BF(a3.re, a1.re, r1, temp1);\
        BF(a2.im, a0.im, i0, temp2);\
		}\
		}
		
		
		static INLINE void TRANSFORM(FixFFTComplex * z, unsigned int n, FixFFTSample wre, FixFFTSample wim)
		{
			register FixFFTSample t1,t2,t5,t6,r_re,r_im;
			r_re = z[n*2].re;
			r_im = z[n*2].im;
			XPROD31_R(r_re, r_im, wre, wim, t1,t2);
			r_re = z[n*3].re;
			r_im = z[n*3].im;
			XNPROD31_R(r_re, r_im, wre, wim, t5,t6);
			BUTTERFLIES(z[0],z[n],z[n*2],z[n*3]);
		}
		
		static INLINE void TRANSFORM_W01(FixFFTComplex * z, unsigned int n, const FixFFTSample * w)
		{
			register const FixFFTSample wre=w[0],wim=w[1];
			register FixFFTSample t1,t2,t5,t6,r_re,r_im;
			r_re = z[n*2].re;
			r_im = z[n*2].im;
			XPROD31_R(r_re, r_im, wre, wim, t1,t2);
			r_re = z[n*3].re;
			r_im = z[n*3].im;
			XNPROD31_R(r_re, r_im, wre, wim, t5,t6);
			BUTTERFLIES(z[0],z[n],z[n*2],z[n*3]);
		}
		
		static INLINE void TRANSFORM_W10(FixFFTComplex * z, unsigned int n, const FixFFTSample * w)
		{
			register const FixFFTSample wim=w[0],wre=w[1];
			register FixFFTSample t1,t2,t5,t6,r_re,r_im;
			r_re = z[n*2].re;
			r_im = z[n*2].im;
			XPROD31_R(r_re, r_im, wre, wim, t1,t2);
			r_re = z[n*3].re;
			r_im = z[n*3].im;
			XNPROD31_R(r_re, r_im, wre, wim, t5,t6);
			BUTTERFLIES(z[0],z[n],z[n*2],z[n*3]);
		}
		
		static INLINE void TRANSFORM_EQUAL(FixFFTComplex * z, unsigned int n)
		{
			register FixFFTSample t1,t2,t5,t6,temp1,temp2;
			register FixFFTSample * my_z = (FixFFTSample *)(z);
			my_z += n*4;
			t2    = MULT31(my_z[0], cPI2_8);
			temp1 = MULT31(my_z[1], cPI2_8);
			my_z += n*2;
			temp2 = MULT31(my_z[0], cPI2_8);
			t5    = MULT31(my_z[1], cPI2_8);
			t1 = ( temp1 + t2 );
			t2 = ( temp1 - t2 );
			t6 = ( temp2 + t5 );
			t5 = ( temp2 - t5 );
			my_z -= n*6;
			BUTTERFLIES(z[0],z[n],z[n*2],z[n*3]);
		}
		
		static INLINE void TRANSFORM_ZERO(FixFFTComplex * z, unsigned int n)
		{
			FixFFTSample t1,t2,t5,t6;
			t1 = z[n*2].re;
			t2 = z[n*2].im;
			t5 = z[n*3].re;
			t6 = z[n*3].im;
			BUTTERFLIES(z[0],z[n],z[n*2],z[n*3]);
		}
		
		/* z[0...8n-1], w[1...2n-1] */
		static void pass(FixFFTComplex *z_arg, unsigned int STEP_arg, unsigned int n_arg)
		{
			register FixFFTComplex * z = z_arg;
			register unsigned int STEP = STEP_arg;
			register unsigned int n = n_arg;
			
			register const FixFFTSample *w = sincos_lookup0+STEP;
			/* wre = *(wim+1) .  ordering is sin,cos */
			register const FixFFTSample *w_end = sincos_lookup0+1024;
			
			/* first two are special (well, first one is special, but we need to do pairs) */
			TRANSFORM_ZERO(z,n);
			z++;
			TRANSFORM_W10(z,n,w);
			w += STEP;
			/* first pass forwards through sincos_lookup0*/
			do {
				z++;
				TRANSFORM_W10(z,n,w);
				w += STEP;
				z++;
				TRANSFORM_W10(z,n,w);
				w += STEP;
			} while(LIKELY(w < w_end));
			/* second half: pass backwards through sincos_lookup0*/
			/* wim and wre are now in opposite places so ordering now [0],[1] */
			w_end=sincos_lookup0;
			while(LIKELY(w>w_end))
			{
				z++;
				TRANSFORM_W01(z,n,w);
				w -= STEP;
				z++;
				TRANSFORM_W01(z,n,w);
				w -= STEP;
			}
		}
		
#define DECL_FFT(n,n2,n4)\
	static void fft##n(FixFFTComplex *z)\
		{\
		fft##n2(z);\
		fft##n4(z+n4*2);\
		fft##n4(z+n4*3);\
		pass(z,8192/n,n4);\
		}
		
		static INLINE void fft4(FixFFTComplex *z)
		{
			FixFFTSample t1, t2, t3, t4, t5, t6, t7, t8;
			
			BF(t3, t1, z[0].re, z[1].re); 
			BF(t8, t6, z[3].re, z[2].re); 
			
			BF(z[2].re, z[0].re, t1, t6); 
			
			BF(t4, t2, z[0].im, z[1].im); 
			BF(t7, t5, z[2].im, z[3].im); 
			
			BF(z[3].im, z[1].im, t4, t8); 
			BF(z[3].re, z[1].re, t3, t7); 
			BF(z[2].im, z[0].im, t2, t5); 
		}
		
		static void fft4_dispatch(FixFFTComplex *z)
		{
			fft4(z);
		}
		
		static INLINE void fft8(FixFFTComplex *z)
		{
			FixFFTSample t1,t2,t3,t4,t7,t8;
			fft4(z);
			
			BF(t1, z[5].re, z[4].re, -z[5].re);
			BF(t2, z[5].im, z[4].im, -z[5].im);
			BF(t3, z[7].re, z[6].re, -z[7].re);
			BF(t4, z[7].im, z[6].im, -z[7].im);
			BF(t8, t1, t3, t1);
			BF(t7, t2, t2, t4);
			BF(z[4].re, z[0].re, z[0].re, t1);
			BF(z[4].im, z[0].im, z[0].im, t2);
			BF(z[6].re, z[2].re, z[2].re, t7);
			BF(z[6].im, z[2].im, z[2].im, t8);
			
			z++;
			TRANSFORM_EQUAL(z,2);
		}
		
		static void fft8_dispatch(FixFFTComplex *z)
		{
			fft8(z);
		}
		
#if !CONFIG_SMALL
		static void fft16(FixFFTComplex *z)
		{
			fft8(z);
			fft4(z+8);
			fft4(z+12);
			
			TRANSFORM_ZERO(z,4);
			z+=2;
			TRANSFORM_EQUAL(z,4);
			z-=1;
			TRANSFORM(z,4,cPI1_8,cPI3_8);
			z+=2;
			TRANSFORM(z,4,cPI3_8,cPI1_8);
		}
#else
		DECL_FFT(16,8,4)
#endif
			DECL_FFT(32,16,8)
			DECL_FFT(64,32,16)
			DECL_FFT(128,64,32)
			DECL_FFT(256,128,64)
			DECL_FFT(512,256,128)
			DECL_FFT(1024,512,256)
			DECL_FFT(2048,1024,512)
			DECL_FFT(4096,2048,1024)
			
			static void (*fixfft_dispatch[])(FixFFTComplex*) = {
			fft4_dispatch, fft8_dispatch, fft16, fft32, fft64, fft128, fft256, fft512, fft1024,
				fft2048, fft4096
		};
		
		static void fix_fft_calc_c(int nbits, FixFFTComplex *z)
		{
			fixfft_dispatch[nbits-2](z);
		}
		
		
		/**
		* init IMDCT computation.
		*/
		av_cold int fix_imdct_init(FixFFTContext *s, int nbits)
		{
			int n, n4, i;
			
			memset_lib(s, 0, sizeof(*s));
			n = 1 << nbits;
			s->mdct_bits = nbits;
			s->nbits= nbits-2;
			s->mdct_size = n;
			n4 = n >> 2;
			
			
			s->tcos = av_malloc(n/2 * sizeof(FixFFTSample));
			if (!s->tcos)
				return -1;
			
			s->tsin = s->tcos + n4;
			
			for(i=0;i<n4;i++) {
				uint32_t fixalpha = (0xffffffff>>nbits)*(uint32_t)i + (0xffffffff>>(nbits+3)) ;
				s->tsin[i] = (-fsincos(fixalpha, &s->tcos[i]));
				s->tcos[i] = -s->tcos[i];
			}
			return 0;
		}
		
		av_cold void fix_imdct_end(FixFFTContext *s)
		{
		   if(s!=NULL && s->tcos!=NULL)
			av_free(s->tcos);
		}
		
		
		/**
		*@brief Uninitialize the decoder and free all resources.
		*@param avctx codec context
		*@return 0 on success, < 0 otherwise
		*/
		int decode_end(AudioContext *avctx)
		{
			WMAProDecodeCtx *s = avctx->priv_data;
			int i;
			
			for (i = 0; i < WMAPRO_BLOCK_SIZES; i++)
			{
			   if(s !=NULL)
				fix_imdct_end(&s->fixmdct_ctx[i]);
			}
		    if(avctx->priv_data!=NULL)
		       free_lib(avctx->priv_data);
			free_lib(avctx);
			return 0;
		}
		
		/**
		*@brief Get the samples per frame for this stream.
		*@param sample_rate output sample_rate
		*@param version wma version
		*@param decode_flags codec compression features
		*@return log2 of the number of output samples per frame
		*/
		int av_cold wmapro_get_frame_len_bits(int sample_rate, int version,
			unsigned int decode_flags)
		{
			
			int frame_len_bits;
			
			if (sample_rate <= 16000) {
				frame_len_bits = 9;
			} else if (sample_rate <= 22050 ||
				(sample_rate <= 32000 && version == 1)) {
				frame_len_bits = 10;
			} else if (sample_rate <= 48000) {
				frame_len_bits = 11;
			} else if (sample_rate <= 96000) {
				frame_len_bits = 12;
			} else {
				frame_len_bits = 13;
			}
			
			if (version == 3) {
				int tmp = decode_flags & 0x6;
				if (tmp == 0x2) {
					++frame_len_bits;
				} else if (tmp == 0x4) {
					--frame_len_bits;
				} else if (tmp == 0x6) {
					frame_len_bits -= 2;
				}
			}
			
			return frame_len_bits;
		}
		
		
		/**
		*@brief Initialize the decoder.
		*@param avctx codec context
		*@return 0 on success, -1 otherwise
		*/
		int decode_init(AudioContext *avctx)
		{
			//static WMAProDecodeCtx static_ctx;//这里使用了静态变量来保存, 注意不要使用多线程
			//WMAProDecodeCtx *s = avctx->priv_data = &static_ctx;
			WMAProDecodeCtx *s = avctx->priv_data =malloc(sizeof(WMAProDecodeCtx));
			uint8_t *edata_ptr = avctx->extradata;
			unsigned int channel_mask;
			int i;
			int log2_max_num_subframes;
			int num_possible_block_sizes;

			if(s!=NULL)
				memset(s,0,sizeof(WMAProDecodeCtx));
	
			s->avctx = avctx;
			init_put_bits(&s->pb, s->frame_data, MAX_FRAMESIZE);
			
			avctx->sample_fmt = SAMPLE_FMT_S16;
			
			if (avctx->extradata_size >= 18) {
				s->decode_flags    = AV_RL16(edata_ptr+14);
				channel_mask       = AV_RL32(edata_ptr+2);
				s->bits_per_sample = AV_RL16(edata_ptr);
				/** dump the extradata */
				for (i = 0; i < avctx->extradata_size; i++);
				//dprintf(avctx, "[%x] ", avctx->extradata[i]);
				//dprintf(avctx, "\n");
				
			} else {
				av_log(avctx, AV_LOG_WARNING, "Unknown extradata size\n");
				//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
				return WMAPRO_ERR_InvalidData;
			}
			
			/** generic init */
			s->log2_frame_size = av_log2(avctx->block_align) + 4;
			
			/** frame info */
			s->skip_frame  = 1; /* skip first frame */
			s->packet_loss = 1;
			s->len_prefix  = (s->decode_flags & 0x40);
			
			/** get frame len */
			s->samples_per_frame = 1 << wmapro_get_frame_len_bits(avctx->sample_rate,
				3, s->decode_flags);
			
			/** init previous block len */
			for (i = 0; i < avctx->channels; i++)
				s->channel[i].prev_block_len = s->samples_per_frame;
			
			/** subframe info */
			log2_max_num_subframes       = ((s->decode_flags & 0x38) >> 3);
			s->max_num_subframes         = 1 << log2_max_num_subframes;
			if (s->max_num_subframes == 16 || s->max_num_subframes == 4)
				s->max_subframe_len_bit = 1;
			s->subframe_len_bits = av_log2(log2_max_num_subframes) + 1;
			
			num_possible_block_sizes     = log2_max_num_subframes + 1;
			s->min_samples_per_subframe  = s->samples_per_frame / s->max_num_subframes;
			s->dynamic_range_compression = (s->decode_flags & 0x80);
			
			if (s->max_num_subframes > MAX_SUBFRAMES) {
				av_log(avctx, AV_LOG_ERROR, "invalid number of subframes %i\n",
					s->max_num_subframes);
				//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
				return WMAPRO_ERR_InvalidData;//LYXWMAPRO_ERR_InvalidData;
			}
			
			s->num_channels = avctx->channels;
			
			/** extract lfe channel position */
			s->lfe_channel = -1;
			
			if (channel_mask & 8) {
				unsigned int mask;
				for (mask = 1; mask < 16; mask <<= 1) {
					if (channel_mask & mask)
						++s->lfe_channel;
				}
			}
			
			if (s->num_channels < 0) {
				av_log(avctx, AV_LOG_ERROR, "invalid number of channels %d\n", s->num_channels);
				//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
				return WMAPRO_ERR_InvalidData;//LYXWMAPRO_ERR_InvalidData;
			} else if (s->num_channels > WMAPRO_MAX_CHANNELS) {
				av_log(avctx, AV_LOG_WARNING, "unsupported number of channels\n");
				return WMAPRO_ERR_InvalidData1;//AVERROR_PATCHWELCOME;
			}
			
			INIT_VLC_STATIC(&sf_vlc, SCALEVLCBITS, HUFF_SCALE_SIZE,
				scale_huffbits, 1, 1,
				scale_huffcodes, 2, 2, 616);
			
			INIT_VLC_STATIC(&sf_rl_vlc, VLCBITS, HUFF_SCALE_RL_SIZE,
				scale_rl_huffbits, 1, 1,
				scale_rl_huffcodes, 4, 4, 1406);
			
			INIT_VLC_STATIC(&coef_vlc[0], VLCBITS, HUFF_COEF0_SIZE,
				coef0_huffbits, 1, 1,
				coef0_huffcodes, 4, 4, 2108);
			
			INIT_VLC_STATIC(&coef_vlc[1], VLCBITS, HUFF_COEF1_SIZE,
				coef1_huffbits, 1, 1,
				coef1_huffcodes, 4, 4, 3912);
			
			INIT_VLC_STATIC(&vec4_vlc, VLCBITS, HUFF_VEC4_SIZE,
				vec4_huffbits, 1, 1,
				vec4_huffcodes, 2, 2, 604);
			
			INIT_VLC_STATIC(&vec2_vlc, VLCBITS, HUFF_VEC2_SIZE,
				vec2_huffbits, 1, 1,
				vec2_huffcodes, 2, 2, 562);
			
			INIT_VLC_STATIC(&vec1_vlc, VLCBITS, HUFF_VEC1_SIZE,
				vec1_huffbits, 1, 1,
				vec1_huffcodes, 2, 2, 562);
			
				/** calculate number of scale factor bands and their offsets
			for every possible block size */
			for (i = 0; i < num_possible_block_sizes; i++) {
				int subframe_len = s->samples_per_frame >> i;
				int x;
				int band = 1;
				
				s->sfb_offsets[i][0] = 0;
				
				for (x = 0; x < MAX_BANDS-1 && s->sfb_offsets[i][band - 1] < subframe_len; x++) {
					int offset = (subframe_len * 2 * critical_freq[x])
						/ s->avctx->sample_rate + 2;
					offset &= ~3;
					if (offset > s->sfb_offsets[i][band - 1])
						s->sfb_offsets[i][band++] = offset;
				}
				s->sfb_offsets[i][band - 1] = subframe_len;
				s->num_sfb[i]               = band - 1;
			}
			
			
			/** Scale factors can be shared between blocks of different size
			as every block has a different scale factor band layout.
			The matrix sf_offsets is needed to find the correct scale factor.
			*/
			
			for (i = 0; i < num_possible_block_sizes; i++) {
				int b;
				for (b = 0; b < s->num_sfb[i]; b++) {
					int x;
					int offset = ((s->sfb_offsets[i][b]
						+ s->sfb_offsets[i][b + 1] - 1) << i) >> 1;
					for (x = 0; x < num_possible_block_sizes; x++) {
						int v = 0;
						while (s->sfb_offsets[x][v + 1] << x < offset)
							++v;
						s->sf_offsets[i][x][b] = v;
					}
				}
			}
			
			/** init MDCT, FIXME: only init needed sizes */
			for (i = 0; i < WMAPRO_BLOCK_SIZES; i++)
			{
				fix_imdct_init(&s->fixmdct_ctx[i], WMAPRO_BLOCK_MIN_BITS+1+i);
			}
			
			/** init MDCT windows: simple sinus window */
			for (i = 0; i < WMAPRO_BLOCK_SIZES; i++) {
				const int win_idx = WMAPRO_BLOCK_MAX_BITS - i;
				s->windows[WMAPRO_BLOCK_SIZES - i - 1] = fix_sine_windows[win_idx];
			}
			
			/** calculate subwoofer cutoff values */
			for (i = 0; i < num_possible_block_sizes; i++) {
				int block_size = s->samples_per_frame >> i;
				int cutoff = (440*block_size + 3 * (s->avctx->sample_rate >> 1) - 1)
					/ s->avctx->sample_rate;
				s->subwoofer_cutoffs[i] = av_clip(cutoff, 4, block_size);
			}
			
			/** calculate sine values for the decorrelation matrix */
			for (i = 0; i < 33; i++)
			{
				sin64[i] = fsincos(i*((uint32_t)(0xffffffff>>7)), NULL);
			}
			
			avctx->channel_layout = channel_mask;
			return 0;
}

/**
*@brief Decode the subframe length.
*@param s context
*@param offset sample offset in the frame
*@return decoded subframe length on success, < 0 in case of an error
*/
static int decode_subframe_length(WMAProDecodeCtx *s, int offset)
{
    int frame_len_shift = 0;
    int subframe_len;
	
    /** no need to read from the bitstream when only one length is possible */
    if (offset == s->samples_per_frame - s->min_samples_per_subframe)
        return s->min_samples_per_subframe;
	
    /** 1 bit indicates if the subframe is of maximum length */
    if (s->max_subframe_len_bit) {
        if (get_bits1(&s->gb))
            frame_len_shift = 1 + get_bits(&s->gb, s->subframe_len_bits-1);
    } else
        frame_len_shift = get_bits(&s->gb, s->subframe_len_bits);
	
    subframe_len = s->samples_per_frame >> frame_len_shift;
	
    /** sanity check the length */
    if (subframe_len < s->min_samples_per_subframe ||
        subframe_len > s->samples_per_frame) {
        av_log(s->avctx, AV_LOG_ERROR, "broken frame: subframe_len %i\n",
			subframe_len);
		//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
        return WMAPRO_ERR_InvalidData;
    }
    return subframe_len;
}

/**
*@brief Decode how the data in the frame is split into subframes.
*       Every WMA frame contains the encoded data for a fixed number of
*       samples per channel. The data for every channel might be split
*       into several subframes. This function will reconstruct the list of
*       subframes for every channel.
*
*       If the subframes are not evenly split, the algorithm estimates the
*       channels with the lowest number of total samples.
*       Afterwards, for each of these channels a bit is read from the
*       bitstream that indicates if the channel contains a subframe with the
*       next subframe size that is going to be read from the bitstream or not.
*       If a channel contains such a subframe, the subframe size gets added to
*       the channel's subframe list.
*       The algorithm repeats these steps until the frame is properly divided
*       between the individual channels.
*
*@param s context
*@return 0 on success, < 0 in case of an error
*/
static int decode_tilehdr(WMAProDecodeCtx *s)
{
    uint16_t num_samples[WMAPRO_MAX_CHANNELS];        /** sum of samples for all currently known subframes of a channel */
    uint8_t  contains_subframe[WMAPRO_MAX_CHANNELS];  /** flag indicating if a channel contains the current subframe */
    int channels_for_cur_subframe = s->num_channels;  /** number of channels that contain the current subframe */
    int fixed_channel_layout = 0;                     /** flag indicating that all channels use the same subframe offsets and sizes */
    int min_channel_len = 0;                          /** smallest sum of samples (channels with this length will be processed first) */
    int c;
	
    /* Should never consume more than 3073 bits (256 iterations for the
	* while loop when always the minimum amount of 128 samples is substracted
	* from missing samples in the 8 channel case).
	* 1 + BLOCK_MAX_SIZE * MAX_CHANNELS / BLOCK_MIN_SIZE * (MAX_CHANNELS  + 4)
	*/
	
    /** reset tiling information */
    for (c = 0; c < s->num_channels; c++)
        s->channel[c].num_subframes = 0;
	
    memset_lib(num_samples, 0, sizeof(num_samples));
	
    if (s->max_num_subframes == 1 || get_bits1(&s->gb))
        fixed_channel_layout = 1;
	
    /** loop until the frame data is split between the subframes */
    do {
        int subframe_len;
		
        /** check which channels contain the subframe */
        for (c = 0; c < s->num_channels; c++) {
            if (num_samples[c] == min_channel_len) {
                if (fixed_channel_layout || channels_for_cur_subframe == 1 ||
					(min_channel_len == s->samples_per_frame - s->min_samples_per_subframe))
                    contains_subframe[c] = 1;
                else
                    contains_subframe[c] = get_bits1(&s->gb);
            } else
                contains_subframe[c] = 0;
        }
		
        /** get subframe length, subframe_len == 0 is not allowed */
        if ((subframe_len = decode_subframe_length(s, min_channel_len)) <= 0){
			//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
            return WMAPRO_ERR_InvalidData;
        }
		
        /** add subframes to the individual channels and find new min_channel_len */
        min_channel_len += subframe_len;
        for (c = 0; c < s->num_channels; c++) {
            WMAProChannelCtx* chan = &s->channel[c];
			
            if (contains_subframe[c]) {
                if (chan->num_subframes >= MAX_SUBFRAMES) {
                    av_log(s->avctx, AV_LOG_ERROR,
						"broken frame: num subframes > 31\n");
					//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
                    return WMAPRO_ERR_InvalidData;
                }
                chan->subframe_len[chan->num_subframes] = subframe_len;
                num_samples[c] += subframe_len;
                ++chan->num_subframes;
                if (num_samples[c] > s->samples_per_frame) {
                    av_log(s->avctx, AV_LOG_ERROR, "broken frame: "
						"channel len > samples_per_frame\n");
					//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
                    return WMAPRO_ERR_InvalidData;
                }
            } else if (num_samples[c] <= min_channel_len) {
                if (num_samples[c] < min_channel_len) {
                    channels_for_cur_subframe = 0;
                    min_channel_len = num_samples[c];
                }
                ++channels_for_cur_subframe;
            }
        }
    } while (min_channel_len < s->samples_per_frame);
	
    for (c = 0; c < s->num_channels; c++) {
        int i;
        int offset = 0;
        for (i = 0; i < s->channel[c].num_subframes; i++) {
            //dprintf(s->avctx, "frame[%i] channel[%i] subframe[%i]"
            //        " len %i\n", s->frame_num, c, i,
            //        s->channel[c].subframe_len[i]);
            s->channel[c].subframe_offset[i] = offset;
            offset += s->channel[c].subframe_len[i];
        }
    }
	
    return 0;
}

/**
*@brief Calculate a decorrelation matrix from the bitstream parameters.
*@param s codec context
*@param chgroup channel group for which the matrix needs to be calculated
*/
static void decode_decorrelation_matrix(WMAProDecodeCtx *s,
                                        WMAProChannelGrp *chgroup)
{
    int i;
    int offset = 0;
    int8_t rotation_offset[WMAPRO_MAX_CHANNELS * WMAPRO_MAX_CHANNELS];
    memset_lib(chgroup->decorrelation_matrix, 0, s->num_channels *
		s->num_channels * sizeof(*chgroup->decorrelation_matrix));
	
    for (i = 0; i < chgroup->num_channels * (chgroup->num_channels - 1) >> 1; i++)
        rotation_offset[i] = get_bits(&s->gb, 6);
	
    for (i = 0; i < chgroup->num_channels; i++)
        chgroup->decorrelation_matrix[chgroup->num_channels * i + i] =
		get_bits1(&s->gb) ? Q0_CONST(1.0) : Q0_CONST(-1.0);
	
    for (i = 1; i < chgroup->num_channels; i++) {
        int x;
        for (x = 0; x < i; x++) {
            int y;
            for (y = 0; y < i + 1; y++) {
                int32_t v1 = (chgroup->decorrelation_matrix[x * chgroup->num_channels + y]);
                int32_t v2 = (chgroup->decorrelation_matrix[i * chgroup->num_channels + y]);
                int n = rotation_offset[offset + x];
                int32_t sinv;
                int32_t cosv;
				
                if (n < 32) {
                    sinv = (sin64[n]);
                    cosv = (sin64[32 - n]);
                } else {
                    sinv = (sin64[64 -  n]);
                    cosv = (-sin64[n  - 32]);
                }
                chgroup->decorrelation_matrix[y + x * chgroup->num_channels] = (MSUB64(FIX64_MUL(v1, sinv), v2, cosv)>>32)<<1;
                chgroup->decorrelation_matrix[y + i * chgroup->num_channels] = (MADD64(FIX64_MUL(v1, cosv), v2, sinv)>>32)<<1;
            }
        }
        offset += i;
    }
}

/**
*@brief Decode channel transformation parameters
*@param s codec context
*@return 0 in case of success, < 0 in case of bitstream errors
*/
static int decode_channel_transform(WMAProDecodeCtx* s)
{
    int i;
    /* should never consume more than 1921 bits for the 8 channel case
	* 1 + MAX_CHANNELS * (MAX_CHANNELS + 2 + 3 * MAX_CHANNELS * MAX_CHANNELS
	* + MAX_CHANNELS + MAX_BANDS + 1)
	*/
	
    /** in the one channel case channel transforms are pointless */
    s->num_chgroups = 0;
    if (s->num_channels > 1) {
        int remaining_channels = s->channels_for_cur_subframe;
		
        if (get_bits1(&s->gb)) {
            av_log(s->avctx, AV_LOG_WARNING, 
				"unsupported channel transform bit\n");
			//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
            return WMAPRO_ERR_InvalidData;
        }
		
        for (s->num_chgroups = 0; remaining_channels &&
			s->num_chgroups < s->channels_for_cur_subframe; s->num_chgroups++) {
            WMAProChannelGrp* chgroup = &s->chgroup[s->num_chgroups];
            int64_t** fixchannel_data = chgroup->fixchannel_data;
            chgroup->num_channels = 0;
            chgroup->transform = 0;
			
            /** decode channel mask */
            if (remaining_channels > 2) {
                for (i = 0; i < s->channels_for_cur_subframe; i++) {
                    int channel_idx = s->channel_indexes_for_cur_subframe[i];
                    if (!s->channel[channel_idx].grouped
                        && get_bits1(&s->gb)) {
                        ++chgroup->num_channels;
                        s->channel[channel_idx].grouped = 1;
                        *fixchannel_data++ = s->channel[channel_idx].fixcoeffs;
                    }
                }
            } else {
                chgroup->num_channels = remaining_channels;
                for (i = 0; i < s->channels_for_cur_subframe; i++) {
                    int channel_idx = s->channel_indexes_for_cur_subframe[i];
                    if (!s->channel[channel_idx].grouped)
                    {
                        *fixchannel_data++ = s->channel[channel_idx].fixcoeffs;
                    }
                    s->channel[channel_idx].grouped = 1;
                }
            }
			
            /** decode transform type */
            if (chgroup->num_channels == 2) {
                if (get_bits1(&s->gb)) {
                    if (get_bits1(&s->gb)) {
                        av_log(s->avctx, AV_LOG_WARNING,
							"unsupported channel transform type\n");
                    }
                } else {
                    chgroup->transform = 1;
                    if (s->num_channels == 2) {
                        chgroup->decorrelation_matrix[0] = Q0_CONST( 1.0);
                        chgroup->decorrelation_matrix[1] = Q0_CONST(-1.0);
                        chgroup->decorrelation_matrix[2] = Q0_CONST( 1.0);
                        chgroup->decorrelation_matrix[3] = Q0_CONST( 1.0);
                    } else {
                        /** cos(pi/4) */
                        chgroup->decorrelation_matrix[0] = Q0_CONST( 0.70703125);
                        chgroup->decorrelation_matrix[1] = Q0_CONST(-0.70703125);
                        chgroup->decorrelation_matrix[2] = Q0_CONST( 0.70703125);
                        chgroup->decorrelation_matrix[3] = Q0_CONST( 0.70703125);
                    }
                }
            } else if (chgroup->num_channels > 2) {
                if (get_bits1(&s->gb)) {
                    chgroup->transform = 1;
                    if (get_bits1(&s->gb)) {
                        decode_decorrelation_matrix(s, chgroup);
                    } else {
                        /** FIXME: more than 6 coupled channels not supported */
                        if (chgroup->num_channels > 6) {
                            av_log(s->avctx, AV_LOG_WARNING,
								"coupled channels > 6\n");
                        } else {
                            memcpy_lib(chgroup->decorrelation_matrix,
								fixdefault_decorrelation[chgroup->num_channels],
								chgroup->num_channels * chgroup->num_channels *
								sizeof(*chgroup->decorrelation_matrix));
                        }
                    }
                }
            }
			
            /** decode transform on / off */
            if (chgroup->transform) {
                if (!get_bits1(&s->gb)) {
                    int i;
                    /** transform can be enabled for individual bands */
                    for (i = 0; i < s->num_bands; i++) {
                        chgroup->transform_band[i] = get_bits1(&s->gb);
                    }
                } else {
                    memset_lib(chgroup->transform_band, 1, s->num_bands);
                }
            }
            remaining_channels -= chgroup->num_channels;
        }
    }
    return 0;
}

/**
* Decode an uncompressed coefficient.
* @param gb GetBitContext
* @return the decoded coefficient
*/
unsigned int wmapro_get_large_val(GetBitContext* gb)
{
    /** consumes up to 34 bits */
    int n_bits = 8;
    /** decode length */
    if (get_bits1(gb)) {
        n_bits += 8;
        if (get_bits1(gb)) {
            n_bits += 8;
            if (get_bits1(gb)) {
                n_bits += 7;
            }
        }
    }
    return get_bits_long(gb, n_bits);
}


static int fix_wmapro_run_level_decode(AudioContext* avctx, GetBitContext* gb,
									   VLC *vlc,
									   const int32_t *level_table, const uint16_t *run_table,
									   int64_t *ptr64, int offset,
									   int num_coefs, int block_len, int frame_len_bits,
									   int coef_nb_bits)
{
    int code, level, sign;
    const unsigned int coef_mask = block_len - 1;
    I64 tmp64;
    for (; offset < num_coefs; offset++) {
        code = get_vlc2(gb, vlc->table, VLCBITS, VLCMAX);
        if (code > 1) {
            /** normal code */
            offset += run_table[code];
            sign = get_bits1(gb) - 1;
            tmp64.r.hi32 = (level_table[code]^sign) - sign;
            tmp64.r.lo32 = 0;
            ptr64[offset & coef_mask] = tmp64.w64;
        } else if (code == 1) {
            /** EOB */
            break;
        } else {
            /** escape */
            level = wmapro_get_large_val(gb);
            /** escape decode */
            if (get_bits1(gb)) {
                if (get_bits1(gb)) {
                    if (get_bits1(gb)) {
                        av_log(avctx,AV_LOG_ERROR,
                            "broken escape sequence\n");
                        return -1;
                    } else
                        offset += get_bits(gb, frame_len_bits) + 4;
                } else
                    offset += get_bits(gb, 2) + 1;
            }
            sign = get_bits1(gb) - 1;
            tmp64.r.hi32 = (level^sign) - sign;
            tmp64.r.lo32 = 0;
            ptr64[offset & coef_mask] = tmp64.w64;
        }
    }
    /** NOTE: EOB can be omitted */
    if (offset > num_coefs) {
        av_log(avctx, AV_LOG_ERROR, "overflow in spectral RLE, ignoring\n");
        return -1;
    }
	
    return 0;
}

/**
*@brief Extract the coefficients from the bitstream.
*@param s codec context
*@param c current channel number
*@return 0 on success, < 0 in case of bitstream errors
*/
static int decode_coeffs(WMAProDecodeCtx *s, int c)
{
/* Integers 0..15 as single-precision floats.  The table saves a
costly int to float conversion, and storing the values as
	integers allows fast sign-flipping. */
    static const int32_t fixval_tab[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    int vlctable;
    VLC* vlc;
    WMAProChannelCtx* ci = &s->channel[c];
    int rl_mode = 0;
    int cur_coeff = 0;
    int num_zeros = 0;
    const uint16_t* run;
    const int* level;
	
    //dprintf(s->avctx, "decode coefficients for channel %i\n", c);
	
    vlctable = get_bits1(&s->gb);
    vlc = &coef_vlc[vlctable];
	
    if (vlctable) {
        run = coef1_run;
        level = fixcoef1_level;
    } else {
        run = coef0_run;
        level = fixcoef0_level;
    }
	
    /** decode vector coefficients (consumes up to 167 bits per iteration for
	4 vector coded large values) */
    while ((s->transmit_num_vec_coeffs || !rl_mode) &&
		(cur_coeff + 3 < ci->num_vec_coeffs)) {
        int32_t vals[4];
        int i;
        unsigned int idx;
		
        idx = get_vlc2(&s->gb, vec4_vlc.table, VLCBITS, VEC4MAXDEPTH);
		
        if (idx == HUFF_VEC4_SIZE - 1) {
            for (i = 0; i < 4; i += 2) {
                idx = get_vlc2(&s->gb, vec2_vlc.table, VLCBITS, VEC2MAXDEPTH);
                if (idx == HUFF_VEC2_SIZE - 1) {
                    int v0, v1;
                    v0 = get_vlc2(&s->gb, vec1_vlc.table, VLCBITS, VEC1MAXDEPTH);
                    if (v0 == HUFF_VEC1_SIZE - 1)
                        v0 += wmapro_get_large_val(&s->gb);
                    v1 = get_vlc2(&s->gb, vec1_vlc.table, VLCBITS, VEC1MAXDEPTH);
                    if (v1 == HUFF_VEC1_SIZE - 1)
                        v1 += wmapro_get_large_val(&s->gb);
                    (vals)[i  ] = v0;
                    (vals)[i+1] = v1;
                } else {
                    vals[i]   = fixval_tab[symbol_to_vec2[idx] >> 4 ];
                    vals[i+1] = fixval_tab[symbol_to_vec2[idx] & 0xF];
                }
            }
        } else {
            vals[0] = fixval_tab[ symbol_to_vec4[idx] >> 12      ];
            vals[1] = fixval_tab[(symbol_to_vec4[idx] >> 8) & 0xF];
            vals[2] = fixval_tab[(symbol_to_vec4[idx] >> 4) & 0xF];
            vals[3] = fixval_tab[ symbol_to_vec4[idx]       & 0xF];
        }
		
        /** decode sign */
        for (i = 0; i < 4; i++) {
            if (vals[i]) {
                int sign = get_bits1(&s->gb)-1;
                I64 vals64;
                vals64.r.hi32 = (vals[i]^sign) - sign;
                vals64.r.lo32 = 0;
                ci->fixcoeffs[cur_coeff] = vals64.w64;
                num_zeros = 0;
            } else {
                ci->fixcoeffs[cur_coeff] = 0;
                /** switch to run level mode when subframe_len / 128 zeros
				were found in a row */
                rl_mode |= (++num_zeros > s->subframe_len >> 8);
            }
            ++cur_coeff;
        }
    }
	
    /** decode run level coded coefficients */
    if (cur_coeff < s->subframe_len) {
        memset_lib(&ci->fixcoeffs[cur_coeff], 0,
			sizeof(*ci->fixcoeffs) * (s->subframe_len - cur_coeff));
        if (fix_wmapro_run_level_decode(s->avctx, &s->gb, vlc,
			level, run, ci->fixcoeffs,
			cur_coeff, s->subframe_len,
			s->subframe_len, s->esc_len, 0)){
			//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
            return WMAPRO_ERR_InvalidData;
        }
    }
	
    return 0;
}

/**
*@brief Extract scale factors from the bitstream.
*@param s codec context
*@return 0 on success, < 0 in case of bitstream errors
*/
static int decode_scale_factors(WMAProDecodeCtx* s)
{
    int i;
	
    /** should never consume more than 5344 bits
	*  MAX_CHANNELS * (1 +  MAX_BANDS * 23)
	*/
	
    for (i = 0; i < s->channels_for_cur_subframe; i++) {
        int c = s->channel_indexes_for_cur_subframe[i];
        int* sf;
        int* sf_end;
        s->channel[c].scale_factors = s->channel[c].saved_scale_factors[!s->channel[c].scale_factor_idx];
        sf_end = s->channel[c].scale_factors + s->num_bands;
		
        /** resample scale factors for the new block size
		*  as the scale factors might need to be resampled several times
		*  before some  new values are transmitted, a backup of the last
		*  transmitted scale factors is kept in saved_scale_factors
		*/
        if (s->channel[c].reuse_sf) {
            const int8_t* sf_offsets = s->sf_offsets[s->table_idx][s->channel[c].table_idx];
            int b;
            for (b = 0; b < s->num_bands; b++)
                s->channel[c].scale_factors[b] =
				s->channel[c].saved_scale_factors[s->channel[c].scale_factor_idx][*sf_offsets++];
        }
		
        if (!s->channel[c].cur_subframe || get_bits1(&s->gb)) {
			
            if (!s->channel[c].reuse_sf) {
                int val;
                /** decode DPCM coded scale factors */
                s->channel[c].scale_factor_step = get_bits(&s->gb, 2) + 1;
                val = 45 / s->channel[c].scale_factor_step;
                for (sf = s->channel[c].scale_factors; sf < sf_end; sf++) {
                    val += get_vlc2(&s->gb, sf_vlc.table, SCALEVLCBITS, SCALEMAXDEPTH) - 60;
                    *sf = val;
                }
            } else {
                int i;
                /** run level decode differences to the resampled factors */
                for (i = 0; i < s->num_bands; i++) {
                    int idx;
                    int skip;
                    int val;
                    int sign;
					
                    idx = get_vlc2(&s->gb, sf_rl_vlc.table, VLCBITS, SCALERLMAXDEPTH);
					
                    if (!idx) {
                        uint32_t code = get_bits(&s->gb, 14);
                        val  =  code >> 6;
                        sign = (code & 1) - 1;
                        skip = (code & 0x3f) >> 1;
                    } else if (idx == 1) {
                        break;
                    } else {
                        skip = scale_rl_run[idx];
                        val  = scale_rl_level[idx];
                        sign = get_bits1(&s->gb)-1;
                    }
					
                    i += skip;
                    if (i >= s->num_bands) {
                        av_log(s->avctx, AV_LOG_ERROR,
							"invalid scale factor coding\n");
						//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
                        return WMAPRO_ERR_InvalidData;
                    }
                    s->channel[c].scale_factors[i] += (val ^ sign) - sign;
                }
            }
            /** swap buffers */
            s->channel[c].scale_factor_idx = !s->channel[c].scale_factor_idx;
            s->channel[c].table_idx = s->table_idx;
            s->channel[c].reuse_sf  = 1;
        }
		
        /** calculate new scale factor maximum */
        s->channel[c].max_scale_factor = s->channel[c].scale_factors[0];
        for (sf = s->channel[c].scale_factors + 1; sf < sf_end; sf++) {
            s->channel[c].max_scale_factor =
                FFMAX(s->channel[c].max_scale_factor, *sf);
        }
		
    }
    return 0;
}


static INLINE void wmapro_vector_fmul_scalar64(int32_t *dst, const int64_t *src, 
											   int32_t mul1, int32_t mul2, int scale, int len)
{
    int i;
    int64_t tmp64;
    if(mul2 == 1)
    {
        for (i = 0; i < len; i++)
        {
            tmp64 = fix64mul32(src[i], mul1, 31);
            dst[i] = (tmp64>>(scale+(31-29)));
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            tmp64 = fix64mul32(src[i], mul1, 31);
            dst[i] = ((tmp64 * (int64_t)mul2)>>(scale+(31-29)));
        }
    }
}

static INLINE void vector_fmul_scalar64(int64_t *src64, int len)
{
    int i;
    I64 *tmp64;
    for (i = 0; i < len; i++)
    {
        tmp64 = (I64*)&src64[i];
        src64[i] = FIX64_MUL(tmp64->r.hi32<<2, Q1_CONST(181.0 / 128));
    }
}


/**
*@brief Reconstruct the individual channel data.
*@param s codec context
*/
static void inverse_channel_transform(WMAProDecodeCtx *s)
{
    int i;
	
    for (i = 0; i < s->num_chgroups; i++) {
        if (s->chgroup[i].transform) {
            int32_t fixdata[WMAPRO_MAX_CHANNELS];
            const int num_channels = s->chgroup[i].num_channels;
            int64_t** fixch_data = s->chgroup[i].fixchannel_data;
            int64_t** fixch_end = fixch_data + num_channels;
            const int8_t* tb = s->chgroup[i].transform_band;
            int16_t* sfb;
			
            /** multichannel decorrelation */
            for (sfb = s->cur_sfb_offsets;
			sfb < s->cur_sfb_offsets + s->num_bands; sfb++) {
                int y;
                if (*tb++ == 1) {
                    /** multiply values with the decorrelation_matrix */
                    for (y = sfb[0]; y < FFMIN(sfb[1], s->subframe_len); y++) {
                        const int32_t* mat = s->chgroup[i].decorrelation_matrix;
                        const int32_t* fixdata_end = fixdata + num_channels;
                        int32_t* fixdata_ptr = fixdata;
                        int64_t** fixch;
						
                        for (fixch = fixch_data; fixch < fixch_end; fixch++)
                        {
                            I64 *tmp64;
                            tmp64 = (I64*)&(*fixch)[y];
                            *fixdata_ptr++ = tmp64->r.hi32;
                        }
						
                        for (fixch = fixch_data; fixch < fixch_end; fixch++) {
                            I64 sum64;
                            fixdata_ptr = fixdata;
                            sum64.w64 = 0;
                            while (fixdata_ptr < fixdata_end)
                                sum64.w64 = MADD64(sum64.w64, (*fixdata_ptr++), *mat++);
							
                            (*fixch)[y] = (sum64.w64)<<1;
                        }
                    }
                } else if (s->num_channels == 2) {
                    int len = FFMIN(sfb[1], s->subframe_len) - sfb[0];
                    vector_fmul_scalar64(fixch_data[0] + sfb[0], len);
                    vector_fmul_scalar64(fixch_data[1] + sfb[0], len);
                }
            }
        }
    }
	
}

static void wmapro_vector_fmul_window(int32_t *dst, const int32_t *src1, const int32_t *win, int len)
{
    int i,j;
    dst += len;
    win += len;
    for(i=-len, j=len-1; i<0; i++, j--) {
        int32_t s0 = (src1[i]);
        int32_t s1 = (src1[j]);
        int32_t wi = (win[i]);
        int32_t wj = (win[j]);
        dst[i] = (MSUB64(FIX64_MUL(s0, wj), s1, wi)>>32)<<1;
        dst[j] = (MADD64(FIX64_MUL(s0, wi), s1, wj)>>32)<<1;
    }
}

static void wmapro_window(WMAProDecodeCtx *s)
{
    int i;
    for (i = 0; i < s->channels_for_cur_subframe; i++) {
        int c = s->channel_indexes_for_cur_subframe[i];
        int32_t* window;
        int winlen = s->channel[c].prev_block_len;
        int32_t* fixstart = s->channel[c].fixcoeffs32 - (winlen >> 1);
		
        if (s->subframe_len < winlen) {
            fixstart += (winlen - s->subframe_len) >> 1;
            winlen = s->subframe_len;
        }
		
        window = s->windows[av_log2(winlen) - WMAPRO_BLOCK_MIN_BITS];
		
        winlen >>= 1;
		
        wmapro_vector_fmul_window(fixstart, fixstart + winlen, window, winlen);
		
        s->channel[c].prev_block_len = s->subframe_len;
    }
}

#if 0
/* complex multiplication: p = a * b */
#define FIXCMUL(pre, pim, are, aim, bre, bim) \
{\
    FixFFTSample _are = (are);\
    FixFFTSample _aim = (aim);\
    FixFFTSample _bre = (bre);\
    FixFFTSample _bim = (bim);\
    (pre) = (FIX_MUL32(_are, _bre) - FIX_MUL32(_aim, _bim))<<1;\
    (pim) = (FIX_MUL32(_are, _bim) + FIX_MUL32(_aim, _bre))<<1;\
}
#else
/* complex multiplication: p = a * b */
#define FIXCMUL(pre, pim, are, aim, bre, bim) \
{\
    FixFFTSample _are = (are);\
    FixFFTSample _aim = (aim);\
    FixFFTSample _bre = (bre);\
    FixFFTSample _bim = (bim);\
    (pre) = (MSUB64(FIX64_MUL(_are, _bre), _aim, _bim)>>32);\
    (pim) = (MADD64(FIX64_MUL(_are, _bim), _aim, _bre)>>32);\
}
#endif

/**
* Compute the middle half of the inverse MDCT of size N = 2^nbits,
* thus excluding the parts that can be derived by symmetry
* @param output N/2 samples
* @param input N/2 samples
*/
static void fix_imdct_half(FixFFTContext *s, FixFFTSample *output, const FixFFTSample *input)
{
    int k, n8, n4, n2, n, j;
    const FixFFTSample *tcos = s->tcos;
    const FixFFTSample *tsin = s->tsin;
    const FixFFTSample *in1, *in2;
    FixFFTComplex *z = (FixFFTComplex *)output;
    const int revtab_shift = (14- s->mdct_bits);
	
    n = 1 << s->mdct_bits;
    n2 = n >> 1;
    n4 = n >> 2;
    n8 = n >> 3;
	
    /* pre rotation */
    in1 = input;
    in2 = input + n2 - 1;
    for(k = 0; k < n4; k++) {
        j=revtab[k]>>revtab_shift;
        FIXCMUL(z[j].re, z[j].im, *in2, *in1, (tcos[k]), (tsin[k]));
        in1 += 2;
        in2 -= 2;
    }
    fix_fft_calc_c(s->nbits, z);
	
    /* post rotation + reordering */
    for(k = 0; k < n8; k++) {
        FixFFTSample r0, i0, r1, i1;
        FIXCMUL(r0, i1, z[n8-k-1].im, z[n8-k-1].re, (tsin[n8-k-1]), (tcos[n8-k-1]));
        FIXCMUL(r1, i0, z[n8+k  ].im, z[n8+k  ].re, (tsin[n8+k  ]), (tcos[n8+k  ]));
        z[n8-k-1].re = r0;
        z[n8-k-1].im = i0;
        z[n8+k  ].re = r1;
        z[n8+k  ].im = i1;
    }
}


/**
*@brief Decode a single subframe (block).
*@param s codec context
*@return 0 on success, < 0 when decoding failed
*/
static int decode_subframe(WMAProDecodeCtx *s)
{
    int offset = s->samples_per_frame;
    int subframe_len = s->samples_per_frame;
    int i;
    int total_samples   = s->samples_per_frame * s->num_channels;
    int transmit_coeffs = 0;
    int cur_subwoofer_cutoff;
	
    s->subframe_offset = get_bits_count(&s->gb);
	
    /** reset channel context and find the next block offset and size
	== the next block of the channel with the smallest number of
	decoded samples
    */
    for (i = 0; i < s->num_channels; i++) {
        s->channel[i].grouped = 0;
        if (offset > s->channel[i].decoded_samples) {
            offset = s->channel[i].decoded_samples;
            subframe_len =
                s->channel[i].subframe_len[s->channel[i].cur_subframe];
        }
    }
	
    //dprintf(s->avctx, "processing subframe with offset %i len %i\n", offset, subframe_len);
    //av_log(s->avctx, AV_LOG_WARNING, "processing subframe with offset %i len %i, idx=%d \n", offset, subframe_len, av_log2(subframe_len));
	
    /** get a list of all channels that contain the estimated block */
    s->channels_for_cur_subframe = 0;
    for (i = 0; i < s->num_channels; i++) {
        const int cur_subframe = s->channel[i].cur_subframe;
        /** substract already processed samples */
        total_samples -= s->channel[i].decoded_samples;
		
        /** and count if there are multiple subframes that match our profile */
        if (offset == s->channel[i].decoded_samples &&
            subframe_len == s->channel[i].subframe_len[cur_subframe]) {
            total_samples -= s->channel[i].subframe_len[cur_subframe];
            s->channel[i].decoded_samples +=
                s->channel[i].subframe_len[cur_subframe];
            s->channel_indexes_for_cur_subframe[s->channels_for_cur_subframe] = i;
            ++s->channels_for_cur_subframe;
        }
    }
	
    /** check if the frame will be complete after processing the
	estimated block */
    if (!total_samples)
        s->parsed_all_subframes = 1;
	
	
    //dprintf(s->avctx, "subframe is part of %i channels\n",
	//       s->channels_for_cur_subframe);
	
    /** calculate number of scale factor bands and their offsets */
    s->table_idx         = av_log2(s->samples_per_frame/subframe_len);
    s->num_bands         = s->num_sfb[s->table_idx];
    s->cur_sfb_offsets   = s->sfb_offsets[s->table_idx];
    cur_subwoofer_cutoff = s->subwoofer_cutoffs[s->table_idx];
	
    /** configure the decoder for the current subframe */
    for (i = 0; i < s->channels_for_cur_subframe; i++) {
        int c = s->channel_indexes_for_cur_subframe[i];
		
        s->channel[c].fixcoeffs = &s->channel[c].fixout[(s->samples_per_frame >> 1) + offset];
        s->channel[c].fixcoeffs32 = &s->channel[c].fixout32[(s->samples_per_frame >> 1) + offset];
    }
	
    s->subframe_len = subframe_len;
    s->esc_len = av_log2(s->subframe_len - 1) + 1;
	
    /** skip extended header if any */
    if (get_bits1(&s->gb)) {
        int num_fill_bits;
        if (!(num_fill_bits = get_bits(&s->gb, 2))) {
            int len = get_bits(&s->gb, 4);
            num_fill_bits = get_bits(&s->gb, len) + 1;
        }
		
        if (num_fill_bits >= 0) {
            if (get_bits_count(&s->gb) + num_fill_bits > s->num_saved_bits) {
                av_log(s->avctx, AV_LOG_ERROR, "invalid number of fill bits\n");
				//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
                return WMAPRO_ERR_InvalidData;
            }
			
            skip_bits_long(&s->gb, num_fill_bits);
        }
    }
	
    /** no idea for what the following bit is used */
    if (get_bits1(&s->gb)) {
        av_log(s->avctx, AV_LOG_WARNING, "reserved bit set\n");
		//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
        return WMAPRO_ERR_InvalidData;
    }
	
    if (decode_channel_transform(s) < 0){
		//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
        return WMAPRO_ERR_InvalidData;
    }
	
    for (i = 0; i < s->channels_for_cur_subframe; i++) {
        int c = s->channel_indexes_for_cur_subframe[i];
        if ((s->channel[c].transmit_coefs = get_bits1(&s->gb)))
            transmit_coeffs = 1;
    }
	
    if (transmit_coeffs) {
        int step;
        int quant_step = 90 * s->bits_per_sample >> 4;
		
        /** decode number of vector coded coefficients */
        if ((s->transmit_num_vec_coeffs = get_bits1(&s->gb))) {
            int num_bits = av_log2((s->subframe_len + 3)/4) + 1;
            for (i = 0; i < s->channels_for_cur_subframe; i++) {
                int c = s->channel_indexes_for_cur_subframe[i];
                s->channel[c].num_vec_coeffs = get_bits(&s->gb, num_bits) << 2;
            }
        } else {
            for (i = 0; i < s->channels_for_cur_subframe; i++) {
                int c = s->channel_indexes_for_cur_subframe[i];
                s->channel[c].num_vec_coeffs = s->subframe_len;
            }
        }
        /** decode quantization step */
        step = get_sbits(&s->gb, 6);
        quant_step += step;
        if (step == -32 || step == 31) {
            const int sign = (step == 31) - 1;
            int quant = 0;
            while (get_bits_count(&s->gb) + 5 < s->num_saved_bits &&
				(step = get_bits(&s->gb, 5)) == 31) {
                quant += 31;
            }
            quant_step += ((quant + step) ^ sign) - sign;
        }
        if (quant_step < 0) {
            av_log(s->avctx, AV_LOG_DEBUG, "negative quant step\n");
        }
		
        /** decode quantization step modifiers for every channel */
		
        if (s->channels_for_cur_subframe == 1) {
            s->channel[s->channel_indexes_for_cur_subframe[0]].quant_step = quant_step;
        } else {
            int modifier_len = get_bits(&s->gb, 3);
            for (i = 0; i < s->channels_for_cur_subframe; i++) {
                int c = s->channel_indexes_for_cur_subframe[i];
                s->channel[c].quant_step = quant_step;
                if (get_bits1(&s->gb)) {
                    if (modifier_len) {
                        s->channel[c].quant_step += get_bits(&s->gb, modifier_len) + 1;
                    } else
                        ++s->channel[c].quant_step;
                }
            }
        }
		
        /** decode scale factors */
        if (decode_scale_factors(s) < 0){
			//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
            return WMAPRO_ERR_InvalidData;
        }
    }
	
    //dprintf(s->avctx, "BITSTREAM: subframe header length was %i\n",
	//       get_bits_count(&s->gb) - s->subframe_offset);
	
    /** parse coefficients */
    for (i = 0; i < s->channels_for_cur_subframe; i++) {
        int c = s->channel_indexes_for_cur_subframe[i];
        if (s->channel[c].transmit_coefs &&
            get_bits_count(&s->gb) < s->num_saved_bits) {
            decode_coeffs(s, c);
        } else {
            memset_lib(s->channel[c].fixcoeffs, 0,
				sizeof(*s->channel[c].fixcoeffs) * subframe_len);
        }
    }
	
	// dprintf(s->avctx, "BITSTREAM: subframe length was %i\n",
    //        get_bits_count(&s->gb) - s->subframe_offset);
	
    if (transmit_coeffs) {
        int scale = (WMAPRO_BLOCK_MIN_BITS + (av_log2(subframe_len) - WMAPRO_BLOCK_MIN_BITS) - 1+s->bits_per_sample - 1);
        /** reconstruct the per channel data */
        inverse_channel_transform(s);
        for (i = 0; i < s->channels_for_cur_subframe; i++) {
            int c = s->channel_indexes_for_cur_subframe[i];
            const int* sf = s->channel[c].scale_factors;
            int b;
			
            if (c == s->lfe_channel)
                memset_lib(&s->fixtmp[cur_subwoofer_cutoff], 0, sizeof(*s->fixtmp) *
				(subframe_len - cur_subwoofer_cutoff));
			
            /** inverse quantization and rescaling */
            for (b = 0; b < s->num_bands; b++) {
                const int end = FFMIN(s->cur_sfb_offsets[b+1], s->subframe_len);
                int exp = s->channel[c].quant_step -
					(s->channel[c].max_scale_factor - *sf++) *
					s->channel[c].scale_factor_step;
                int expfrac = exp%20;
                int expint = exp/20;
                int start = s->cur_sfb_offsets[b];
                int fixquant10, fixquant5;
                if(expfrac < 0)
                {
                    expint--;
                    expfrac+=20;
                }
                fixquant10 = pow10_1_20sf[expfrac];
                fixquant5 = 1;
                if(expint > 0)
                {
                    for(exp=expint;exp--;)
                        fixquant5 *= 5;
                }
                else if(expint < 0)
                {
                    for(exp=expint;exp++;)
                        fixquant10 = FIX_MUL32(fixquant10, Q0_CONST(1.0*2/5.0));
                }
                
                wmapro_vector_fmul_scalar64(s->fixtmp + start, s->channel[c].fixcoeffs + start,
                    (fixquant10), fixquant5, (scale-expint-pow10_1_20exp2[expfrac]), end - start);
            }
			
            /** apply imdct (ff_imdct_half == DCTIV with reverse) */
            fix_imdct_half(&s->fixmdct_ctx[av_log2(subframe_len) - WMAPRO_BLOCK_MIN_BITS], s->channel[c].fixcoeffs32, s->fixtmp);
        }
    }
    else
    {//gas add
        for (i = 0; i < s->channels_for_cur_subframe; i++) {
            int c = s->channel_indexes_for_cur_subframe[i];
            memset_lib(s->channel[c].fixcoeffs32, 0,
				sizeof(*s->channel[c].fixcoeffs32) * subframe_len);
        }
    }
	
	
    /** window and overlapp-add */
    wmapro_window(s);
	
    /** handled one subframe */
    for (i = 0; i < s->channels_for_cur_subframe; i++) {
        int c = s->channel_indexes_for_cur_subframe[i];
        if (s->channel[c].cur_subframe >= s->channel[c].num_subframes) {
            av_log(s->avctx, AV_LOG_ERROR, "broken subframe\n");
			//LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
            return WMAPRO_ERR_InvalidData;
        }
        ++s->channel[c].cur_subframe;
    }
	
    return 0;
}

/**
*@brief Decode one WMA frame.
*@param s codec context
*@return 0 if the trailer bit indicates that this is the last frame,
*        1 if there are additional frames
*/
static int decode_frame(WMAProDecodeCtx *s)
{
    GetBitContext* gb = &s->gb;
    int more_frames = 0;
    int len = 0;
    int i;
	
    /*check for potential output buffer overflow*/
    if (s->num_channels * s->samples_per_frame > s->samples_end - s->samples){
        /*return an error if no frame could be decoded at all*/
        av_log(s->avctx, AV_LOG_ERROR,"not enough space for the output samples\n");
        s->packet_loss = 1;
        return 0;
    }
	
    if (s->len_prefix)      //get frame length 
        len = get_bits(gb, s->log2_frame_size);
        
    if (decode_tilehdr(s)){ //decode tile information
        s->packet_loss = 1;
        return 0;
    }
	
    if (s->num_channels > 1 && get_bits1(gb)){ //read postproc transform
        if (get_bits1(gb)){
            for (i = 0; i < s->num_channels * s->num_channels; i++)
                skip_bits(gb, 4);
        }
    }
	
    if (s->dynamic_range_compression){//read drc info
        s->drc_gain = get_bits(gb, 8);
    }
	
    /*no idea what these are for, might be the number of samples
      that need to be skipped at the beginning or end of a stream */
    if (get_bits1(gb)){
        int skip;
        if (get_bits1(gb)) //usually true for the first frame 
            skip = get_bits(gb, av_log2(s->samples_per_frame * 2));		
        if (get_bits1(gb)) //sometimes true for the last frame
            skip = get_bits(gb, av_log2(s->samples_per_frame * 2));		
    }
	
    s->parsed_all_subframes = 0;//reset subframe states
    for (i = 0; i < s->num_channels; i++){
        s->channel[i].decoded_samples = 0;
        s->channel[i].cur_subframe    = 0;
        s->channel[i].reuse_sf        = 0;
    }
	
    while (!s->parsed_all_subframes){//decode all subframes
        if (decode_subframe(s) < 0){
            s->packet_loss = 1;
            return 0;
        }
    }
#define DOWNMIX	
#ifndef DOWNMIX
    for (i = 0; i < s->num_channels; i++){
        short* ptr  = s->samples + i;
        int incr = s->num_channels;
        int32_t* iptr = (s->channel[i].fixout32);
        int32_t* iend = iptr + s->samples_per_frame;
        // FIXME should create/use a DSP function here
        while (iptr < iend) {
            *ptr = av_clip_int16(((*iptr++)>>(27-15)));
            ptr += incr;
        }
    }
#else
	//5: L R C Lb Rb
	//6: L R C Lb Rb LFE
	//7: L R C Cb Ls Rs LFE
	//8: L R C Lb Rb Ls Rs LFE
	if(s->num_channels<=2){			    
		for (i = 0; i < s->num_channels; i++){
			short* ptr  = s->samples + i;
			int incr = s->num_channels;
			int32_t* iptr = (s->channel[i].fixout32);
			int32_t* iend = iptr + s->samples_per_frame;  
			while (iptr < iend) {
				*ptr = av_clip_int16(
					((*iptr++)>>(27-15)));
				ptr += incr;
			}
		}	
	}else if(s->num_channels==3){
		int32_t c0,c1,c2;
		short* ptr = s->samples;
		for (i = 0; i < s->samples_per_frame; i++){
			c0=s->channel[0].fixout32[i];
			c1=s->channel[1].fixout32[i];
			c2=s->channel[2].fixout32[i];
			*ptr++=av_clip_int16( (c0+c2)>>(27-15) );
			*ptr++=av_clip_int16( (c1+c2)>>(27-15) );
		}
	}else if(s->num_channels==4){
		int32_t c0,c1,c2,c3;
		short* ptr = s->samples;
		for (i = 0; i < s->samples_per_frame; i++){
			c0=s->channel[0].fixout32[i];
			c1=s->channel[1].fixout32[i];
			c2=s->channel[2].fixout32[i];
			c3=s->channel[3].fixout32[i];
			*ptr++=av_clip_int16( (c0+c2)>>(27-15) );
			*ptr++=av_clip_int16( (c1+c3)>>(27-15) );
		}
	}else if(s->num_channels>=5){//只考虑到6ch
		      int32_t c0,c1,c2,c3,c4;
			  short* ptr = s->samples;
			  for (i = 0; i < s->samples_per_frame; i++){
                  c0=s->channel[0].fixout32[i];
				  c1=s->channel[1].fixout32[i];
                  c2=s->channel[2].fixout32[i];
				  c3=s->channel[3].fixout32[i];
				  c4=s->channel[4].fixout32[i];
				  *ptr++=av_clip_int16((c0+c2+c3)>>(27-15));
				  *ptr++=av_clip_int16((c1+c2+c4)>>(27-15));
			  }
	}	
#endif
    for (i = 0; i < s->num_channels; i++){
        memcpy_lib(&s->channel[i].fixout32[0],
			&s->channel[i].fixout32[s->samples_per_frame],
			s->samples_per_frame * sizeof(*s->channel[i].fixout32) >> 1);
    }
	
    if (s->skip_frame) 
        s->skip_frame = 0;
    else
#ifndef DOWNMIX
        s->samples += s->num_channels * s->samples_per_frame;
#else
	    s->samples += (s->samples_per_frame*(s->num_channels>2?2:s->num_channels));
#endif
	
    if (s->len_prefix) {
        if (len != (get_bits_count(gb) - s->frame_offset) + 2){
            /*FIXME: not sure if this is always an error */
            av_log(s->avctx, AV_LOG_ERROR,
				"frame[%i] would have to skip %i bits\n", s->frame_num,
				len - (get_bits_count(gb) - s->frame_offset) - 1);
            s->packet_loss = 1;
            return 0;
        }
        /*skip the rest of the frame data */
        skip_bits_long(gb, len - (get_bits_count(gb) - s->frame_offset) - 1);
    } else {
        while (get_bits_count(gb) < s->num_saved_bits && get_bits1(gb) == 0) {
        }
    }
	
    /** decode trailer bit */
    more_frames = get_bits1(gb);
    ++s->frame_num;
    return more_frames;
}

/**
*@brief Calculate remaining input buffer length.
*@param s codec context
*@param gb bitstream reader context
*@return remaining size in bits
*/
static int remaining_bits(WMAProDecodeCtx *s, GetBitContext *gb)
{
    return s->buf_bit_size - get_bits_count(gb);
}

/**
*@brief Fill the bit reservoir with a (partial) frame.
*@param s codec context
*@param gb bitstream reader context
*@param len length of the partial frame
*@param append decides wether to reset the buffer or not
*/
static void save_bits(WMAProDecodeCtx *s, GetBitContext* gb, int len,
                      int append)
{
    int buflen;
	
    /** when the frame data does not need to be concatenated, the input buffer
	is resetted and additional bits from the previous frame are copyed
	and skipped later so that a fast byte copy is possible */
	
    if (!append) {
        s->frame_offset = get_bits_count(gb) & 7;
        s->num_saved_bits = s->frame_offset;
        init_put_bits(&s->pb, s->frame_data, MAX_FRAMESIZE);
    }
	
    buflen = (s->num_saved_bits + len + 8) >> 3;
	
    if (len <= 0 || buflen > MAX_FRAMESIZE){
        //av_log(s->avctx, AV_LOG_WARNING, "input buffer too small\n");
        s->packet_loss = 1;
        return;
    }
	
    s->num_saved_bits += len;
    if (!append) {
        ff_copy_bits(&s->pb, gb->buffer + (get_bits_count(gb) >> 3),
			s->num_saved_bits);
    } else {
        int align = 8 - (get_bits_count(gb) & 7);
        align = FFMIN(align, len);
        put_bits(&s->pb, align, get_bits(gb, align));
        len -= align;
        ff_copy_bits(&s->pb, gb->buffer + (get_bits_count(gb) >> 3), len);
    }
    skip_bits_long(gb, len);
	
    {
        PutBitContext tmp = s->pb;
        flush_put_bits(&tmp);
    }
	
    init_get_bits(&s->gb, s->frame_data, s->num_saved_bits);
    skip_bits(&s->gb, s->frame_offset);
}

/**
*@brief Decode a single WMA packet.
*@param avctx codec context
*@param data the output buffer
*@param data_size number of bytes that were written to the output buffer
*@param avpkt input packet
*@return number of bytes that were read from the input buffer
*/
int decode_packet(AudioContext *avctx, void *data, int *data_size, uint8_t* buf, int buf_size)
{
    WMAProDecodeCtx *s = avctx->priv_data;
    GetBitContext* gb  = &s->pgb;
    int num_bits_prev_frame;
    int packet_sequence_number;
	
    s->samples= data;
    s->samples_end= (short*)((int8_t*)data + *data_size);
    *data_size = 0;
	
    if (s->packet_done || s->packet_loss) {
    
        s->packet_done = 0;//只解大小至少为avctx->block_align的包
        if (buf_size < avctx->block_align) { /*sanity check for the buffer length */
            ALOGI("[%s %d]WARNING:buf_size/%d < avctx->block_align/%d,discard it!\n",__FUNCTION__,__LINE__,buf_size,avctx->block_align);
            return WMAPRO_ERR_InvalidData;
        }
        s->next_packet_start = buf_size - avctx->block_align;
        buf_size = avctx->block_align;
        s->buf_bit_size = buf_size << 3;
        
        init_get_bits(gb, buf, s->buf_bit_size);/*parse packet header */
        packet_sequence_number = get_bits(gb, 4);
        skip_bits(gb, 2);
		
        num_bits_prev_frame = get_bits(gb, s->log2_frame_size);//get number of bits that need 
		                                                       //to be added to the previous frame
        /*check for packet loss */
        if (!s->packet_loss && ((s->packet_sequence_number + 1)&0xF)!= packet_sequence_number) 
        {
            s->packet_loss = 1;
            //av_log(avctx, AV_LOG_ERROR, "Packet loss detected! seq %x vs %x\n",s->packet_sequence_number, packet_sequence_number);
        }
        s->packet_sequence_number = packet_sequence_number;
		
        if(num_bits_prev_frame > 0){
            int remaining_packet_bits = s->buf_bit_size - get_bits_count(gb);
            if (num_bits_prev_frame >= remaining_packet_bits){
                num_bits_prev_frame = remaining_packet_bits;
                s->packet_done = 1;
            }
            save_bits(s, gb, num_bits_prev_frame, 1);/*append the previous frame data to the remaining data from the
                                                       previous packet to create a full frame */
            if (!s->packet_loss)//decode the cross packet frame if it is valid
                decode_frame(s);
        }else if (s->num_saved_bits - s->frame_offset) {
            //dprintf(avctx, "ignoring %x previously saved bits\n",s->num_saved_bits - s->frame_offset);
        }
        
        if (s->packet_loss) {     //reset number of saved bits so that the decoder does not 
            s->num_saved_bits = 0;//start to decode incomplete frames in thes->len_prefix == 0 case 
            s->packet_loss = 0;
        }
		
    } else {
        int frame_size;
        s->buf_bit_size = (buf_size - s->next_packet_start) << 3;
        init_get_bits(gb, buf, s->buf_bit_size);
        skip_bits(gb, s->packet_offset);
        if ( s->len_prefix && remaining_bits(s, gb) > s->log2_frame_size &&
             (frame_size = show_bits(gb, s->log2_frame_size)) &&
             frame_size <= remaining_bits(s, gb) ) 
        {
            save_bits(s, gb, frame_size, 0);
            s->packet_done = !decode_frame(s);
        }
        else if (!s->len_prefix && s->num_saved_bits > get_bits_count(&s->gb)) 
		{
            /*when the frames do not have a length prefix, we don't know
			the compressed length of the individual frames
			however, we know what part of a new packet belongs to the
			previous frame
			therefore we save the incoming packet first, then we append
			the "previous frame" data from the next packet so that
			we get a buffer that only contains full frames */
            s->packet_done = !decode_frame(s);
        }
        else
            s->packet_done = 1;
    }
	
    if (s->packet_done && !s->packet_loss && remaining_bits(s, gb) > 0) {
        save_bits(s, gb, remaining_bits(s, gb), 0);//save the rest of the data so that
    }                                              //it can be decoded with the next packet 
    
    *data_size = (int8_t *)s->samples - (int8_t *)data;
    s->packet_offset = get_bits_count(gb) & 7;
	//if((s->packet_loss))
	   //LOGI("TRACE:%s %d %s\n",__FUNCTION__,__LINE__,__FILE__);
    return (s->packet_loss) ? WMAPRO_ERR_InvalidData : get_bits_count(gb) >> 3;
}

/**
*@brief Clear decoder buffers (for seeking).
*@param avctx codec context
*/
void flush(AudioContext *avctx)
{
    WMAProDecodeCtx *s = avctx->priv_data;
    int i;
    /** reset output buffer as a part of it is used during the windowing of a
	new frame */
    for (i = 0; i < s->num_channels; i++)
    {
        memset_lib(s->channel[i].fixout, 0, s->samples_per_frame *
			sizeof(*s->channel[i].fixout));
        memset_lib(s->channel[i].fixout32, 0, s->samples_per_frame *
			sizeof(*s->channel[i].fixout32));
    }
    s->packet_loss = 1;
}


