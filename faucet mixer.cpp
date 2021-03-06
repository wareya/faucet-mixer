#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_endian.h>
#include <SDL2/SDL_thread.h>

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <atomic>

template <typename type>
Uint64 power(type b, type n)
{
	if(n == 0)
		return 1;
	if(n < 0)
		return 1/power(b,-n);
	Uint64 x = b;
	for(auto i = 1; i < n; i++)
		x *= b;
	return x;
}

struct wavfile
{
	int channels;
	int bytespersample; // bytes per sample on a single channel
	int isfloatingpoint;
	int samplerate;
	int blocksize; // number of bytes per sample in time (includes all channels)
	double datagain;
	
	std::atomic<bool> ready; // whether the audio has finished loading/uncompressing in the background
	
	Uint32 length;
	Uint32 bytes;
	Uint8 * data;
	
	wavfile(const char *);
	wavfile();
	~wavfile();
	void do_fmt(FILE *);
	void do_data(FILE *);
    float sample_from_channel_and_position(int channel, Uint32 position);
};

float wavfile::sample_from_channel_and_position(int channel, Uint32 position)
{
    if(!ready)
        return 0;
    auto memoryoffset = position*blocksize;
    auto channeloffset = channel*bytespersample;
    if(!isfloatingpoint)
    {
        Sint64 samplevalue = 0;
        for(auto i = 0; i < bytespersample; i++)
            samplevalue += data[memoryoffset+channeloffset+i] << i*8; // NOTE: LITTLE ENDIAN DATA
        Sint64 signmask = 0x80 << ((bytespersample-1)*8);
        if (samplevalue & signmask)
            samplevalue = samplevalue-power(0x100, bytespersample);
        if (bytespersample == 1) // 8-bit WAV samples are unsigned, and WAV is how we define our memory buffer
            return (float)(*(Uint8*)(&samplevalue)-128)/datagain; // NOTE: LITTLE ENDIAN POINTER ABUSE
        else
            return (float)(samplevalue)/datagain;
    }
    else
    {
        float x;
        if(bytespersample == 4)
            x = *(float*)(&data[memoryoffset+channeloffset]);
        if(bytespersample == 8)
            x = *(double*)(&data[memoryoffset+channeloffset]);
        
        return x/datagain;
    }
}

int normalize_float(void * smp) // called on a thread
{
	wavfile * sample = (wavfile*)smp;
	float highest = 1.0;
	float lowest = 1.0;
	float t;
	if(sample->bytespersample == 4)
	{
		for(unsigned i = 0; i < sample->length*sample->channels; i++)
		{
			t = *(float*)&sample->data[i*sample->bytespersample];
			highest = t>highest?t:highest;
			lowest = t<lowest?t:lowest;
		}
	}
	if(sample->bytespersample == 8)
	{
		for(unsigned i = 0; i < sample->length*sample->channels; i++)
		{
			t = *(double*)&sample->data[i*sample->bytespersample];
			highest = t>highest?t:highest;
			lowest = t<lowest?t:lowest;
		}
	}
	if(-lowest > highest)
		highest = -lowest;
	sample->datagain = highest;
	printf("Normalized sample for %f amplitude.\n", sample->datagain);
	sample->ready = true;
	return 0;
}
void wavfile::do_fmt(FILE * file)
{
	char isuncompressedifthissizeis16[4];
	fread(isuncompressedifthissizeis16, 4, 1, file);
	if(*(Uint32*)(isuncompressedifthissizeis16) != 16)
	{
		puts("Format appears to be compressed.");
		return;
	}
	char format[2];
	fread(format, 2, 1, file);
	switch(*(Uint16*)(format))
	{
	case 1: // Integer PCM
		isfloatingpoint = false;
		break;
	case 3: // floating point PCM
		isfloatingpoint = true;
		break;
	case 0: // literally unknown
		printf("Unknown format.\n");
		return;
	default: // any others
		printf("Unsupported format %d.\n", *(Uint16*)(format));
		return;
	}
	char numchannels[2];
	fread(numchannels, 2, 1, file);
	channels = *(Uint16*)(numchannels);
	char fsamplerate[4];
	fread(fsamplerate, 4, 1, file);
	samplerate = *(Uint32*)(fsamplerate);
	
	fread(fsamplerate, 4, 1, file); // "ByteRate" -- unnecessary
	
	char fblocksize[2];
	fread(fblocksize, 2, 1, file);
	blocksize = *(Uint16*)(fblocksize);
	
	char fbps[2];
	fread(fbps, 2, 1, file);
	bytespersample = (*(Uint16*)(fbps))/8;
	
	if(isfloatingpoint)
		datagain = 1.0f;
	else if(bytespersample == 1)
		datagain = 0x80;
	else if(bytespersample == 2)
		datagain = 0x8000;
	else if(bytespersample == 4)
		datagain = 0x80000000;
	else
		datagain = power(2, bytespersample*8)/2;
	
	if(isfloatingpoint and bytespersample != 8 and bytespersample != 4)
	{
		puts("Unknown floating point format!");
		return;
	}
}
void wavfile::do_data(FILE * file)
{
	char fdatabytes[4];
	fread(fdatabytes, 4, 1, file);
	bytes = *(Uint32*)(fdatabytes);
	if(bytes%blocksize > 0)
	{
		puts("Data length seems to be invalid!");
		return;
	}
	printf("(INFO) Bytes: 0x%04X\n", bytes);
	length = bytes/blocksize;
	data = (Uint8*)malloc(bytes*sizeof(Uint8));
	Uint32 i;
	for(i = 0; i < bytes and !ferror(file) and !feof(file); i++)
	{
		data[i] = fgetc(file);
	}
	if(ferror(file))
		puts("Unknown error reading from file");
	else if(!feof(file))
	{
		Uint32 asdf;
		fread(&asdf, 4, 1, file);
		fseek(file, -4, SEEK_CUR);
		if(asdf != 0x6C706D73) //'smpl'
			printf("Did not reach end of file. 0x%04X\n", asdf);
		
	}
	else if(i < bytes)
		puts("Finished file prematurely");
}
wavfile::wavfile(const char * fname)
{
	auto file = fopen(fname, "rb");
	ready = false;
	
	if (file == NULL)
		puts("Could not open file.");
		
	fseek(file, 0, SEEK_END);
	auto filesize = ftell(file);
	fseek(file, 0, SEEK_SET);

	char riff[16];
	fread(riff, 4, 3, file);
	
	if(*(Uint32*)(riff) != 0x46464952) //'RIFF'
		puts("Not a RIFF file!");
	if(*(Uint32*)(riff+8) != 0x45564157) //'WAVE'
		puts("Not a WAVE file!");
	
	int havefmt = 0;
	while(!ferror(file) and !feof(file))
	{
		char subchunk[4];
		fread(subchunk, 4, 1, file);
		switch(*(Uint32*)(subchunk))
		{
		case 0x20746d66: // 'fmt '
			do_fmt(file);
			havefmt = 1;
			break;
		case 0x61746164: //'data'
			if(!havefmt)
				puts("NO FORMAT!!!");
			do_data(file);
			break;
		default:
			Uint32 len;
			fread(&len, 4, 1, file);
			if (len%2)
				len++;
			printf("Unknown chunk 0x%08X, seeking by 0x%08X\n", *(Uint32*)(subchunk), len);
			fseek(file, len, SEEK_CUR);
			if(ftell(file) >= filesize)
				goto out; // double break pls
		}
	}
	out:
	
	fclose(file);
	
	if(isfloatingpoint)
	{
		SDL_CreateThread(&normalize_float, "faucet\20mixer.cpp:normalize_float", this);
	}
	else
		ready = true;
}
wavfile::wavfile()
{
	data = NULL;
}
wavfile::~wavfile()
{
	if(data != NULL)
		free(data);
}

struct emitter
{
	wavfile * sample;
	Uint32 position; // Position is in OUTPUT SAMPLES, not SOUNDBYTE SAMPLES, i.e. it counts want.freqs not wavfile.freqs
	float pan;
	float volume;
	float mixdown;
    std::atomic<bool> playing;
    bool loop;
};
std::vector<emitter*> emitters;

SDL_AudioSpec want;
SDL_AudioSpec got;

void playfile(void * udata, Uint8 * stream, int len)
{
	(void)udata;
	int stream_bytespersample = SDL_AUDIO_BITSIZE(got.format)/8;
	int stream_datagain = power(2, stream_bytespersample*8)/2;
	int channels = got.channels;
	int used = 0;
	
	int stream_LE = SDL_AUDIO_ISLITTLEENDIAN(got.format);
	
	while(used < len)
	{
		for(auto i = 0; i < channels; i++)
		{
			float transient = 0.0f;
			for(auto emitter : emitters)
			{
				if(!emitter->sample->ready or emitter->playing == false)
					continue;
                
				float ratefactor = emitter->sample->samplerate/(float)(got.freq); // stream samples per emitter samples
                
				if(ceil(emitter->position*ratefactor) >= emitter->sample->length)
                {
                    emitter->playing = false;
					continue;
                }
                
                if(ratefactor == 1)
                    transient += emitter->sample->sample_from_channel_and_position(i, emitter->position);
                else if (ratefactor < 1) // upsample, use triangle filter to artificially create SUPER RETRO SOUNDING highs
                {
                    // convert output position to surrounding input positions
                    long long point = emitter->position * emitter->sample->samplerate;
                    long outpoint1 = point/got.freq;
                    long outpoint2 = point/got.freq + 1;
                    
                    //float point = ratefactor*emitter->position; // point is position on audio stream
                    auto a = emitter->sample->sample_from_channel_and_position(i, outpoint1);
                    auto b = emitter->sample->sample_from_channel_and_position(i, outpoint2);
                    float fraction = (point%got.freq)/(float)(got.freq);
                    //float fraction = point-floor(point);
                    transient += fraction*b + (1-fraction)*a;
                }
                else // ratefactor > 1
                {   // downsample, use triangle filter for laziness's sake
                    float point = ratefactor*emitter->position; // point is position on emitter stream
                    int bottom = ceil(point-ratefactor); // window
                    int top = floor(point+ratefactor);
                    float windowlen = ratefactor*2;
                    
                    float calibrate = 0; // convolution normalization
                    float sample = 0; // output sample
                    for(float j = ceil(bottom); j < top; j++) // convolution
                    {
                        float factor = j>point?j-point:point-j; // distance from output sample
                        factor = ratefactor - factor; // convolution index of this sample
                        calibrate += factor;
                        sample += emitter->sample->sample_from_channel_and_position(i, j) * factor;
                    }
                    sample /= calibrate;
                    transient += sample;
                }
			}
			Sint64 output = transient*stream_datagain;
			Uint8 * outbytes = (Uint8 *)&output;
			for(auto j = 0; j < stream_bytespersample; j++)
			{
				*(stream+used
				  +i*stream_bytespersample
				  +j)
				= stream_LE ? outbytes[j] : outbytes[stream_bytespersample-1-j];
			}
		}
		for(auto emitter : emitters)
		{
			emitter->position += 1; // n OUTPUT samples into EMITTER (opposed to EMITTER samples)
		}
		used += stream_bytespersample*channels;
	}
}

/*
 * faucmix_init
 *  (int channels          // the number of channels in your output stream (You're on your own if you don't pick 2, sorry!)
 *  ,int rate              // output sample rate in Hz. You should usually use 44.1khz or 48khz.
 *  ,int format            // output format according to the SDL sample format enumeration. You should usually use AUDIO_S16.
 *  ,int ahead             // number of samples to mix at once (passed straight to SDL) -- 1024 is a good default value for 44.1khz audio
 *  )
 * 
 * faucmix_set_fadetime(float milliseconds) // set the period length which panning and volume changes interpolate over
 * faucmix_set_volume(float volume) // sets the volume of faucmix as a whole
 * 
 * faucmix_close_device() // closes the output device, halting playback.
 * faucmix_reopen_device(int channels, int rate, int format, int ahead) // opens a new audio device. can not be called unless faucmix_close_device has last been called.
 * faucmix_close_everything() // closes everything, destroying all samples and emitters in memory. cancels running threads. allows faucmix_init to be used again.
 */

/*
 * sample faucmix_sample_create(char * filename, bool normalize)
 * int faucmix_sample_volume(sample, float32 volume)
 * int faucmix_sample_kill(sample)
 * 
 * int faucmix_kill_all_samples(sample) // kills all samples.
 */
 
/*
 * emitter faucmix_emitter_create(sample)
 * int faucmix_emitter_kill(emitter) // stops an emitter immediately and destroys it, deallocating its data.
 * int faucmix_emitter_killgradeful(emitter) // gracefully stop an emitter and kill it once it's stopped
 * 
 * int faucmix_emitter_fire(emitter) // plays the emitter once. hijacks existing playbacks.
 * int faucmix_emitter_loop(emitter) // plays the emitter and starts looping it. does not hijack existing playbacks.
 * int faucmix_emitter_fireloop(emitter, emitter) // plays one emitter, then starts looping a second one the moment the first one finishes.
 *                                                // if the first emitter is stopped or killed before it finishes, the second doesn't play.
 * int faucmix_emitter_stop(emitter) // immediately stops an emitter
 * int faucmix_emitter_stopgraceful(emitter) // stops an emitter once it's finished its current play period (useful for loops)
 * 
 * int faucmix_emitter_all_stop(emitter) // immediately stops all emitters.
 * 
 * int faucmix_emitter_volume(emitter, float volume) // sets an emitter's volume to x (does not affect panning) (uses peak channel)
 * int faucmix_emitter_volumes(emitter, float left, float right) // sets an emitter's volume between the left and right channels
 * int faucmix_emitter_pan(emitter, float left) // sets an emitter's panning from -1 left to 1 right (uses peak channel)
 * 
 * float faucmix_emitter_get_volume(emitter) // gets the faucmix internal volume for the emitter (uses peak channel)
 * float faucmix_emitter_get_volume_right(emitter) // gets the faucmix internal right channel volume for the emitter
 * float faucmix_emitter_get_volume_left(emitter)
 * float faucmix_emitter_get_pan(emitter) // returns the balance between the left and right channels (from -1 to 1)
 * float faucmix_emitter_get_playing(emitter) // returns whether the emitter is currently playing
 * 
 * int faucmix_kill_all_emitters(emitter) // kills all emitters.
 */
 
 /*
 * int faucmix_count_emitters(emitter) // returns the number of emitters currently in the system
 * int faucmix_count_samples(emitter) // returns the number of samples
 * int faucmix_count_emitters_by_sample(emitter) // returns the number of emitters with a given sample
 */
int main(int argc, char * argv[])
{
	if(argc == 1)
		return 0 & puts("Usage: program file");
	wavfile sample(argv[1]);
	
	emitter output;
	output.sample = &sample;
	output.position = 0;
	output.pan = 0.0f;
	output.volume = 1.0f;
	output.mixdown = 1.0f;
    output.playing = true;
    output.loop = true;
	
	emitters.push_back(&output);
	
	want.freq = 41000;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 1024;
    want.callback = playfile;
    want.userdata = NULL;
	SDL_OpenAudio(&want, &got);
    
    printf("%d\n", got.freq);
	
    SDL_PauseAudio(0);
    while(output.playing)
        SDL_Delay(10);
	
	return 0;
}
