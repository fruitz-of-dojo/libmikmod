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

  $Id: drv_osx.c,v 1.0 2001/10/18 00:00:00 awe Exp $

  Driver for output via CoreAudio [MacOS X and Darwin].

==============================================================================*/

/*
        
	Written by Axel Wefers <awe@fruitz-of-dojo.de>
        
        Notes:
        - if HAVE_PTHREAD (config.h) is defined, an extra thread will be created to fill the buffers.
        - if HAVE_PTHREAD is defined, a double buffered method will be used.
        - if an unsupported frequency is selected [md_mixfreq], the native device frequency is used.
        - if mono playback is selected and is not supported by the device, we will emulate mono
          playback.
        - if stereo/surround playback is selected and is not supported by the device, DMODE_STEREO
          will be deactivated automagically.
        
*/

//------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma mark Includes

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mikmod_internals.h"

#include <CoreAudio/AudioHardware.h>

#pragma mark -

//------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma mark Defines

#define SOUND_BUFFER_SCALE_8BIT		(1.0f / 128.0f)		/* CoreAudio requires float input.    */
#define SOUND_BUFFER_SCALE_16BIT	(1.0f / 32768.0f)	/* CoreAudio requires float input.    */
#define SOUND_BUFFER_SIZE			4096				/* The buffersize libmikmod will use. */

#pragma mark -

//------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma mark Macros

#define CHECK_ERROR(ERRNO, RESULT)		if (RESULT != kAudioHardwareNoError)										\
                                        {																			\
                                            _mm_errno = ERRNO;														\
                                            return(1);																\
                                        }

#define SET_PROPS()						if (AudioDeviceSetProperty (gSoundDeviceID, NULL, 0, 0,						\
																	kAudioDevicePropertyStreamFormat,				\
																	myPropertySize, &mySoundBasicDescription))		\
										{																			\
											CHECK_ERROR																\
											(																		\
												MMERR_OSX_BAD_PROPERTY,												\
												AudioDeviceGetProperty (gSoundDeviceID, 0, 0,						\
																		kAudioDevicePropertyStreamFormat,			\
																		&myPropertySize, &mySoundBasicDescription)	\
											);																		\
										}

#define SET_STEREO()					switch (mySoundBasicDescription.mChannelsPerFrame)							\
										{																			\
											case 1:																	\
												md_mode				&= ~DMODE_STEREO;								\
												gBufferMono2Stereo	= 0;											\
												break;																\
											case 2:																	\
												if (md_mode & DMODE_STEREO)											\
												{																	\
													gBufferMono2Stereo = 0;											\
												}																	\
												else																\
												{																	\
													gBufferMono2Stereo = 1;											\
												}																	\
												break;																\
											default:																\
												_mm_errno = MMERR_OSX_SET_STEREO;									\
												return(1);															\
										}

#define FILL_BUFFER(SIZE)				if (Player_Paused())														\
										{																			\
											MUTEX_LOCK (vars);														\
											VC_SilenceBytes (gSoundBuffer, (ULONG) (SIZE));							\
											MUTEX_UNLOCK (vars);													\
										}																			\
										else																		\
										{																			\
											MUTEX_LOCK (vars);														\
											VC_WriteBytes (gSoundBuffer, (ULONG) (SIZE));							\
											MUTEX_UNLOCK (vars);													\
										}

#define CONVERT_BUFFER(SCALE)			if (gBufferMono2Stereo)														\
										{																			\
											for (i = 0; i < SOUND_BUFFER_SIZE >> 1; i++)							\
											{																		\
												*myOutBuffer = (*myInBuffer++) * SCALE;								\
												myOutBuffer++;														\
												*myOutBuffer = *(myOutBuffer - 1);									\
												myOutBuffer++;														\
											}																		\
										}																			\
										else																		\
										{																			\
											for (i = 0; i < SOUND_BUFFER_SIZE; i++)									\
											{																		\
												*myOutBuffer = (*myInBuffer++) * SCALE;								\
												myOutBuffer++;														\
											}																		\
										}

#define AUDIO_IO(SIZE)					FILL_BUFFER (gInBufferSize);												\
										CONVERT_BUFFER (SIZE);
                                    
#define AUDIO_IO_PTHREAD(SIZE)			if (gBufferFilled)															\
										{																			\
											/*pthread_mutex_lock (&gBufferMutex);*/									\
											gSoundBuffer = gSoundBackBuffer;										\
											gSoundBackBuffer = (UInt8 *) myInBuffer;								\
											gBufferFilled = 0;														\
											pthread_cond_signal (&gBufferCondition);								\
											/*pthread_mutex_unlock (&gBufferMutex);*/								\
											CONVERT_BUFFER (SIZE);													\
										}

#pragma mark -

//------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma mark Globals

#ifdef HAVE_PTHREAD

static Boolean				gBufferFilled = 0;
static pthread_t			gBufferFillThread;
static pthread_mutex_t		gBufferMutex;
static pthread_cond_t		gBufferCondition;
static Boolean				gExitBufferFillThread = 0;
static unsigned char *		gSoundBackBuffer = NULL;

#endif /* HAVE_PTHREAD */

static AudioDeviceID 		gSoundDeviceID;
static UInt32				gInBufferSize;
static unsigned char *		gSoundBuffer = NULL;
static volatile Boolean		gIOProcIsInstalled = 0;
static Boolean				gDeviceHasStarted = 0;
static Boolean				gBufferMono2Stereo = 0;
static OSStatus				(*gAudioIOProc) (AudioDeviceID, const AudioTimeStamp *,
											 const AudioBufferList *, const AudioTimeStamp *,
											 AudioBufferList *, const AudioTimeStamp *, void *);

#pragma mark -

//------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma mark Function Prototypes

#ifdef HAVE_PTHREAD

static void *	OSX_FillBuffer (void *);

#endif /* HAVE_PTHREAD */

static OSStatus OSX_AudioIOProc8Bit (AudioDeviceID, const AudioTimeStamp *, const AudioBufferList *,
                                     const AudioTimeStamp *, AudioBufferList *, const AudioTimeStamp *,
                                     void *);
static OSStatus OSX_AudioIOProc16Bit (AudioDeviceID, const AudioTimeStamp *, const AudioBufferList *,
                                      const AudioTimeStamp *, AudioBufferList *, const AudioTimeStamp *,
                                      void *);
static BOOL		OSX_IsPresent (void);
static BOOL		OSX_Init (void);
static void		OSX_Exit (void);
static BOOL		OSX_PlayStart (void);
static void 	OSX_PlayStop (void);
static void		OSX_Update (void);

#pragma mark -

//------------------------------------------------------------------------------------------------------------------------------------------------------------

#ifdef HAVE_PTHREAD

static void *	OSX_FillBuffer (void *theID)
{
    while (1)
    {
        // shall the thread exit?
        if (gExitBufferFillThread)
		{
			pthread_exit (NULL);
		}
		
        // fill the buffer...
        pthread_mutex_lock (&gBufferMutex);
        FILL_BUFFER (gInBufferSize);
        gBufferFilled = 1;
        pthread_mutex_unlock (&gBufferMutex);
        
        // wait for the next buffer request...
        pthread_cond_wait (&gBufferCondition, &gBufferMutex);
    }
	
    return (theID);
}

#endif /* HAVE_PTHREAD */

//------------------------------------------------------------------------------------------------------------------------------------------------------------

static OSStatus OSX_AudioIOProc8Bit (AudioDeviceID				inDevice,
                                     const AudioTimeStamp *		inNow,
                                     const AudioBufferList *	inInputData,
                                     const AudioTimeStamp *		inInputTime,
                                     AudioBufferList *			outOutputData, 
                                     const AudioTimeStamp *		inOutputTime,
                                     void *						inClientData)
{
	// fix for a possible crash on exit (race condition in CoreAudio?)
	if (gIOProcIsInstalled != 0)
	{
		register float *	myOutBuffer = (float *) outOutputData->mBuffers[0].mData;
		register UInt8 *	myInBuffer  = (UInt8 *) gSoundBuffer;
		register UInt32		i;

#ifdef HAVE_PTHREAD

		AUDIO_IO_PTHREAD (SOUND_BUFFER_SCALE_8BIT);

#else

		AUDIO_IO (SOUND_BUFFER_SCALE_8BIT);

#endif /* HAVE_PTHREAD */
	}
	
	return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------

static OSStatus OSX_AudioIOProc16Bit (AudioDeviceID				inDevice,
                                      const AudioTimeStamp *	inNow,
                                      const AudioBufferList *	inInputData,
                                      const AudioTimeStamp *	inInputTime,
                                      AudioBufferList *			outOutputData, 
                                      const AudioTimeStamp *	inOutputTime,
                                      void *					inClientData)
{
	// fix for a possible crash on exit (race condition in CoreAudio?)
	if (gIOProcIsInstalled != 0)
	{
		register float *	myOutBuffer = (float *) outOutputData->mBuffers[0].mData;
		register SInt16 *	myInBuffer  = (SInt16 *) gSoundBuffer;
		register UInt32		i;

#ifdef HAVE_PTHREAD

		AUDIO_IO_PTHREAD (SOUND_BUFFER_SCALE_16BIT);

#else

		AUDIO_IO (SOUND_BUFFER_SCALE_16BIT);

#endif /* HAVE_PTHREAD */
	}
	
	return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------

static BOOL OSX_IsPresent (void)
{
    // bad boy... have to find a better way!
	return AudioHardwareGetProperty ? 1 : 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------

static BOOL OSX_Init (void)
{
    AudioStreamBasicDescription		mySoundBasicDescription;
 	UInt32							myBufferByteCount;
	UInt32							myPropertySize = sizeof (gSoundDeviceID);

    // get the device...
    CHECK_ERROR
    (
        MMERR_DETECTING_DEVICE,
        AudioHardwareGetProperty (kAudioHardwarePropertyDefaultOutputDevice, &myPropertySize, &gSoundDeviceID)
    );
 
	if (gSoundDeviceID == kAudioDeviceUnknown)
    {
        _mm_errno = MMERR_OSX_UNKNOWN_DEVICE;
        return 1;
    }

    // get the device format...
    myPropertySize = sizeof (mySoundBasicDescription);
	
    CHECK_ERROR
    (
        MMERR_OSX_BAD_PROPERTY,
        AudioDeviceGetProperty (gSoundDeviceID, 0, 0, kAudioDevicePropertyStreamFormat, &myPropertySize, &mySoundBasicDescription)
    );

    // set up basic md_mode, just to be secure...
    md_mode |= DMODE_SOFT_MUSIC | DMODE_SOFT_SNDFX;

    // try the selected mix frequency, if failure, fall back to native frequency...
    if (mySoundBasicDescription.mSampleRate != md_mixfreq)
    {
        mySoundBasicDescription.mSampleRate = md_mixfreq;
        SET_PROPS ();
        md_mixfreq = mySoundBasicDescription.mSampleRate;
    }

    // try selected channels, if failure select native channels...
    switch (md_mode & DMODE_STEREO)
    {
        case 0:
            if (mySoundBasicDescription.mChannelsPerFrame != 1)
            {
                mySoundBasicDescription.mChannelsPerFrame = 1;
                SET_PROPS ();
                SET_STEREO ();
            }
            break;
        case 1:
            if (mySoundBasicDescription.mChannelsPerFrame != 2)
            {
                mySoundBasicDescription.mChannelsPerFrame = 2;
                SET_PROPS();
                SET_STEREO();
            }
            break;
    }

    // linear PCM is required...
    if (mySoundBasicDescription.mFormatID != kAudioFormatLinearPCM)
    {
        _mm_errno = MMERR_OSX_UNSUPPORTED_FORMAT;
        return 1;
    }

    // prepare the buffers...
    if (gBufferMono2Stereo)
    {
        gInBufferSize = SOUND_BUFFER_SIZE >> 1;
    }
    else
    {
        gInBufferSize = SOUND_BUFFER_SIZE;
    }
	
    if (md_mode & DMODE_16BITS)
    {
        gInBufferSize *= sizeof(SInt16);
        gAudioIOProc   = OSX_AudioIOProc16Bit;
    }
    else
    {
        gInBufferSize *= sizeof(UInt8);
        gAudioIOProc   = OSX_AudioIOProc8Bit;
    }
	
    myBufferByteCount = SOUND_BUFFER_SIZE * sizeof(float);
	
    CHECK_ERROR
    (
        MMERR_OSX_BUFFER_ALLOC,
        AudioDeviceSetProperty (gSoundDeviceID, NULL, 0, 0, kAudioDevicePropertyBufferSize, sizeof(myBufferByteCount), &myBufferByteCount)
    );
    
    // add our audio IO procedure....
    CHECK_ERROR
    (
        MMERR_OSX_ADD_IO_PROC,
        AudioDeviceAddIOProc (gSoundDeviceID, gAudioIOProc, NULL)
    );
	
    gIOProcIsInstalled = 1;

    // get the buffer memory...
    if ((gSoundBuffer = malloc(gInBufferSize)) == NULL)
    {
        _mm_errno = MMERR_OUT_OF_MEMORY;
        return 1;
    }

#ifdef HAVE_PTHREAD

    // some thread init...
    pthread_mutex_init (&gBufferMutex, NULL);
    pthread_cond_init (&gBufferCondition, NULL);
	
    if ((gSoundBackBuffer = malloc(gInBufferSize)) == NULL)
    {
        free (gSoundBuffer);
        gSoundBuffer = NULL;
        _mm_errno = MMERR_OUT_OF_MEMORY;
        return 1;
    }

#endif /* HAVE_PTHREAD */

    // last not least init mikmod...
    return VC_Init ();
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------

static void OSX_Exit (void)
{
    if (gDeviceHasStarted)
    {
		gDeviceHasStarted = 0;
		
        // stop the audio device...
        AudioDeviceStop (gSoundDeviceID, gAudioIOProc);

#ifdef HAVE_PTHREAD

        // finish the fill buffer thread off...
        pthread_mutex_lock (&gBufferMutex);
        gExitBufferFillThread = 1;
        pthread_mutex_unlock (&gBufferMutex);
        pthread_cond_signal (&gBufferCondition);
        pthread_join (gBufferFillThread, NULL);
    }
    
    // destroy other thread related stuff...
    pthread_mutex_destroy (&gBufferMutex);
    pthread_cond_destroy (&gBufferCondition);

#else

    }

#endif /* HAVE_PTHREAD */

    // remove the audio IO proc...
    if (gIOProcIsInstalled)
    {
		gIOProcIsInstalled = 0;
		
        AudioDeviceRemoveIOProc (gSoundDeviceID, gAudioIOProc);
    }

    // free up the sound buffer...
    if (gSoundBuffer)
    {
        free (gSoundBuffer);
        gSoundBuffer = NULL;
    }

#ifdef HAVE_PTHREAD

    // free up the back buffer...
    if (gSoundBackBuffer)
    {
        free (gSoundBackBuffer);
        gSoundBuffer = NULL;
    }
    
#endif /* HAVE_PTHREAD */
    
    VC_Exit ();
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------

static BOOL OSX_PlayStart (void)
{
    // start virtch...
    if (VC_PlayStart ())
    {
        return 1;
    }
    
    // just for security: audio device already playing?
    if (gDeviceHasStarted)
	{
		return 0;
	}
	
#ifdef HAVE_PTHREAD

    // start the buffer fill thread...
    gExitBufferFillThread = 0;
	
    if (pthread_create (&gBufferFillThread, NULL, OSX_FillBuffer , NULL))
    {
        _mm_errno = MMERR_OSX_PTHREAD;
        return 1;
    }

#endif /* HAVE_PTHREAD */

    // start the audio IO Proc...
    if (AudioDeviceStart (gSoundDeviceID, gAudioIOProc))
    {
        _mm_errno = MMERR_OSX_DEVICE_START;
        return 1;
    }
	
    gDeviceHasStarted = 1;
    
    return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------

static void OSX_PlayStop (void)
{
 
    if (gDeviceHasStarted)
    {
        // stop the audio IO Proc...
        AudioDeviceStop (gSoundDeviceID, gAudioIOProc);
        gDeviceHasStarted = 0;

#ifdef HAVE_PTHREAD

        // finish the fill buffer thread off...
        pthread_mutex_lock (&gBufferMutex);
        gExitBufferFillThread = 1;
        pthread_mutex_unlock (&gBufferMutex);
        pthread_cond_signal (&gBufferCondition);
        pthread_join (gBufferFillThread, NULL);

#endif /* HAVE_PTHREAD */

    }
    
    // finally tell virtch that playback has stopped...
    VC_PlayStop ();
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------

static void OSX_Update (void)
{
    // do nothing...
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------

MIKMODAPI MDRIVER drv_osx =	{
								NULL,
								"CoreAudio Driver",
								"CoreAudio Driver v1.0",
								0,255,
								"osx",
								
								NULL,
								OSX_IsPresent,
								VC_SampleLoad,
								VC_SampleUnload,
								VC_SampleSpace,
								VC_SampleLength,
								OSX_Init,
								OSX_Exit,
								NULL,
								VC_SetNumVoices,
								OSX_PlayStart,
								OSX_PlayStop,
								OSX_Update,
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

//------------------------------------------------------------------------------------------------------------------------------------------------------------
