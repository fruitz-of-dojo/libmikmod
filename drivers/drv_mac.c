/*	MikMod sound library
	(c) 1998, 1999, 2000 Miodrag Vallat and others - see file AUTHORS for
	complete list.

	This library is free software; you can redistribute it and/or modify
	it under the terms of the GNU Library General Public License as
	published by the Free Software Foundation; either version 2 of
	the License, or (at your option) any later version.
 
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Library General Public License for more details.
 
	You should have received a copy of the GNU Library General Public
	License along with this library; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
	02111-1307, USA.
*/

/*==============================================================================

  $Id: drv_mac.c,v 1.9 2000/02/14 22:10:21 miod Exp $

  Driver for output to the Macintosh Sound Manager

==============================================================================*/

/*
	Written by Anders F Bjoerklund <afb@algonet.se>

	Based on free code by:
	- Antoine Rosset <RossetAntoine@bluewin.ch> (author of PlayerPRO)
	- John Stiles <stiles@emulation.net>
	- Pierre-Olivier Latour <pol@french-touch.net>
	
	This code uses two different ways of filling the buffers:
	- Classic code uses SndPlayDoubleBuffer callbacks
	- Carbon code uses SndCallBacks with Deferred Tasks
        
        <AWE> all changes are marked with "// <AWE> ...".
        - changed code for compatibility with ProjectBuilder/OSX:
            - "NewSndCallBackProc()" to "NewSndCallBackUPP()".
            - "NewDeferredTaskProc()" to "NewDeferredTaskUPP()".
            - added some conditionals to avoid compiler warnings.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mikmod_internals.h"

#if defined (__APPLE__) || defined (MACOSX)

#include <Carbon/Carbon.h>

#else

#include <Sound.h>
#include <OSUtils.h>
#include <Gestalt.h>

#endif

#define USE_SNDDOUBLEBUFFER		!TARGET_CARBON

#define SOUND_BUFFER_SIZE		4096L

static SndChannelPtr			soundChannel = nil;	/* pointer to a sound channel */
#if USE_SNDDOUBLEBUFFER
static SndDoubleBufferHeader	doubleHeader;      	/* pointer to double buffers  */
#else
static ExtSoundHeader			sndHeader;          /* a sound manager bufferCmd header */
static DeferredTask				dtask;				/* deferred task record */
static Ptr						sndBuffer1 = NULL,sndBuffer2 = NULL,currentBuffer;
static Boolean					deferredTaskFired;
#endif

#if USE_SNDDOUBLEBUFFER

/* DoubleBackProc, called at interrupt time */
static pascal void MyDoubleBackProc(SndChannelPtr soundChannel,SndDoubleBufferPtr doubleBuffer)
{
#pragma unused(soundChannel)
	SBYTE *buffer;
	long written;
#if TARGET_CPU_68K
	long oldA5=SetA5(doubleBuffer->dbUserInfo[0]);
#endif

	buffer=(SBYTE*)doubleBuffer->dbSoundData;

	MUTEX_LOCK(vars);
	
	if (Player_Paused())
		written=VC_SilenceBytes(buffer,(ULONG)SOUND_BUFFER_SIZE);
	else
		written=VC_WriteBytes(buffer,(ULONG)SOUND_BUFFER_SIZE);
	
	MUTEX_UNLOCK(vars);
	
	if (doubleHeader.dbhNumChannels==2) written>>=1;
	if (doubleHeader.dbhSampleSize==16) written>>=1;
        
	doubleBuffer->dbNumFrames=written;
	doubleBuffer->dbFlags|=dbBufferReady;

#if TARGET_CPU_68K
	SetA5(oldA5);
#endif
}

#else

/* DeferredTask, called at almost-interrupt time (not for 68K) */
static pascal void DeferredTaskCallback(long param)
{
	long written;

	deferredTaskFired = true;

	MUTEX_LOCK(vars);

	if (Player_Paused())
		written=VC_SilenceBytes(currentBuffer,(ULONG)SOUND_BUFFER_SIZE);
	else 
		written=VC_WriteBytes(currentBuffer,(ULONG)SOUND_BUFFER_SIZE);

	MUTEX_UNLOCK(vars);
}

/* SoundCallback, called at interrupt time (not for 68K)  */
static pascal void SoundCallback(SndChannelPtr chan, SndCommand *cmd )
{
	SndCommand buffer   = { bufferCmd, 0, (long) &sndHeader };
	SndCommand callback = { callBackCmd, 0, 0 };

	/* Install current buffer */
	sndHeader.samplePtr = currentBuffer;
	SndDoImmediate( soundChannel, &buffer );

	/* Setup deferred task to fill next buffer */
	if(deferredTaskFired)
	{
		currentBuffer = (currentBuffer == sndBuffer1) ? sndBuffer2 : sndBuffer1;
		dtask.dtParam = (long) currentBuffer;
		
		deferredTaskFired = false;
		DTInstall((DeferredTaskPtr) &dtask);
	}

	/* Queue next callback */
	SndDoCommand( soundChannel, &callback, true );
}

#endif

static BOOL MAC_IsThere(void)
{
	NumVersion nVers;

	nVers=SndSoundManagerVersion();
	if (nVers.majorRev>=2)
		return 1; /* need SoundManager 2.0+ */
	else
		return 0;
}

static BOOL MAC_Init(void)
{
	static SndCallBackUPP callBackUPP = nil;
	OSErr err,iErr;
#if USE_SNDDOUBLEBUFFER						 // <AWE> avoids compiler warning.
	int i;
#endif
	long rate,maxrate,maxbits;
	long gestaltAnswer;
	NumVersion nVers;
	Boolean Stereo,StereoMixing,Audio16;
	Boolean NewSoundManager,NewSoundManager31;

#if USE_SNDDOUBLEBUFFER
	SndDoubleBufferPtr doubleBuffer;
#else
	if( callBackUPP == nil )
		callBackUPP = NewSndCallBackUPP( SoundCallback ); // <AWE> was "NewSndCallBackProc()".
#endif

	NewSoundManager31=NewSoundManager=false;

	nVers=SndSoundManagerVersion();
	if (nVers.majorRev>=3) {
		NewSoundManager=true;
		if (nVers.minorAndBugRev>=0x10)
			NewSoundManager31=true;
	} else
	  if (nVers.majorRev<2)
		return 1; /* failure, need SoundManager 2.0+ */

	iErr=Gestalt(gestaltSoundAttr,&gestaltAnswer);
	if (iErr==noErr) {
		Stereo=(gestaltAnswer & (1<<gestaltStereoCapability))!=0;
		StereoMixing=(gestaltAnswer & (1<<gestaltStereoMixing))!=0;
		Audio16=(gestaltAnswer & (1<<gestalt16BitSoundIO))!=0;
	} else {
		/* failure, couldn't get any sound info at all ? */
		Stereo=StereoMixing=Audio16=false;
	}

#if !TARGET_CPU_68K || !TARGET_RT_MAC_CFM
	if (NewSoundManager31) {
		iErr=GetSoundOutputInfo(0L,siSampleRate,(void*)&maxrate);
		if (iErr==noErr)
			iErr=GetSoundOutputInfo(0L,siSampleSize,(void*)&maxbits);
	}

	if (iErr!=noErr) {
#endif
		maxrate=rate22khz;

		if (NewSoundManager && Audio16)
			maxbits=16;
		else
			maxbits=8;
#if !TARGET_CPU_68K || !TARGET_RT_MAC_CFM
	}
#endif

	switch (md_mixfreq) {
		case 48000:rate=rate48khz;break;
		case 44100:rate=rate44khz;break;
		case 22254:rate=rate22khz;break;
		case 22050:rate=rate22050hz;break;
		case 11127:rate=rate11khz;break;
		case 11025:rate=rate11025hz;break;
		default:   rate=0;break;
	}

	if (!rate) {
		_mm_errno=MMERR_MAC_SPEED;
		return 1;
	}

	md_mode|=DMODE_SOFT_MUSIC|DMODE_SOFT_SNDFX;

	if ((md_mode&DMODE_16BITS)&&(maxbits<16))
		md_mode&=~DMODE_16BITS;

	if (!Stereo || !StereoMixing)
		md_mode&=~DMODE_STEREO;

	if (rate>maxrate)
		rate=maxrate;
	if (md_mixfreq>(maxrate>>16))
		md_mixfreq=maxrate>>16;

	err=SndNewChannel(&soundChannel,sampledSynth,
	                  (md_mode&DMODE_STEREO)?initStereo:initMono, callBackUPP );
	if(err!=noErr) {
		_mm_errno=MMERR_OPENING_AUDIO;
		return 1;
	}

#if USE_SNDDOUBLEBUFFER

	doubleHeader.dbhCompressionID=0;
	doubleHeader.dbhPacketSize   =0;
	doubleHeader.dbhSampleRate   =rate;
	doubleHeader.dbhSampleSize   =(md_mode&DMODE_16BITS)?16:8;
	doubleHeader.dbhNumChannels  =(md_mode&DMODE_STEREO)?2:1;
	doubleHeader.dbhDoubleBack   =NewSndDoubleBackProc(&MyDoubleBackProc);
    
	for(i=0;i<2;i++) {
		doubleBuffer=(SndDoubleBufferPtr)NewPtrClear(sizeof(SndDoubleBuffer)+
		                                             SOUND_BUFFER_SIZE);
		if(!doubleBuffer) {
			_mm_errno=MMERR_OUT_OF_MEMORY;
			return 1;
		}

		doubleBuffer->dbNumFrames=0;
		doubleBuffer->dbFlags=0;
		doubleBuffer->dbUserInfo[0]=SetCurrentA5();
		doubleBuffer->dbUserInfo[1]=0;

		doubleHeader.dbhBufferPtr[i]=doubleBuffer;
	}

#else

	sndBuffer1 = NewPtrClear(SOUND_BUFFER_SIZE);
	sndBuffer2 = NewPtrClear(SOUND_BUFFER_SIZE);
	if (sndBuffer1 == NULL || sndBuffer2 == NULL) {
		_mm_errno=MMERR_OUT_OF_MEMORY;
		return 1;
	}
	currentBuffer = sndBuffer1;
	
  	/* Setup sound header */
  	memset( &sndHeader, 0, sizeof(sndHeader) );
	sndHeader.numChannels = (md_mode&DMODE_STEREO)? 2: 1;
	sndHeader.sampleRate = rate;
	sndHeader.encode = extSH;
	sndHeader.baseFrequency = kMiddleC;
	sndHeader.numFrames = SOUND_BUFFER_SIZE >> (((md_mode&DMODE_STEREO)? 1: 0) + ((md_mode&DMODE_16BITS)?1: 0));
	sndHeader.sampleSize = (md_mode&DMODE_16BITS)? 16: 8;
	sndHeader.samplePtr = currentBuffer;
    
  	/* Setup deferred task record */
	memset( &dtask, 0, sizeof(dtask) );
	dtask.qType = dtQType;
	dtask.dtFlags = 0;
	dtask.dtAddr = NewDeferredTaskUPP(DeferredTaskCallback); // <AWE> was "NewDeferredTaskProc()".
	dtask.dtReserved = 0;
	deferredTaskFired = true;

#endif
    
	return VC_Init();
}

static void MAC_Exit(void)
{
#if USE_SNDDOUBLEBUFFER						// <AWE> avoids compiler warning.
	int i;
#endif

	SndDisposeChannel(soundChannel,true);
	soundChannel=NULL;
	
#if USE_SNDDOUBLEBUFFER

	DisposeRoutineDescriptor((UniversalProcPtr)doubleHeader.dbhDoubleBack);
	doubleHeader.dbhDoubleBack=NULL;

	for(i=0;i<doubleHeader.dbhNumChannels;i++) {
		DisposePtr((Ptr)doubleHeader.dbhBufferPtr[i]);
		doubleHeader.dbhBufferPtr[i]=NULL;
	}

#else

	DisposePtr( sndBuffer1 );
	sndBuffer1 = NULL;
	DisposePtr( sndBuffer2 );
	sndBuffer2 = NULL;

#endif

	VC_Exit();
}

static BOOL MAC_PlayStart(void)
{
#if USE_SNDDOUBLEBUFFER

	OSErr err;

	MyDoubleBackProc(soundChannel,doubleHeader.dbhBufferPtr[0]);
	MyDoubleBackProc(soundChannel,doubleHeader.dbhBufferPtr[1]);

	err=SndPlayDoubleBuffer(soundChannel,&doubleHeader);
	if(err!=noErr) {
		_mm_errno=MMERR_MAC_START;
		return 1;
	}

#else

	SndCommand callback = { callBackCmd, 0, 0 };

	callback.param2 = SetCurrentA5();
	SndDoCommand( soundChannel, &callback, true );

 #endif
       
	return VC_PlayStart();
}

static void MAC_PlayStop(void)
{
	SndCommand quiet = { quietCmd, 0, 0 };
	SndCommand flush = { flushCmd, 0, 0 };

	SndDoImmediate(soundChannel,&quiet);
	SndDoImmediate(soundChannel,&flush);

	VC_PlayStop();
}

static void MAC_Update(void)
{
	return;
}

MIKMODAPI MDRIVER drv_mac={
    NULL,
    "Mac Driver (Carbonized)",
    "Macintosh Sound Manager Driver v2.0",
    0,255,
	"mac",

	NULL,
    MAC_IsThere,
    VC_SampleLoad,
    VC_SampleUnload,
    VC_SampleSpace,
    VC_SampleLength,
    MAC_Init,
    MAC_Exit,
    NULL,
    VC_SetNumVoices,
    MAC_PlayStart,
    MAC_PlayStop,
    MAC_Update,
    NULL,
    VC_VoiceSetVolume,
    VC_VoiceGetVolume,
    VC_VoiceSetFrequency,
    VC_VoiceGetFrequency,
    VC_VoiceSetPanning,
    VC_VoiceGetPanning,
    VC_VoicePlay,
    VC_VoiceStop,
    VC_VoiceStopped,
    VC_VoiceGetPosition,
    VC_VoiceRealVolume
};

/* ex:set ts=4: */

