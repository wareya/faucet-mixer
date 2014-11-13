#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_timer.h>

#include <stdlib.h>
#include <iostream>

SDL_AudioSpec want;
SDL_AudioSpec got;

void noise(void * udata, Uint8 * stream, int len)
{
	int bytes_per_sample = SDL_AUDIO_BITSIZE(got.format)/8;
	int channels = got.channels;
	int used = 0;
	std::cout << "Stream length is " << len << ".\n";
	while(used < len)
	{
		for(auto i = 0; i < channels; i++)
		{
			int r = rand()%32;
			for(auto j = 0; j < bytes_per_sample; j++)
			{
				*(stream+used
				  +i*bytes_per_sample
				  +j) 
				= r;
			}
		}
		used += bytes_per_sample*channels;
	}
}

int main()
{
	want.freq = 44100;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 1024;
    want.callback = noise;
    want.userdata = NULL;
	SDL_OpenAudio(&want, &got);
	std::cout << "Got " << int(got.channels) << " channels.\n";
    SDL_PauseAudio(0);
	SDL_Delay(1000);
	return 0;
}