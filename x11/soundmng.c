/*	$Id: soundmng.c,v 1.14 2011/01/15 16:01:52 monaka Exp $	*/

/*
 * Copyright (c) 2001-2003 NONAKA Kimihiro
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "compiler.h"

#include "soundmng.h"

BYTE
snddrv_drv2num(const char* cfgstr)
{

	if (strcasecmp(cfgstr, "SDL") == 0)
		return SNDDRV_SDL;
	return SNDDRV_NODRV;
}

const char *
snddrv_num2drv(BYTE num)
{

	switch (num) {
	case SNDDRV_SDL:
		return "SDL";
	}
	return "nosound";
}

#if !defined(NOSOUND)

#include "np2.h"
#include "pccore.h"
#include "ini.h"
#include "dosio.h"
#include "parts.h"

#include "sysmng.h"
#include "sound.h"

#if defined(VERMOUTH_LIB)
#include "vermouth.h"

MIDIMOD vermouth_module = NULL;
#endif

/*
 * driver
 */
static struct {
        BOOL (*drvinit)(UINT rate, UINT samples);
        BOOL (*drvterm)(void);
	void (*drvlock)(void);
	void (*drvunlock)(void);

        void (*sndplay)(void);
        void (*sndstop)(void);

	void *(*pcmload)(UINT num, const char *path);
	void (*pcmdestroy)(void *chanp, UINT num);
	void (*pcmplay)(void *chanp, UINT num, BOOL loop);
	void (*pcmstop)(void *chanp, UINT num);
	void (*pcmvolume)(void *chanp, UINT num, int volume);
} snddrv;
static int audio_fd = -1;
static BOOL opened = FALSE;
static UINT opna_frame;

static BOOL nosound_setup(void);
static BOOL sdlaudio_setup(void);

static void PARTSCALL (*fnmix)(SINT16* dst, const SINT32* src, UINT size);

#if defined(GCC_CPU_ARCH_IA32)
void PARTSCALL _saturation_s16(SINT16 *dst, const SINT32 *src, UINT size);
void PARTSCALL _saturation_s16x(SINT16 *dst, const SINT32 *src, UINT size);
void PARTSCALL saturation_s16mmx(SINT16 *dst, const SINT32 *src, UINT size);
#endif

/*
 * PCM
 */
static void *pcm_channel[SOUND_MAXPCM];

static void soundmng_pcminit(void);
static void soundmng_pcmdestroy(void);

/*
 * buffer
 */
#ifndef	NSOUNDBUFFER
#define	NSOUNDBUFFER	2
#endif
static char *sound_buffer[NSOUNDBUFFER];
static int sound_nextbuf;
static char *sound_event;

static BOOL buffer_init(void);
static void buffer_destroy(void);
static void buffer_clear(void);


static UINT
calc_blocksize(UINT size)
{
	UINT s = size;

	if (size & (size - 1))
		for (s = 32; s < size; s <<= 1)
			continue;
	return s;
}

static void
snddrv_setup(void)
{

	if (np2oscfg.snddrv < SNDDRV_DRVMAX) {
		switch (np2oscfg.snddrv) {
#if defined(USE_SDLAUDIO) || defined(USE_SDLMIXER)
		case SNDDRV_SDL:
			sdlaudio_setup();
			return;
#endif
		}
	} else {
#if defined(USE_SDLAUDIO) || defined(USE_SDLMIXER)
		if (sdlaudio_setup() == SUCCESS) {
			np2oscfg.snddrv = SNDDRV_SDL;
			sysmng_update(SYS_UPDATEOSCFG);
			return;
		} else
#endif
		{
			/* Nothing to do */
			/* fall thourgh "no match" */
		}
	}

	/* no match */
	nosound_setup();
	np2oscfg.snddrv = SNDDRV_NODRV;
	sysmng_update(SYS_UPDATEOSCFG);
}

static void
sounddrv_lock(void)
{

	(*snddrv.drvlock)();
}

static void
sounddrv_unlock(void)
{

	(*snddrv.drvunlock)();
}

UINT
soundmng_create(UINT rate, UINT bufmsec)
{
	UINT samples;

	if (opened || ((rate != 11025) && (rate != 22050) && (rate != 44100))) {
		return 0;
	}

	if (bufmsec < 20)
		bufmsec = 20;
	else if (bufmsec > 1000)
		bufmsec = 1000;

	snddrv_setup();

	samples = (rate * bufmsec) / 1000 / 2;
	samples = calc_blocksize(samples);
	opna_frame = samples * 2 * sizeof(SINT16);

	if ((*snddrv.drvinit)(rate, samples) != SUCCESS) {
		audio_fd = -1;
		nosound_setup();
		np2oscfg.snddrv = SNDDRV_NODRV;
		sysmng_update(SYS_UPDATEOSCFG);
		return 0;
	}

#if defined(VERMOUTH_LIB)
	vermouth_module = midimod_create(rate);
	midimod_loadall(vermouth_module);
#endif

	soundmng_setreverse(FALSE);
	buffer_init();
	soundmng_reset();

	opened = TRUE;

	return samples;
}

void
soundmng_reset(void)
{

	sounddrv_lock();
	sound_nextbuf = 0;
	sound_event = NULL;
	sounddrv_unlock();
	buffer_clear();
}

void
soundmng_destroy(void)
{
	UINT i;

	if (opened) {
#if defined(VERMOUTH_LIB)
		midimod_destroy(vermouth_module);
		vermouth_module = NULL;
#endif
		for (i = 0; i < SOUND_MAXPCM; i++) {
			soundmng_pcmstop(i);
		}
		(*snddrv.sndstop)();
		(*snddrv.drvterm)();
		audio_fd = -1;
		opened = FALSE;
	}
}

void
soundmng_play(void)
{

	(*snddrv.sndplay)();
}

void
soundmng_stop(void)
{

	(*snddrv.sndstop)();
}

BOOL
soundmng_initialize(void)
{

	snddrv_setup();
	soundmng_pcminit();

	return SUCCESS;
}

void
soundmng_deinitialize(void)
{

	soundmng_pcmdestroy();
	soundmng_destroy();
	buffer_destroy();
}

void
soundmng_sync(void)
{
	const SINT32 *pcm;
	SINT16 *q;

	if (opened) {
		sounddrv_lock();
		if (sound_event) {
			pcm = sound_pcmlock();
			q = (SINT16 *)sound_event;
			sound_event = NULL;
			(*fnmix)(q, pcm, opna_frame);
			sound_pcmunlock(pcm);
		}
		sounddrv_unlock();
	}
}

void
soundmng_setreverse(BOOL reverse)
{

#if defined(GCC_CPU_ARCH_AMD64)
	if (!reverse) {
		if (mmxflag & (MMXFLAG_NOTSUPPORT|MMXFLAG_DISABLE)) {
			fnmix = satuation_s16;
		} else {
			fnmix = saturation_s16mmx;
		}
	} else {
		fnmix = satuation_s16x;
	}
#elif defined(GCC_CPU_ARCH_IA32)
	if (!reverse) {
		if (mmxflag & (MMXFLAG_NOTSUPPORT|MMXFLAG_DISABLE)) {
			fnmix = _saturation_s16;
		} else {
			fnmix = saturation_s16mmx;
		}
	} else {
		fnmix = _saturation_s16x;
	}
#else
	if (!reverse) {
		fnmix = satuation_s16;
	} else {
		fnmix = satuation_s16x;
	}
#endif
}

/*
 * PCM function
 */
void
soundmng_pcminit(void)
{
	int i;

	for (i = 0; i < SOUND_MAXPCM; i++) {
		pcm_channel[i] = NULL;
	}
}

void
soundmng_pcmdestroy(void)
{
	int i;

	for (i = 0; i < SOUND_MAXPCM; i++) {
		if (pcm_channel[i]) {
			(*snddrv.pcmdestroy)(pcm_channel[i], i);
			pcm_channel[i] = NULL;
		}
	}
}

BOOL
soundmng_pcmload(UINT num, const char *filename)
{

	if (num < SOUND_MAXPCM) {
		if (pcm_channel[num])
			(*snddrv.pcmdestroy)(pcm_channel[num], num);
		pcm_channel[num] = (*snddrv.pcmload)(num, filename);
		if (pcm_channel[num])
			return SUCCESS;
	}
	return FAILURE;
}

void
soundmng_pcmvolume(UINT num, int volume)
{

	if ((num < SOUND_MAXPCM) && (pcm_channel[num])) {
		(*snddrv.pcmvolume)(pcm_channel[num], num, volume);
	}
}

BOOL
soundmng_pcmplay(UINT num, BOOL loop)
{

	if ((num < SOUND_MAXPCM) && (pcm_channel[num])) {
		(*snddrv.pcmplay)(pcm_channel[num], num, loop);
		return SUCCESS;
	}
	return FAILURE;
}

void
soundmng_pcmstop(UINT num)
{

	if ((num < SOUND_MAXPCM) && (pcm_channel[num])) {
		(*snddrv.pcmstop)(pcm_channel[num], num);
	}
}

/*
 * sound buffer
 */
static BOOL
buffer_init(void)
{
	int i;

	sounddrv_lock();
	for (i = 0; i < NSOUNDBUFFER; i++) {
		if (sound_buffer[i]) {
			_MFREE(sound_buffer[i]);
		}
		sound_buffer[i] = (char *)_MALLOC(opna_frame, "sound buffer");
		if (sound_buffer[i] == NULL) {
			fprintf(stderr, "buffer_init: can't alloc memory\n");
			sounddrv_unlock();
			return FAILURE;
		}
	}
	sounddrv_unlock();
	return SUCCESS;
}

static void
buffer_clear(void)
{
	int i;

	sounddrv_lock();
	for (i = 0; i < NSOUNDBUFFER; i++) {
		if (sound_buffer[i]) {
			memset(sound_buffer[i], 0, opna_frame);
		}
	}
	sounddrv_unlock();
}

static void
buffer_destroy(void)
{
	int i;

	sounddrv_lock();
	for (i = 0; i < NSOUNDBUFFER; i++) {
		if (sound_buffer[i]) {
			_MFREE(sound_buffer[i]);
			sound_buffer[i] = NULL;
		}
	}
	sounddrv_unlock();
}

/*
 * No sound support
 */
static BOOL
nosound_drvinit(UINT rate, UINT bufmsec)
{

	return SUCCESS;
}

static BOOL
nosound_drvterm(void)
{

	return SUCCESS;
}

static void
nosound_drvlock(void)
{

	/* Nothing to do */
}

static void
nosound_drvunlock(void)
{

	/* Nothing to do */
}

static void
nosound_sndplay(void)
{

	/* Nothing to do */
}

static void
nosound_sndstop(void)
{

	/* Nothing to do */
}

static void *
nosound_pcmload(UINT num, const char *path)
{

	return NULL;
}

static void
nosound_pcmdestroy(void *chanp, UINT num)
{

	/* Nothing to do */
}

static void
nosound_pcmplay(void *chanp, UINT num, BOOL loop)
{

	/* Nothing to do */
}

static void
nosound_pcmstop(void *chanp, UINT num)
{

	/* Nothing to do */
}

static void
nosound_pcmvolume(void *chanp, UINT num, int volume)
{

	/* Nothing to do */
}

static BOOL
nosound_setup(void)
{

	snddrv.drvinit = nosound_drvinit;
	snddrv.drvterm = nosound_drvterm;
	snddrv.drvlock = nosound_drvlock;
	snddrv.drvunlock = nosound_drvunlock;
	snddrv.sndplay = nosound_sndplay;
	snddrv.sndstop = nosound_sndstop;
	snddrv.pcmload = nosound_pcmload;
	snddrv.pcmdestroy = nosound_pcmdestroy;
	snddrv.pcmplay = nosound_pcmplay;
	snddrv.pcmstop = nosound_pcmstop;
	snddrv.pcmvolume = nosound_pcmvolume;

	return SUCCESS;
}

#if defined(GCC_CPU_ARCH_AMD64)
void PARTSCALL
_saturation_s16(SINT16 *dst, const SINT32 *src, UINT size)
{
	asm volatile (
		"movq	%0, %%rcx;"
		"movq	%1, %%rdx;"
		"movl	%2, %%ebx;"
		"shrl	$1, %%ebx;"
		"je	.ss16_ed;"
	".ss16_lp:"
		"movl	(%%rdx), %%eax;"
		"cmpl	$0x000008000, %%eax;"
		"jl	.ss16_min;"
		"movw	$0x7fff, %%ax;"
		"jmp	.ss16_set;"
	".ss16_min:"
		"cmpl	$0x0ffff8000, %%eax;"
		"jg	.ss16_set;"
		"movw	$0x8001, %%ax;"
	".ss16_set:"
		"leal	4(%%rdx), %%edx;"
		"movw	%%ax, (%%rcx);"
		"decl	%%ebx;"
		"leal	2(%%rcx), %%ecx;"
		"jne	.ss16_lp;"
	".ss16_ed:"
		: /* output */
		: "m" (dst), "m" (src), "m" (size)
		: "ebx");
}

void PARTSCALL
_saturation_s16x(SINT16 *dst, const SINT32 *src, UINT size)
{

	asm volatile (
		"movq	%0, %%rcx;"
		"movq	%1, %%rdx;"
		"movl	%2, %%ebx;"
		"shrl	$2, %%ebx;"
		"je	.ss16x_ed;"
	".ss16x_lp:"
		"movl	(%%rdx), %%eax;"
		"cmpl	$0x000008000, %%eax;"
		"jl	.ss16xl_min;"
		"movw	$0x7fff, %%ax;"
		"jmp	.ss16xl_set;"
	".ss16xl_min:"
		"cmpl	$0x0ffff8000, %%eax;"
		"jg	.ss16xl_set;"
		"movw	$0x8001, %%ax;"
	".ss16xl_set:"
		"movw	%%ax, 2(%%rcx);"
		"movl	4(%%rdx), %%eax;"
		"cmpl	$0x000008000, %%eax;"
		"jl	.ss16xr_min;"
		"movw	$0x7fff, %%ax;"
		"jmp	.ss16xr_set;"
	".ss16xr_min:"
		"cmpl	$0x0ffff8000, %%eax;"
		"jg	.ss16xr_set;"
		"mov	$0x8001, %%ax;"
	".ss16xr_set:"
		"movw	%%ax, (%%rcx);"
		"leal	8(%%rdx), %%edx;"
		"decl	%%ebx;"
		"leal	4(%%rcx), %%ecx;"
		"jne	.ss16x_lp;"
	".ss16x_ed:"
		: /* output */
		: "m" (dst), "m" (src), "m" (size)
		: "ebx");
}

void PARTSCALL
saturation_s16mmx(SINT16 *dst, const SINT32 *src, UINT size)
{

	asm volatile (
		"movq	%0, %%rcx;"
		"movq	%1, %%rdx;"
		"movl	%2, %%eax;"
		"shrl	$3, %%eax;"
		"je	.ss16m_ed;"
		"pxor	%%mm0, %%mm0;"
	".ss16m_lp:"
		"movq	(%%rdx), %%mm1;"
		"movq	8(%%rdx), %%mm2;"
		"packssdw %%mm2, %%mm1;"
		"leaq	16(%%rdx), %%rdx;"
		"movq	%%mm1, (%%rcx);"
		"leaq	8(%%rcx), %%rcx;"
		"dec	%%eax;"
		"jne	.ss16m_lp;"
		"emms;"
	".ss16m_ed:"
		: /* output */
		: "m" (dst), "m" (src), "m" (size));
}
#elif defined(GCC_CPU_ARCH_IA32)
void PARTSCALL
_saturation_s16(SINT16 *dst, const SINT32 *src, UINT size)
{
	asm volatile (
		"movl	%0, %%ecx;"
		"movl	%1, %%edx;"
		"movl	%2, %%ebx;"
		"shrl	$1, %%ebx;"
		"je	.ss16_ed;"
	".ss16_lp:"
		"movl	(%%edx), %%eax;"
		"cmpl	$0x000008000, %%eax;"
		"jl	.ss16_min;"
		"movw	$0x7fff, %%ax;"
		"jmp	.ss16_set;"
	".ss16_min:"
		"cmpl	$0x0ffff8000, %%eax;"
		"jg	.ss16_set;"
		"movw	$0x8001, %%ax;"
	".ss16_set:"
		"leal	4(%%edx), %%edx;"
		"movw	%%ax, (%%ecx);"
		"decl	%%ebx;"
		"leal	2(%%ecx), %%ecx;"
		"jne	.ss16_lp;"
	".ss16_ed:"
		: /* output */
		: "m" (dst), "m" (src), "m" (size)
		: "ebx");
}

void PARTSCALL
_saturation_s16x(SINT16 *dst, const SINT32 *src, UINT size)
{

	asm volatile (
		"movl	%0, %%ecx;"
		"movl	%1, %%edx;"
		"movl	%2, %%ebx;"
		"shrl	$2, %%ebx;"
		"je	.ss16x_ed;"
	".ss16x_lp:"
		"movl	(%%edx), %%eax;"
		"cmpl	$0x000008000, %%eax;"
		"jl	.ss16xl_min;"
		"movw	$0x7fff, %%ax;"
		"jmp	.ss16xl_set;"
	".ss16xl_min:"
		"cmpl	$0x0ffff8000, %%eax;"
		"jg	.ss16xl_set;"
		"movw	$0x8001, %%ax;"
	".ss16xl_set:"
		"movw	%%ax, 2(%%ecx);"
		"movl	4(%%edx), %%eax;"
		"cmpl	$0x000008000, %%eax;"
		"jl	.ss16xr_min;"
		"movw	$0x7fff, %%ax;"
		"jmp	.ss16xr_set;"
	".ss16xr_min:"
		"cmpl	$0x0ffff8000, %%eax;"
		"jg	.ss16xr_set;"
		"mov	$0x8001, %%ax;"
	".ss16xr_set:"
		"movw	%%ax, (%%ecx);"
		"leal	8(%%edx), %%edx;"
		"decl	%%ebx;"
		"leal	4(%%ecx), %%ecx;"
		"jne	.ss16x_lp;"
	".ss16x_ed:"
		: /* output */
		: "m" (dst), "m" (src), "m" (size)
		: "ebx");
}

void PARTSCALL
saturation_s16mmx(SINT16 *dst, const SINT32 *src, UINT size)
{

	asm volatile (
		"movl	%0, %%ecx;"
		"movl	%1, %%edx;"
		"movl	%2, %%eax;"
		"shrl	$3, %%eax;"
		"je	.ss16m_ed;"
		"pxor	%%mm0, %%mm0;"
	".ss16m_lp:"
		"movq	(%%edx), %%mm1;"
		"movq	8(%%edx), %%mm2;"
		"packssdw %%mm2, %%mm1;"
		"leal	16(%%edx), %%edx;"
		"movq	%%mm1, (%%ecx);"
		"leal	8(%%ecx), %%ecx;"
		"dec	%%eax;"
		"jne	.ss16m_lp;"
		"emms;"
	".ss16m_ed:"
		: /* output */
		: "m" (dst), "m" (src), "m" (size));
}
#endif	/* GCC_CPU_ARCH_AMD64 */

#if defined(USE_SDLAUDIO) || defined(USE_SDLMIXER)

#include <SDL.h>

static void sdlaudio_lock(void);
static void sdlaudio_unlock(void);
static void sdlaudio_play(void);
static void sdlaudio_stop(void);
static void sdlaudio_callback(void *, unsigned char *, int);

#if !defined(USE_SDLMIXER)

static BOOL
sdlaudio_init(UINT rate, UINT samples)
{
	static SDL_AudioSpec fmt;
	int rv;

	fmt.freq = rate;
	fmt.format = AUDIO_S16SYS;
	fmt.channels = 2;
	fmt.samples = samples;
	fmt.callback = sdlaudio_callback;
	fmt.userdata = (void *)(samples * 2 * sizeof(SINT16));

	rv = SDL_InitSubSystem(SDL_INIT_AUDIO);
	if (rv < 0) {
		fprintf(stderr, "sdlaudio_init: SDL_InitSubSystem\n");
		return FAILURE;
	}

	audio_fd = SDL_OpenAudio(&fmt, NULL);
	if (audio_fd < 0) {
		fprintf(stderr, "sdlaudio_init: SDL_OpenAudio\n");
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return FAILURE;
	}

	return SUCCESS;
}

static BOOL
sdlaudio_term(void)
{

	SDL_PauseAudio(1);
	SDL_CloseAudio();

	return SUCCESS;
}

static BOOL
sdlaudio_setup(void)
{

	snddrv.drvinit = sdlaudio_init;
	snddrv.drvterm = sdlaudio_term;
	snddrv.drvlock = sdlaudio_lock;
	snddrv.drvunlock = sdlaudio_unlock;
	snddrv.sndplay = sdlaudio_play;
	snddrv.sndstop = sdlaudio_stop;
	snddrv.pcmload = nosound_pcmload;
	snddrv.pcmdestroy = nosound_pcmdestroy;
	snddrv.pcmplay = nosound_pcmplay;
	snddrv.pcmstop = nosound_pcmstop;
	snddrv.pcmvolume = nosound_pcmvolume;

	return SUCCESS;
}

#else	/* USE_SDLMIXER */

#include <SDL_mixer.h>

static BOOL
sdlmixer_init(UINT rate, UINT samples)
{
	int rv;

	rv = SDL_InitSubSystem(SDL_INIT_AUDIO);
	if (rv < 0) {
		fprintf(stderr, "sdlmixer_init: SDL_InitSubSystem(): %s\n",
		    SDL_GetError());
		goto failure;
	}

	rv = Mix_OpenAudio(rate, AUDIO_S16SYS, 2, samples);
	if (rv < 0) {
		fprintf(stderr, "sdlmixer_init: Mix_OpenAudio(): %s\n",
		    Mix_GetError());
		goto failure1;
	}

	rv = Mix_AllocateChannels(SOUND_MAXPCM);
	if (rv < 0) {
		fprintf(stderr, "sdlmixer_init: Mix_AllocateChannels(): %s\n",
		    Mix_GetError());
		goto failure1;
	}

	Mix_HookMusic(sdlaudio_callback, (void*)(samples * 2 * sizeof(SINT16)));

	return SUCCESS;

failure1:
	Mix_CloseAudio();
failure:
	return FAILURE;
}

static BOOL
sdlmixer_term(void)
{

	SDL_PauseAudio(1);
	Mix_CloseAudio();

	return SUCCESS;
}

static void *
sdlmixer_pcmload(UINT num, const char *path)
{
	Mix_Chunk *chunk;

	chunk = Mix_LoadWAV(path);
	return (void *)chunk;
}

static void
sdlmixer_pcmdestroy(void *chanp, UINT num)
{
	Mix_Chunk *chunk = (Mix_Chunk *)chanp;

	Mix_HaltChannel(num);
	Mix_FreeChunk(chunk);
}

static void
sdlmixer_pcmplay(void *chanp, UINT num, BOOL loop)
{
	Mix_Chunk *chunk = (Mix_Chunk *)chanp;

	Mix_PlayChannel(num, chunk, loop ? -1 : 1);
}

static void
sdlmixer_pcmstop(void *chanp, UINT num)
{

	Mix_HaltChannel(num);
}

static void
sdlmixer_pcmvolume(void *chanp, UINT num, int volume)
{

	Mix_Volume(num, (MIX_MAX_VOLUME * volume) / 100);
}

static BOOL
sdlaudio_setup(void)
{

	snddrv.drvinit = sdlmixer_init;
	snddrv.drvterm = sdlmixer_term;
	snddrv.drvlock = sdlaudio_lock;
	snddrv.drvunlock = sdlaudio_unlock;
	snddrv.sndplay = sdlaudio_play;
	snddrv.sndstop = sdlaudio_stop;
	snddrv.pcmload = sdlmixer_pcmload;
	snddrv.pcmdestroy = sdlmixer_pcmdestroy;
	snddrv.pcmplay = sdlmixer_pcmplay;
	snddrv.pcmstop = sdlmixer_pcmstop;
	snddrv.pcmvolume = sdlmixer_pcmvolume;

	return SUCCESS;
}

#endif	/* !USE_SDLMIXER */

static void
sdlaudio_lock(void)
{

	SDL_LockAudio();
}

static void
sdlaudio_unlock(void)
{

	SDL_UnlockAudio();
}

static void
sdlaudio_play(void)
{

	SDL_PauseAudio(0);
}

static void
sdlaudio_stop(void)
{

	SDL_PauseAudio(1);
}

static void
sdlaudio_callback(void *userdata, unsigned char *stream, int len)
{
	UINT samples = (UINT)userdata;
	int nextbuf = sound_nextbuf;

	if (sound_event != NULL)
		memset(sound_event, 0, samples);
	sound_nextbuf = (sound_nextbuf + 1) % NSOUNDBUFFER;
	sound_event = sound_buffer[sound_nextbuf];

	SDL_MixAudio(stream, (const void *)sound_buffer[nextbuf], len,
	    SDL_MIX_MAXVOLUME);
}

#endif	/* USE_SDLAUDIO || USE_SDLMIXER */

#endif	/* !NOSOUND */
