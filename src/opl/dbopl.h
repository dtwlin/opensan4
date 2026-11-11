/*
 *  Copyright (C) 2002-2010  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#pragma once
#include "dbopl_shim.h"

//Use 8 handlers based on a small logatirmic wavetabe and an exponential table for volume
#define WAVE_HANDLER	10
//Use a logarithmic wavetable with an exponential table for volume
#define WAVE_TABLELOG	11
//Use a linear wavetable with a multiply table for volume
#define WAVE_TABLEMUL	12

//Select the type of wave generator routine
#define DBOPL_WAVE WAVE_TABLEMUL

namespace DBOPL {

struct Chip;
struct Operator;
struct Channel;

#if (DBOPL_WAVE == WAVE_HANDLER)
typedef Bits ( DB_FASTCALL *WaveHandler) ( Bitu i, Bitu volume );
#endif

typedef Bits ( DBOPL::Operator::*VolumeHandler) ( );
typedef Channel* ( DBOPL::Channel::*SynthHandler) ( Chip* chip, Bit32u samples, Bit32s* output );

//Different synth modes that can generate blocks of data
typedef enum {
	sm2AM,
	sm2FM,
	sm3AM,
	sm3FM,
	sm4Start,
	sm3FMFM,
	sm3AMFM,
	sm3FMAM,
	sm3AMAM,
	sm6Start,
	sm2Percussion,
	sm3Percussion,
} SynthMode;

//Shifts for the values contained in chandata variable
enum {
	SHIFT_KSLBASE = 16,
	SHIFT_KEYCODE = 24,
};

struct Operator {
public:
	//Masks for operator 20 values
	enum {
		MASK_KSR = 0x10,
		MASK_SUSTAIN = 0x20,
		MASK_VIBRATO = 0x40,
		MASK_TREMOLO = 0x80,
	};

	typedef enum {
		OFF,
		RELEASE,
		SUSTAIN,
		DECAY,
		ATTACK,
	} State;

	VolumeHandler volHandler = nullptr;

#if (DBOPL_WAVE == WAVE_HANDLER)
	WaveHandler waveHandler = nullptr;	//Routine that generate a wave 
#else
	Bit16s* waveBase = nullptr;
	Bit32u waveMask = 0;
	Bit32u waveStart = 0;
#endif
	Bit32u waveIndex = 0;			//WAVE_BITS shifted counter of the frequency index
	Bit32u waveAdd = 0;				//The base frequency without vibrato
	Bit32u waveCurrent = 0;			//waveAdd + vibratao

	Bit32u chanData = 0;			//Frequency/octave and derived data coming from whatever channel controls this
	Bit32u freqMul = 0;				//Scale channel frequency with this, TODO maybe remove?
	Bit32u vibrato = 0;				//Scaled up vibrato strength
	Bit32s sustainLevel = 0;		//When stopping at sustain level stop here
	Bit32s totalLevel = 0;			//totalLevel is added to every generated volume
	Bit32u currentLevel = 0;		//totalLevel + tremolo
	Bit32s volume = 0;				//The currently active volume
	
	Bit32u attackAdd = 0;			//Timers for the different states of the envelope
	Bit32u decayAdd = 0;
	Bit32u releaseAdd = 0;
	Bit32u rateIndex = 0;			//Current position of the evenlope

	Bit8u rateZero = 0;				//Bits for the different states of the envelope having no changes
	Bit8u keyOn = 0;				//Bitmask of different values that can generate keyon
	//Registers, also used to check for changes
	Bit8u reg20 = 0, reg40 = 0, reg60 = 0, reg80 = 0, regE0 = 0;
	//Active part of the envelope we're in
	Bit8u state = 0;
	//0xff when tremolo is enabled
	Bit8u tremoloMask = 0;
	//Strength of the vibrato
	Bit8u vibStrength = 0;
	//Keep track of the calculated KSR so we can check for changes
	Bit8u ksr = 0;
private:
	void SetState( Bit8u s );
	void UpdateAttack( const Chip* chip );
	void UpdateRelease( const Chip* chip );
	void UpdateDecay( const Chip* chip );
public:
	void UpdateAttenuation();
	void UpdateRates( const Chip* chip );
	void UpdateFrequency( );

	void Write20( const Chip* chip, Bit8u val );
	void Write40( const Chip* chip, Bit8u val );
	void Write60( const Chip* chip, Bit8u val );
	void Write80( const Chip* chip, Bit8u val );
	void WriteE0( const Chip* chip, Bit8u val );

	bool Silent() const;
	void Prepare( const Chip* chip );

	void KeyOn( Bit8u mask);
	void KeyOff( Bit8u mask);

	template< State state>
	Bits TemplateVolume( );

	Bit32s RateForward( Bit32u add );
	Bitu ForwardWave();
	Bitu ForwardVolume();

	Bits GetSample( Bits modulation );
	Bits GetWave( Bitu index, Bitu vol );
public:
	Operator();
};

struct Channel {
	Operator op[2];
	inline Operator* Op( Bitu index ) {
		return &( ( this + (index >> 1) )->op[ index & 1 ]);
	}
	SynthHandler synthHandler = nullptr;
	Bit32u chanData = 0;		//Frequency/octave and derived values
	Bit32s old[2] = {0, 0};			//Old data for feedback

	Bit8u feedback = 31;			//Feedback shift
	Bit8u regB0 = 0;			//Register values to check for changes
	Bit8u regC0 = 0;
	//This should correspond with reg104, bit 6 indicates a Percussion channel, bit 7 indicates a silent channel
	Bit8u fourMask = 0;
	Bit8s maskLeft = -1;		//Sign extended values for both channel's panning
	Bit8s maskRight = -1;

	//Forward the channel data to the operators of the channel
	void SetChanData( const Chip* chip, Bit32u data );
	//Change in the chandata, check for new values and if we have to forward to operators
	void UpdateFrequency( const Chip* chip, Bit8u fourOp );
	void WriteA0( const Chip* chip, Bit8u val );
	void WriteB0( const Chip* chip, Bit8u val );
	void WriteC0( const Chip* chip, Bit8u val );
	void ResetC0( const Chip* chip );

	//call this for the first channel
	template< bool opl3Mode >
	void GeneratePercussion( Chip* chip, Bit32s* output );

	//Generate blocks of data in specific modes
	template<SynthMode mode>
	Channel* BlockTemplate( Chip* chip, Bit32u samples, Bit32s* output );
	Channel();
};

struct Chip {
	//This is used as the base counter for vibrato and tremolo
	Bit32u lfoCounter = 0;
	Bit32u lfoAdd = 0;
	

	Bit32u noiseCounter = 0;
	Bit32u noiseAdd = 0;
	Bit32u noiseValue = 1;

	//Frequency scales for the different multiplications
	Bit32u freqMul[16] = {};
	//Rates for decay and release for rate of this chip
	Bit32u linearRates[76] = {};
	//Best match attack rates for the rate of this chip
	Bit32u attackRates[76] = {};

	//18 channels with 2 operators each
	Channel chan[18];

	Bit8u reg104 = 0;
	Bit8u reg08 = 0;
	Bit8u reg04 = 0;
	Bit8u regBD = 0;
	Bit8u vibratoIndex = 0;
	Bit8u tremoloIndex = 0;
	Bit8s vibratoSign = 0;
	Bit8u vibratoShift = 0;
	Bit8u tremoloValue = 0;
	Bit8u vibratoStrength = 0;
	Bit8u tremoloStrength = 0;
	//Mask for allowed wave forms
	Bit8u waveFormMask = 0;
	//0 or -1 when enabled
	Bit8s opl3Active = 0;

	//Return the maximum amount of samples before and LFO change
	Bit32u ForwardLFO( Bit32u samples );
	Bit32u ForwardNoise();

	void WriteBD( Bit8u val );
	void WriteReg(Bit32u reg, Bit8u val );

	Bit32u WriteAddr( Bit32u port, Bit8u val );

	void GenerateBlock2( Bitu samples, Bit32s* output );
	void GenerateBlock3( Bitu samples, Bit32s* output );

	void Generate( Bit32u samples );
	void Setup( Bit32u r );

	Chip();
};

void InitTables();

}  // namespace DBOPL
