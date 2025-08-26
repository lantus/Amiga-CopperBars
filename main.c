#include "support/gcc8_c_support.h"
#include <exec/types.h>
#include <exec/exec.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <hardware/custom.h>
#include <hardware/intbits.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/dos.h>

// Global variables
struct ExecBase *SysBase;
volatile struct Custom *custom;
struct DosLibrary *DOSBase;
struct GfxBase *GfxBase = NULL;
struct ViewPort *viewPort = NULL;
struct RasInfo rasInfo;
struct BitMap *bitMap = NULL;
UWORD *copperPtr = NULL;

static UWORD SystemInts;
static UWORD SystemDMA;
static UWORD SystemADKCON;
static volatile APTR VBR = 0;
static APTR SystemIrq;
struct View *ActiView;

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 256
#define BPL_DEPTH 1
#define BPL_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 8) // Bytes per bitplane
#define NUM_COLORS (1 << BPL_DEPTH)

/* write definitions for dmaconw */
#define DMAF_SETCLR  0x8000
#define DMAF_AUDIO   0x000F   /* 4 bit mask */
#define DMAF_AUD0    0x0001
#define DMAF_AUD1    0x0002
#define DMAF_AUD2    0x0004
#define DMAF_AUD3    0x0008
#define DMAF_DISK    0x0010
#define DMAF_SPRITE  0x0020
#define DMAF_BLITTER 0x0040
#define DMAF_COPPER  0x0080
#define DMAF_RASTER  0x0100
#define DMAF_MASTER  0x0200
#define DMAF_BLITHOG 0x0400
#define DMAF_ALL     0x01FF   /* all dma channels */

// Global variables
UBYTE *bitplanes = NULL;
UWORD *copperList = NULL;
struct Interrupt vblankInt;
BOOL running = TRUE;
 
__attribute__((always_inline)) inline short MouseLeft() { return !((*(volatile UBYTE*)0xbfe001) & 64); }
__attribute__((always_inline)) inline short MouseRight() { return !((*(volatile UWORD*)0xdff016) & (1 << 10)); }

void BlitterClearScreen(void);

void WaitLine(USHORT line) 
{
	while (1) 
    {
		volatile ULONG vpos=*(volatile ULONG*)0xDFF004;
		if(((vpos >> 8) & 511) == line)
			break;
	}
}

static APTR GetVBR(void) {
    APTR vbr = 0;
    UWORD getvbr[] = { 0x4e7a, 0x0801, 0x4e73 }; // MOVEC.L VBR,D0 RTE
    if (SysBase->AttnFlags & AFF_68010)
        vbr = (APTR)Supervisor((ULONG (*)())getvbr);
    return vbr;
}

void SetInterruptHandler(APTR interrupt) {
    *(volatile APTR*)(((UBYTE*)VBR) + 0x6c) = interrupt;
}

APTR GetInterruptHandler() {
    return *(volatile APTR*)(((UBYTE*)VBR) + 0x6c);
}

void WaitVbl() {
    while (1) {
        volatile ULONG vpos = *(volatile ULONG*)0xdff004;
        vpos &= 0x1ff00;
        if (vpos != (311 << 8)) break;
    }
    while (1) {
        volatile ULONG vpos = *(volatile ULONG*)0xdff004;
        vpos &= 0x1ff00;
        if (vpos == (311 << 8)) break;
    }
}
 
void TakeSystem() 
{
	Forbid();
	//Save current interrupts and DMA settings so we can restore them upon exit. 
	SystemADKCON=custom->adkconr;
	SystemInts=custom->intenar;
	SystemDMA=custom->dmaconr;
	ActiView=GfxBase->ActiView; //store current view

	LoadView(0);
	WaitTOF();
	WaitTOF();

	WaitVbl();
	WaitVbl();

	OwnBlitter();
	WaitBlit();	
	Disable();
	
	custom->intena=0x7fff;//disable all interrupts
	custom->intreq=0x7fff;//Clear any interrupts that were pending
	
	custom->dmacon=0x7fff;//Clear all DMA channels

	//set all colors black
	for(int a=0;a<32;a++)
		custom->color[a]=0;

	WaitVbl();
	WaitVbl();

	VBR=GetVBR();
	SystemIrq=GetInterruptHandler(); //store interrupt register
}

void FreeSystem()
{ 
	WaitVbl();
	WaitBlit();
    
	custom->intena=0x7fff;//disable all interrupts
	custom->intreq=0x7fff;//Clear any interrupts that were pending
	custom->dmacon=0x7fff;//Clear all DMA channels

	//restore interrupts
	SetInterruptHandler(SystemIrq);

	/*Restore system copper list(s). */
	custom->cop1lc=(ULONG)GfxBase->copinit;
	custom->cop2lc=(ULONG)GfxBase->LOFlist;
	custom->copjmp1=0x7fff; //start coppper

	/*Restore all interrupts and DMA settings. */
	custom->intena=SystemInts|0x8000;
	custom->dmacon=SystemDMA|0x8000;
	custom->adkcon=SystemADKCON|0x8000;

	WaitBlit();	
	DisownBlitter();
	Enable();

	LoadView(ActiView);
	WaitTOF();
	WaitTOF();

	Permit();
} 
 
// Copper bar parameters
 
#define NUM_BARS 28
 
typedef struct 
{
    UWORD yPos;     // Current Y position of bar
    UWORD height;   // Height of bar in scanlines
    UWORD color;    // RGB color (12-bit, 0xRGB)
    WORD speed;     // Pixels per frame
} copperBars;

copperBars bars[NUM_BARS] =  
{
    {52, 2, 0x300, 1},  // Red bar
    {54, 2, 0x600, 1},  // Red bar
    {56, 2, 0xA00, 1},  // Red bar
    {58, 2, 0xF00, 1},  // Red bar
    {60, 2, 0xA00, 1},  // Red bar
    {62, 2, 0x600, 1},  // Red bar
    {64, 2, 0x300, 1},  // Red bar
 
    {66, 2, 0x030, 1},  // Green bar
    {68, 2, 0x060, 1},  // Green bar
    {70, 2, 0x090, 1},  // Green bar
    {72, 2, 0x0F0, 1},  // Green bar
    {74, 2, 0x090, 1},  // Green bar
    {76, 2, 0x060, 1},  // Green bar
    {78, 2, 0x030, 1},  // Green bar

    {80, 2, 0x003, 1},  // Blue bar
    {82, 2, 0x006, 1},  // Blue bar
    {84, 2, 0x009, 1},  // Blue bar
    {86, 2, 0x00F, 1},  // Blue bar
    {88, 2, 0x009, 1},  // Blue bar
    {90, 2, 0x006, 1},  // Blue bar
    {92, 2, 0x003, 1},  // Blue bar

    {94, 2, 0x303, 1},  // Pink bar
    {96, 2, 0x606, 1 },  // Pink bar
    {98, 2, 0x909, 1},  // Pink bar
    {100, 2, 0xF0F, 1},  // Pink bar
    {102, 2, 0x909, 1},  // Pink bar
    {104, 2, 0x606, 1},  // Pink bar
    {106, 2, 0x303, 1}  // Pink bar     
   
};

 
 
static __attribute__((interrupt)) void InterruptHandler() 
{
	custom->intreq=(1<<INTB_VERTB);  

    // Update bar positions

    UWORD *copPtr = copperList + 14; // Skip initial setup (7 instructions * 2 words)
 
    for (int i = 0; i < NUM_BARS; i++) {
        bars[i].yPos += bars[i].speed;
        if (bars[i].yPos < 20) {
            bars[i].yPos = 20;
            bars[i].speed = -bars[i].speed; // Reverse at top
        }
        if (bars[i].yPos > SCREEN_HEIGHT - bars[i].height - 60) {
            bars[i].yPos = SCREEN_HEIGHT - bars[i].height - 60; // Clamp bottom
            bars[i].speed = -bars[i].speed; // Reverse at bottom
        }
    }

    // Sort them 
 
    for (int i = 1; i < NUM_BARS; i++) 
    {
        copperBars key = bars[i];

        int j = i - 1;

        while (j >= 0 && bars[j].yPos > key.yPos) 
        {
            bars[j + 1] = bars[j];
            j--;
        }
        bars[j + 1] = key;
    }   
   
   
    for (int i = 0; i < NUM_BARS; i++ ) 
    {
        bars[i].yPos += bars[i].speed;
        
        UWORD y = bars[i].yPos+0x2c; // PAL offset (~$2C)

        *copPtr++ = (y << 8) | 0x11; // WAIT (y, $0x11)
        *copPtr++ = 0xFFFE; 
        *copPtr++ = 0x0180; // Move COLOR00
        *copPtr++ = bars[i].color;
 
        y += bars[i].height;
        *copPtr++ = (y << 8) | 0x11;
        *copPtr++ = 0xFFFE;
        *copPtr++ = 0x0180;
        *copPtr++ = 0x0000; // Black background    
    }
    // Update Copper list for bars
   
 
    *copPtr++ = 0xFFFF; // End Copper list
    *copPtr++ = 0xFFFE;

  
}

void BlitterClearScreen(void) 
{
    while (custom->dmaconr & 0x4000); // Wait for Blitter

    custom->bltcon0 = 0x0100;
    custom->bltcon1 = 0x0000;
    custom->bltafwm = 0xFFFF;
    custom->bltalwm = 0xFFFF;
    custom->bltdmod = 40 - (SCREEN_WIDTH / 8);
    
    for (int i = 0; i < BPL_DEPTH; i++) 
    {
        custom->bltdpt = (APTR)((ULONG)bitplanes + (i * BPL_SIZE));
        custom->bltsize = (SCREEN_HEIGHT << 6) | (SCREEN_WIDTH / 16);
        while (custom->dmaconr & 0x4000);
    }
}

void SetupCopper()
{
    // Initialize Copper list
    UWORD *copPtr = copperList;

    *copPtr++ = 0x008E; *copPtr++ = 0x2C81; // DIWSTRT (PAL)
    *copPtr++ = 0x0090; *copPtr++ = 0x2CC1; // DIWSTOP
    *copPtr++ = 0x0092; *copPtr++ = 0x0038; // DDFSTRT
    *copPtr++ = 0x0094; *copPtr++ = 0x00D0; // DDFSTOP
    *copPtr++ = 0x0100; *copPtr++ = 0x0200; // BPLCON0 (1 bitplanes)
    *copPtr++ = 0x0108; *copPtr++ = 0x0000; // BPL1MOD
    *copPtr++ = 0x010A; *copPtr++ = 0x0000; // BPL2MOD

    // Set bitplane pointers
    
    ULONG bplAddr = (ULONG)bitplanes;

    for (int i = 0; i < BPL_DEPTH; i++) 
    {
        *copPtr++ = 0x00E0 + (i * 1); // BPLxPTH
        *copPtr++ = bplAddr >> 16;
        *copPtr++ = 0x00E2 + (i * 1); // BPLxPTL
        *copPtr++ = bplAddr & 0xFFFF;
        bplAddr += BPL_SIZE;
    }
 

    WaitBlit();
 
    return;
}
 
// Cleanup
void DisableCopper(void)
{
    if (copperList)
    {
        custom->cop1lc = 0; // Disable Copper
        FreeMem(copperList, 1024);
    }

    if (bitplanes) FreeMem(bitplanes, BPL_SIZE * BPL_DEPTH);
    
}


int main(void)
{
    SysBase = *((struct ExecBase**)4UL);
	custom = (struct Custom*)0xdff000;

    // Allocate CHIP RAM for bitplanes
    bitplanes = AllocMem(BPL_SIZE * BPL_DEPTH, MEMF_CHIP | MEMF_CLEAR);
   
    // Allocate CHIP RAM for Copper list
    copperList = AllocMem(1024, MEMF_CHIP | MEMF_CLEAR);
     
	// We will use the graphics library only to locate and restore the system copper list once we are through.

	GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library",0);
	if (!GfxBase)
		Exit(0);

	// used for printing
	DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
	if (!DOSBase)
		Exit(0);

    Delay(10);

	TakeSystem();
	WaitVbl();
 
    SetupCopper();
  
    custom->cop1lc = (ULONG)copperList; // Set Copper list
 
    WaitVbl();

	custom->dmacon = DMAF_BLITTER;//disable blitter dma for copjmp bug
	custom->copjmp1 = 0x7fff; //start coppper
	custom->dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_RASTER | DMAF_COPPER | DMAF_BLITTER;

    SetInterruptHandler((APTR)InterruptHandler);
 
    custom->intena = INTF_SETCLR | INTF_INTEN | INTF_VERTB;
    custom->intreq=(1<<INTB_VERTB);//reset vbl req

	while(!MouseLeft()) 
    {		 
 
        WaitBlit();   
	}

    DisableCopper();
    FreeSystem();
 
    return 0;
}