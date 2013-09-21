//------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// ShellPlay v1.0 - a simple and cheap command line tool for testing purposes.
//
// coding by aWe/fRUITZ oF dOJO!
//
//------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma mark Includes

#include	<stdio.h>
#include	<stdarg.h>
#include	<unistd.h>

#include	"MikMod.h"

#pragma mark -

//------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma mark Defines

// undef to include all drivers:
#define	USE_DRIVER	drv_osx

// undef to include all loaders:
//#define	USE_LOADER	load_xm

// enables all XM hacks for 100% fasttracker compatibility...
#define	ENABLE_FULL_XM_COMPATIBILITY



#if defined (USE_DRIVER)
#define REGISTER_DRIVER()	MikMod_RegisterDriver (&USE_DRIVER);
#else
#define REGISTER_DRIVER()	MikMod_RegisterAllDrivers ();
#endif /* USE_DRIVER */

#if defined (USE_LOADER)
#define REGISTER_LOADER()	MikMod_RegisterLoader (&USE_LOADER);
#else
#define REGISTER_LOADER()	MikMod_RegisterAllLoaders ();
#endif /* USE_LOADER */

#pragma mark -

//------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma mark Function Prototypes

int	main(int argc, char *argv[]);

#pragma mark -

//------------------------------------------------------------------------------------------------------------------------------------------------------------

int	main(int argc, char *argv[])
{
    MODULE *	pModule;

    fprintf(stderr, "\n### ShellPlay v1.0  - a simple module player. ###");
    fprintf(stderr, "\n###       Coding by aWe/fRUITZ oF dOJO!       ###\n\n");
    
    if(argc != 2)
    {
        fprintf(stderr, "Wrong parameters. Usage:\n");
        fprintf(stderr, "'%s Module'\n\n", argv[0]);

        exit(1);
    }

    // register the drivers.
    REGISTER_DRIVER ();

    // register the module loaders.
    REGISTER_LOADER ();

    // set up the playback quality.
    md_mode		= DMODE_SOFT_MUSIC | DMODE_HQMIXER | DMODE_INTERP | DMODE_16BITS | DMODE_STEREO;
    md_mixfreq	= 44100;
    md_reverb	= 0;

    // initialize mikmod. 
    if (MikMod_Init(""))
    {
        fprintf(stderr, "Could not initialize sound, reason: %s\n\n", MikMod_strerror(MikMod_errno));
        return(1);
    }

    // load the module.
    pModule = Player_Load(argv[argc-1], 255, 0);
    
    // start playback.
    if (pModule)
    {
        // some last minute module setup:
#ifdef ENABLE_FULL_XM_COMPATIBILITY
		pModule->flags |= UF_FT2QUIRKS | UF_XMPERIODS;
#endif /* ENABLE_FULL_XM_COMPATIBILITY */

        pModule->loop		= 0;
        pModule->fadeout	= 1;
		
        fprintf(stderr, "Playing module '%s' ('ctrl-c' aborts)... \n", argv[argc-1]);

        // start playback:
        Player_Start(pModule);
        
        // wait until the module has finished!
        while (Player_Active())
        {
            // not required with MacOS X, but let's use it for compatibility reasons:
            MikMod_Update();
            
            // reduce processor usage:
            sleep(1);
		}
        
        // finished!
		fprintf(stderr, "Playing stopped...\n\n");
        
        // get rid of the module.
        Player_Stop();
        Player_Free(pModule);
    }
    else
    {
        fprintf(stderr, "Could not load module '%s', reason: %s\n\n", argv[argc-1], MikMod_strerror(MikMod_errno));
    }
    
    // shutdown libmikmod.
    MikMod_Exit();
    
    // good bye!
    return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------
