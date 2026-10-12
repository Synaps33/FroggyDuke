/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org

    This file written by Ryan C. Gordon (icculus@icculus.org)
*/
#include "SDL_config.h"

/* Output audio to nowhere... */

#include "SDL_rwops.h"
#include "SDL_timer.h"
#include "SDL_audio.h"
#include "SDL_audiomem.h"
#include "SDL_audio_c.h"
#include "SDL_audiodev_c.h"
#include "SDL_dummyaudio.h"

/* The tag name used by DUMMY audio */
#define DUMMYAUD_DRIVER_NAME         "dummy"

/* Audio driver functions */
static int DUMMYAUD_OpenAudio(_THIS, SDL_AudioSpec *spec);
static void DUMMYAUD_WaitAudio(_THIS);
static void DUMMYAUD_PlayAudio(_THIS);
static Uint8 *DUMMYAUD_GetAudioBuf(_THIS);
static void DUMMYAUD_CloseAudio(_THIS);

/* Audio driver bootstrap functions */
static int DUMMYAUD_Available(void)
{
	/* Always available.
	 *
	 * The stock check requires SDL_AUDIODRIVER=dummy, but this SDL has no
	 * real environment: SDL_getenv() only sees variables set through
	 * SDL_putenv() (SDL_env), and nothing here ever sets an audio one.
	 * SDL_AudioInit() then finds no driver at all, reports "No available
	 * audio device" and Mixer::Init() leaves the mixer invalid, so the game
	 * silently drops every sound. DUMMY is the only driver compiled in
	 * (SDL_AUDIO_DRIVER_DUMMY), so it is always the right answer. */
	return(1);
}

static void DUMMYAUD_DeleteDevice(SDL_AudioDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_AudioDevice *DUMMYAUD_CreateDevice(int devindex)
{
	SDL_AudioDevice *this;

	/* Initialize all variables that we clean on shutdown */
	this = (SDL_AudioDevice *)SDL_malloc(sizeof(SDL_AudioDevice));
	if ( this ) {
		SDL_memset(this, 0, (sizeof *this));
		this->hidden = (struct SDL_PrivateAudioData *)
				SDL_malloc((sizeof *this->hidden));
	}
	if ( (this == NULL) || (this->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( this ) {
			SDL_free(this);
		}
		return(0);
	}
	SDL_memset(this->hidden, 0, (sizeof *this->hidden));

	/* Set the function pointers */
	this->OpenAudio = DUMMYAUD_OpenAudio;
	this->WaitAudio = DUMMYAUD_WaitAudio;
	this->PlayAudio = DUMMYAUD_PlayAudio;
	this->GetAudioBuf = DUMMYAUD_GetAudioBuf;
	this->CloseAudio = DUMMYAUD_CloseAudio;

	this->free = DUMMYAUD_DeleteDevice;

	return this;
}

AudioBootStrap DUMMYAUD_bootstrap = {
	DUMMYAUD_DRIVER_NAME, "SDL dummy audio driver",
	DUMMYAUD_Available, DUMMYAUD_CreateDevice
};

/* This function waits until it is possible to write a full sound buffer */
static void DUMMYAUD_WaitAudio(_THIS)
{
	/* Don't block on first calls to simulate initial fragment filling. */
	if (this->hidden->initial_calls)
		this->hidden->initial_calls--;
	else
		SDL_Delay(this->hidden->write_delay);
}

static void DUMMYAUD_PlayAudio(_THIS)
{
	/* no-op...this is a null driver. */
}

static Uint8 *DUMMYAUD_GetAudioBuf(_THIS)
{
	return(this->hidden->mixbuf);
}

static void DUMMYAUD_CloseAudio(_THIS)
{
	if ( this->hidden->mixbuf != NULL ) {
		SDL_FreeAudioMem(this->hidden->mixbuf);
		this->hidden->mixbuf = NULL;
	}
}

static int DUMMYAUD_OpenAudio(_THIS, SDL_AudioSpec *spec)
{
	float bytes_per_sec = 0.0f;

	/* Enforce 22050Hz stereo 16-bit 735 samples (1 video frame @ 30fps) */
	spec->freq = 22050;
	spec->format = AUDIO_S16LSB;
	spec->channels = 2;
	spec->samples = 735;
	SDL_CalculateAudioSpec(spec);

	/* Allocate mixing buffer */
	this->hidden->mixlen = spec->size;
	this->hidden->mixbuf = (Uint8 *) SDL_AllocAudioMem(this->hidden->mixlen);
	if ( this->hidden->mixbuf == NULL ) {
		return(-1);
	}
	SDL_memset(this->hidden->mixbuf, spec->silence, spec->size);

	bytes_per_sec = (float) (((spec->format & 0xFF) / 8) *
	                   spec->channels * spec->freq);

	this->hidden->initial_calls = 2;
	this->hidden->write_delay =
	               (Uint32) ((((float) spec->size) / bytes_per_sec) * 1000.0f);

	/* Return 1 so audio->opened becomes 2, meaning non-threaded / callback driven */
	return(1);
}

