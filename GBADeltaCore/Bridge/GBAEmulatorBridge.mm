//
//  GBAEmulatorBridge.m
//  GBADeltaCore
//
//  Created by Riley Testut on 6/3/16.
//  Copyright © 2016 Riley Testut. All rights reserved.
//

#import "GBAEmulatorBridge.h"
#import "GBASoundDriver.h"

#import <CoreMotion/CoreMotion.h>

// VBA-M
#include "System.h"
#include "gba/Sound.h"
#include "gba/GBA.h"
#include "gba/Globals.h"
#include "../BGM/rom.h"
#include "../BGM/bgm_mirror.h"
#include "gba/Cheats.h"
#include "gba/RTC.h"
#include "gba/Sound.h"
#include "common/Patch.h"
#include "Util.h"

#include <sys/time.h>
#include <memory>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>

// DeltaCore
#import <GBADeltaCore/GBADeltaCore.h>
#import <DeltaCore/DeltaCore.h>
#import <DeltaCore/DeltaCore-Swift.h>

#if STATIC_LIBRARY
#import "GBADeltaCore-Swift.h"
#import "GBATypes.h"
#else
#import <GBADeltaCore/GBADeltaCore-Swift.h>
#endif

// Required vars, used by the emulator core
//
int  systemRedShift = 19;
int  systemGreenShift = 11;
int  systemBlueShift = 3;
int  systemColorDepth = 32;
int  systemVerbose;
int  systemSaveUpdateCounter = 0;
int  systemFrameSkip;
uint32_t  systemColorMap32[0x10000];
uint16_t  systemColorMap16[0x10000];
uint16_t  systemGbPalette[24];

int  emulating;
int  RGB_LOW_BITS_MASK;

@interface GBAEmulatorBridge ()

@property (nonatomic, copy, nullable, readwrite) NSURL *gameURL;

@property (assign, nonatomic, getter=isFrameReady) BOOL frameReady;
@property (assign, nonatomic, getter=isSavingDisabled) BOOL isSavingDisabled;

@property (nonatomic) uint32_t activatedInputs;

@property (strong, nonatomic, readonly) CMMotionManager *motionManager;

@end

static std::unique_ptr<Rom> bgmRom;
static std::unique_ptr<BgmMultiMirror> bgmMirror;
static std::vector<MPlayTableCandidate> bgmCandidates;

// VBA-M RAM adapter for the BGM mirror.
class VBAMGbaBus final : public GbaBus {
public:
    uint8_t r8(uint32_t address) override {
        if (address >= 0x02000000 && address < 0x02040000 && workRAM)
            return workRAM[address - 0x02000000];
        if (address >= 0x03000000 && address < 0x03008000 && internalRAM)
            return internalRAM[address - 0x03000000];
        return 0;
    }

    void w8(uint32_t address, uint8_t value) override {
        if (address >= 0x02000000 && address < 0x02040000 && workRAM) {
            workRAM[address - 0x02000000] = value;
            return;
        }
        if (address >= 0x03000000 && address < 0x03008000 && internalRAM)
            internalRAM[address - 0x03000000] = value;
    }
};

static VBAMGbaBus bgmBus;

static dispatch_queue_t bgmRenderQueue;
static dispatch_source_t bgmRenderTimer;
static uint32_t bgmRenderFrameRemainder = 0;

static void StopBGMRenderTimer(void)
{
    if (bgmRenderTimer) {
        dispatch_source_cancel(bgmRenderTimer);
        bgmRenderTimer = nil;
        if (bgmRenderQueue) {
            dispatch_sync(bgmRenderQueue, ^{});
        }
    }
    bgmRenderFrameRemainder = 0;
}

static void StartBGMRenderTimer(void)
{
    StopBGMRenderTimer();

    if (!bgmRenderQueue) {
        bgmRenderQueue = dispatch_queue_create("com.ajun22333.Delta.GBA.BGMRender", DISPATCH_QUEUE_SERIAL);
    }

    bgmRenderTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, bgmRenderQueue);
    if (!bgmRenderTimer) return;

    // Generate 32768 Hz stereo PCM independently of the emulator frame rate.
    // 10 ms produces alternating 327/328-frame blocks, keeping BGM at 1x
    // even while the game emulation is fast-forwarding.
    dispatch_source_set_timer(bgmRenderTimer,
                             dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC),
                             10 * NSEC_PER_MSEC,
                             1 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(bgmRenderTimer, ^{
        if (!emulating || !bgmMirror || !bgmMirror->located()) return;

        bgmRenderFrameRemainder += 32768u * 10u;
        const int frames = (int)(bgmRenderFrameRemainder / 1000u);
        bgmRenderFrameRemainder %= 1000u;
        if (frames <= 0) return;

        GBAEmulatorBridge *bridge = GBAEmulatorBridge.sharedBridge;
        id output = bridge.audioRenderer.bgmAudioBuffer;
        if (!output || ![[output valueForKey:@"isEnabled"] boolValue]) return;

        const size_t sampleCount = (size_t)frames * 2u;
        const size_t byteCount = sampleCount * sizeof(int16_t);
        if ([[output valueForKey:@"availableBytesForWriting"] integerValue] < (NSInteger)byteCount) return;

        std::vector<float> rendered(sampleCount);
        bgmMirror->render(rendered.data(), frames);

        std::vector<int16_t> pcm(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i) {
            const float value = std::max(-1.0f, std::min(1.0f, rendered[i]));
            pcm[i] = (int16_t)std::lrintf(value * 32767.0f);
        }

        [output writeBuffer:(uint8_t *)pcm.data() size:(NSInteger)byteCount];
    });

    dispatch_resume(bgmRenderTimer);
}

@implementation GBAEmulatorBridge

- (void)mixBGMIntoBuffer:(uint16_t *)buffer length:(int)length
{
    if (!buffer || length <= 0 || !bgmMirror) return;

    const int frames = length / (sizeof(int16_t) * 2);
    if (frames <= 0) return;

    std::vector<float> mix((size_t)frames * 2);
    bgmMirror->render(mix.data(), frames);

    int16_t *dst = reinterpret_cast<int16_t *>(buffer);
    const size_t count = (size_t)frames * 2;

    for (size_t i = 0; i < count; ++i) {
        const int32_t sample = (int32_t)dst[i] +
            (int32_t)std::lrintf(mix[i] * 32767.0f);
        dst[i] = (int16_t)std::clamp(sample, -32768, 32767);
    }
}
@synthesize audioRenderer = _audioRenderer;
@synthesize videoRenderer = _videoRenderer;
@synthesize saveUpdateHandler = _saveUpdateHandler;

+ (instancetype)sharedBridge
{
    static GBAEmulatorBridge *_emulatorBridge = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        _emulatorBridge = [[self alloc] init];
    });
    
    return _emulatorBridge;
}

- (instancetype)init
{
    self = [super init];
    if (self)
    {
        _motionManager = [[CMMotionManager alloc] init];
    }
    
    return self;
}

#pragma mark - Emulation -

- (void)startWithGameURL:(NSURL *)gameURL
{
    StopBGMRenderTimer();
    self.gameURL = gameURL;
    
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfURL:gameURL options:0 error:&error];
    if (data == nil)
    {
        NSLog(@"Failed to load GBA game at %@. %@", gameURL, error);
        return;
    }
    
    int romSize = (int)data.length;
    void *romBytes = (void *)calloc(ROM_SIZE, 1);
    memcpy(romBytes, data.bytes, romSize);
    
    if (self.patchesDirectoryURL != nil)
    {
        // Check if patches directory contains patch with same filename (with .ips extension) as passed in gameURL.
        NSString *ipsFilename = [[gameURL URLByDeletingPathExtension] URLByAppendingPathExtension:@"ips"].lastPathComponent;
        NSString *upsFilename = [[gameURL URLByDeletingPathExtension] URLByAppendingPathExtension:@"ups"].lastPathComponent;
        NSString *ppfFilename = [[gameURL URLByDeletingPathExtension] URLByAppendingPathExtension:@"ppf"].lastPathComponent;
        
        NSURL *ipsPatchURL = [self.patchesDirectoryURL URLByAppendingPathComponent:ipsFilename];
        NSURL *upsPatchURL = [self.patchesDirectoryURL URLByAppendingPathComponent:upsFilename];
        NSURL *ppfPatchURL = [self.patchesDirectoryURL URLByAppendingPathComponent:ppfFilename];
        
        if ([[NSFileManager defaultManager] fileExistsAtPath:ipsPatchURL.path])
        {
            // .ips patch
            applyPatch(ipsPatchURL.fileSystemRepresentation, (uint8_t **)&romBytes, &romSize);
        }
        else if ([[NSFileManager defaultManager] fileExistsAtPath:upsPatchURL.path])
        {
            // .ups patch
            applyPatch(upsPatchURL.fileSystemRepresentation, (uint8_t **)&romBytes, &romSize);
        }
        else if ([[NSFileManager defaultManager] fileExistsAtPath:ppfPatchURL.path])
        {
            // .ppf patch
            applyPatch(ppfPatchURL.fileSystemRepresentation, (uint8_t **)&romBytes, &romSize);
        }
        
        // Enable VERBOSE_ILLEGAL_WRITE logging so we can detect enabling/disabling saving.
        systemVerbose = VERBOSE_ILLEGAL_WRITE;
    }
    else
    {
        systemVerbose = 0;
    }
        
    bgmMirror.reset();
    bgmCandidates.clear();
    bgmRom.reset();
    bgmRom = std::make_unique<Rom>();
    if (!bgmRom->loadFromMemory((const uint8_t *)romBytes, (size_t)romSize)) {
        NSLog(@"BGM ROM copy failed");
        bgmRom.reset();
    }
    if (!CPULoadRomData((const char *)romBytes, romSize))
    {
        free(romBytes);
        return;
    }
    
    free(romBytes);
        
    utilUpdateSystemColorMaps(NO);
    utilGBAFindSave(romSize);
    
    // Update per-game settings after utilGBAFindSave determines defaults.
    [self updateGameSettings];
    
    soundInit();
    soundSetSampleRate(32768); // 44100 chirps
    
    soundReset();
    
    CPUInit(0, false);
    
    GBASystem.emuReset();

    if (bgmRom) {
        bgmCandidates = findMPlayTables(*bgmRom);
        bgmMirror = std::make_unique<BgmMultiMirror>(bgmRom.get());
        EngineConfig bgmConfig;
        bgmConfig.outRate = 32768;
        bgmMirror->configure(bgmConfig);
        NSLog(@"BGM: found %lu MPlay table candidates",
              (unsigned long)bgmCandidates.size());
    }
    
    [self.audioRenderer.bgmAudioBuffer reset];
    emulating = 1;
    StartBGMRenderTimer();
}

- (void)stop
{
    emulating = 0;
    StopBGMRenderTimer();
    [self.audioRenderer.bgmAudioBuffer reset];
    if (bgmMirror) {
        bgmMirror->releaseMute(bgmBus);
        bgmMirror->reset();
    }
    GBASystem.emuCleanUp();
    soundShutdown();
    
    emulating = 0;
    
    [self deactivateGyroscope];
}

- (void)pause
{
    emulating = 0;
    [self.audioRenderer.bgmAudioBuffer reset];
    
    [self deactivateGyroscope];
}

- (void)resume
{
    [self.audioRenderer.bgmAudioBuffer reset];
    emulating = 1;
}

- (void)runFrameAndProcessVideo:(BOOL)processVideo
{
    self.frameReady = NO;
    
    while (![self isFrameReady])
    {
        GBASystem.emuMain(GBASystem.emuCount);
    }

    if (bgmMirror && !bgmMirror->located()) {
        bgmMirror->locate(bgmBus, bgmCandidates);
    }
    if (bgmMirror && bgmMirror->located()) {
        bgmMirror->onFrame(bgmBus, true);
    }
}

- (nullable NSData *)readMemoryAtAddress:(NSInteger)address size:(NSInteger)size
{
    if (workRAM == NULL)
    {
        return nil;
    }
    
    void *bytes = NULL;
    if (address >= 0x8000)
    {
        bytes = (void *)(workRAM + address - 0x8000);
    }
    else
    {
        bytes = (void *)(internalRAM + address);
    }
    
    NSData *data = [NSData dataWithBytesNoCopy:bytes length:size freeWhenDone:NO];
    return data;
}

#pragma mark - Settings -

- (void)updateGameSettings
{
    NSString *gameID = [NSString stringWithFormat:@"%c%c%c%c", rom[0xac], rom[0xad], rom[0xae], rom[0xaf]];
    
    NSLog(@"VBA-M: GameID in ROM is: %@", gameID);
    
    // Set defaults
    // Use underscores to prevent shadowing of global variables
    BOOL _enableRTC       = NO;
    BOOL _enableMirroring = NO;
    BOOL _useBIOS         = NO;
    int  _cpuSaveType     = 0;
    int  _flashSize       = 0x10000;
    
    // Read in vba-over.ini and break it into an array of strings
    NSString *iniPath = [GBAEmulatorBridge.gbaResources pathForResource:@"vba-over" ofType:@"ini"];
    NSString *iniString = [NSString stringWithContentsOfFile:iniPath encoding:NSUTF8StringEncoding error:NULL];
    NSArray *settings = [iniString componentsSeparatedByString:@"\n"];
    
    BOOL matchFound = NO;
    NSMutableDictionary *overridesFound = [[NSMutableDictionary alloc] init];
    NSString *temp;
    
    // Check if vba-over.ini has per-game settings for our gameID
    for (NSString *s in settings)
    {
        temp = nil;
        
        if ([s hasPrefix:@"["])
        {
            NSScanner *scanner = [NSScanner scannerWithString:s];
            [scanner scanString:@"[" intoString:nil];
            [scanner scanUpToString:@"]" intoString:&temp];
            
            if ([temp caseInsensitiveCompare:gameID] == NSOrderedSame)
            {
                matchFound = YES;
            }
            
            continue;
        }
        
        else if (matchFound && [s hasPrefix:@"saveType="])
        {
            NSScanner *scanner = [NSScanner scannerWithString:s];
            [scanner scanString:@"saveType=" intoString:nil];
            [scanner scanUpToString:@"\n" intoString:&temp];
            _cpuSaveType = [temp intValue];
            [overridesFound setObject:temp forKey:@"CPU saveType"];
            
            continue;
        }
        
        else if (matchFound && [s hasPrefix:@"rtcEnabled="])
        {
            NSScanner *scanner = [NSScanner scannerWithString:s];
            [scanner scanString:@"rtcEnabled=" intoString:nil];
            [scanner scanUpToString:@"\n" intoString:&temp];
            _enableRTC = [temp boolValue];
            [overridesFound setObject:temp forKey:@"rtcEnabled"];
            
            continue;
        }
        
        else if (matchFound && [s hasPrefix:@"flashSize="])
        {
            NSScanner *scanner = [NSScanner scannerWithString:s];
            [scanner scanString:@"flashSize=" intoString:nil];
            [scanner scanUpToString:@"\n" intoString:&temp];
            _flashSize = [temp intValue];
            [overridesFound setObject:temp forKey:@"flashSize"];
            
            continue;
        }
        
        else if (matchFound && [s hasPrefix:@"mirroringEnabled="])
        {
            NSScanner *scanner = [NSScanner scannerWithString:s];
            [scanner scanString:@"mirroringEnabled=" intoString:nil];
            [scanner scanUpToString:@"\n" intoString:&temp];
            _enableMirroring = [temp boolValue];
            [overridesFound setObject:temp forKey:@"mirroringEnabled"];
            
            continue;
        }
        
        else if (matchFound && [s hasPrefix:@"useBios="])
        {
            NSScanner *scanner = [NSScanner scannerWithString:s];
            [scanner scanString:@"useBios=" intoString:nil];
            [scanner scanUpToString:@"\n" intoString:&temp];
            _useBIOS = [temp boolValue];
            [overridesFound setObject:temp forKey:@"useBios"];
            
            continue;
        }
        
        else if (matchFound)
            break;
    }
    
    // Some hacked games use the RealTimeClock even when the game they're based off of doesn't (ex: Pokemon Liquid Crystal), so we always have it enabled.
    rtcEnable(true);
    
    if (!matchFound)
    {
        // Only update remaining settings if we found a match.
        return;
    }
    
    NSLog(@"VBA: overrides found: %@", overridesFound);
    
    // Apply settings
    mirroringEnable = _enableMirroring;
    doMirroring(mirroringEnable);
    cpuSaveType = _cpuSaveType;
    
    // Delta doesn't use BIOS files.
    // useBios = _useBIOS;
    
    if (_flashSize == 0x10000 || _flashSize == 0x20000)
    {
        flashSetSize(_flashSize);
    }
    
}

#pragma mark - Inputs -

- (void)activateInput:(NSInteger)gameInput value:(double)value playerIndex:(NSInteger)playerIndex
{
    self.activatedInputs |= (uint32_t)gameInput;
}

- (void)deactivateInput:(NSInteger)gameInput playerIndex:(NSInteger)playerIndex
{
    self.activatedInputs &= ~((uint32_t)gameInput);
}

- (void)resetInputs
{
    self.activatedInputs = 0;
}

#pragma mark - Game Saves -

- (void)saveGameSaveToURL:(NSURL *)URL
{
    if ([self isSavingDisabled])
    {
        return;
    }
    
    GBASystem.emuWriteBattery(URL.fileSystemRepresentation);
}

- (void)loadGameSaveFromURL:(NSURL *)URL
{
    GBASystem.emuReadBattery(URL.fileSystemRepresentation);
}

#pragma mark - Save States -

- (void)saveSaveStateToURL:(NSURL *)URL
{
    GBASystem.emuWriteState(URL.fileSystemRepresentation);
}

- (void)loadSaveStateFromURL:(NSURL *)URL
{
    GBASystem.emuReadState(URL.fileSystemRepresentation);
}

#pragma mark - Cheats -

- (BOOL)addCheatCode:(NSString *)cheatCode type:(NSString *)type
{
    NSArray<NSString *> *codes = [cheatCode componentsSeparatedByString:@"\n"];
    for (NSString *code in codes)
    {
        NSMutableCharacterSet *legalCharactersSet = [NSMutableCharacterSet hexadecimalCharacterSet];
        [legalCharactersSet addCharactersInString:@" "];
        
        if ([code rangeOfCharacterFromSet:[legalCharactersSet invertedSet]].location != NSNotFound)
        {
            return NO;
        }
        
        if ([type isEqualToString:CheatTypeActionReplay] || [type isEqualToString:CheatTypeGameShark])
        {
            NSString *sanitizedCode = [code stringByReplacingOccurrencesOfString:@" " withString:@""];
            
            if (sanitizedCode.length != 16)
            {
                return NO;
            }
            
            cheatsAddGSACode([sanitizedCode UTF8String], "code", true);
        }
        else if ([type isEqualToString:CheatTypeCodeBreaker])
        {
            if (code.length != 13)
            {
                return NO;
            }
            
            cheatsAddCBACode([code UTF8String], "code");
        }
    }
    
    return YES;
}

- (void)resetCheats
{
    cheatsDeleteAll(true);
}

- (void)updateCheats
{
    
}

#pragma mark - Gyroscope -

- (void)activateGyroscope
{
    if ([self.motionManager isGyroActive] || ![self.motionManager isGyroAvailable])
    {
        return;
    }
    
    [self.motionManager startGyroUpdates];
    
    [[NSNotificationCenter defaultCenter] postNotificationName:GBADidActivateGyroNotification object:self];
}

- (void)deactivateGyroscope
{
    if (![self.motionManager isGyroActive])
    {
        return;
    }
    
    [self.motionManager stopGyroUpdates];
    
    [[NSNotificationCenter defaultCenter] postNotificationName:GBADidDeactivateGyroNotification object:self];
}

#pragma mark - Getters/Setters -

- (NSTimeInterval)frameDuration
{
    return (1.0 / 60.0);
}

@end

#pragma mark - VBA-M -

void systemMessage(int _iId, const char * _csFormat, ...)
{
    NSLog(@"VBA-M: %s", _csFormat);
}

void systemDrawScreen()
{
    // Get rid of the first line and the last row
    for (int y = 0; y < 160; y++)
    {
        memcpy([GBAEmulatorBridge sharedBridge].videoRenderer.videoBuffer + y * 240 * 4, pix + (y + 1) * (240 + 1) * 4, 240 * 4);
    }
    
    [[GBAEmulatorBridge sharedBridge].videoRenderer processFrame];
    [[GBAEmulatorBridge sharedBridge] setFrameReady:YES];
}

bool systemReadJoypads()
{
    return true;
}

uint32_t systemReadJoypad(int joy)
{
    return [GBAEmulatorBridge sharedBridge].activatedInputs;
}

void systemShowSpeed(int _iSpeed)
{
    
}

void system10Frames(int _iRate)
{
    if (systemSaveUpdateCounter > 0)
    {
        systemSaveUpdateCounter--;
        
        if (systemSaveUpdateCounter <= SYSTEM_SAVE_NOT_UPDATED)
        {
            if (GBAEmulatorBridge.sharedBridge.saveUpdateHandler)
            {
                GBAEmulatorBridge.sharedBridge.saveUpdateHandler();
            }
            
            systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;
        }
    }
}

void systemFrame()
{
    
}

void systemSetTitle(const char * _csTitle)
{

}

void systemScreenCapture(int _iNum)
{

}

uint32_t systemGetClock()
{
    timeval time;
    
    gettimeofday(&time, NULL);
    
    double milliseconds = (time.tv_sec * 1000.0) + (time.tv_usec / 1000.0);
    return milliseconds;
}

SoundDriver *systemSoundInit()
{
    soundShutdown();
    
    auto driver = new GBASoundDriver;
    return driver;
}

void systemUpdateMotionSensor()
{
}

uint8_t systemGetSensorDarkness()
{
    return 0;
}

int systemGetSensorX()
{
    return 0;
}

int systemGetSensorY()
{
    return 0;
}

int systemGetSensorZ()
{
    if (![GBAEmulatorBridge.sharedBridge.motionManager isGyroActive])
    {
        [GBAEmulatorBridge.sharedBridge activateGyroscope];
    }
    
    CMGyroData *gyroData = GBAEmulatorBridge.sharedBridge.motionManager.gyroData;
    
    int sensorZ = -gyroData.rotationRate.z * 25;
    return sensorZ;
}

void systemCartridgeRumble(bool)
{
}

void systemGbPrint(uint8_t * _puiData,
                   int  _iLen,
                   int  _iPages,
                   int  _iFeed,
                   int  _iPalette,
                   int  _iContrast)
{
}

void systemScreenMessage(const char * _csMsg)
{
}

bool systemCanChangeSoundQuality()
{
    return true;
}

bool systemPauseOnFrame()
{
    return false;
}

void systemGbBorderOn()
{
}

void systemOnSoundShutdown()
{
}

void systemOnWriteDataToSoundBuffer(const uint16_t * finalWave, int length)
{
}

void debuggerMain()
{
}

void debuggerSignal(int, int)
{
}

void log(const char *defaultMsg, ...)
{
    va_list valist;
    va_start(valist, defaultMsg);
    
    // Format the message to a string
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), defaultMsg, valist);
    
    // Strip trailing newline if present
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }
    
    NSLog(@"[GBALog] %s", buffer);
    
    va_end(valist);
    
    static const unsigned char deltaMagicNumber[] = "DELTA";
    static const unsigned char altstoreMagicNumber[] = "ALTSTORE";
    static const int magicNumberMaxLength = 8;
    
    // Rolling buffer of the most recent illegal-write byte values.
    static char recentBytes[magicNumberMaxLength] = {0};
    
    unsigned int value = 0;
    if (sscanf(buffer, "Illegal byte write: %x", &value) != 1)
    {
        return;
    }
    
    // Shift buffer down and append latest character to end.
    memmove(recentBytes, recentBytes + 1, magicNumberMaxLength - 1);
    recentBytes[magicNumberMaxLength - 1] = value;
    
    if (memcmp(recentBytes + 3, deltaMagicNumber, 5) == 0)
    {
        // DELTA detected, which means we want to disable saving.
        
        GBAEmulatorBridge.sharedBridge.isSavingDisabled = true;
        [[NSNotificationCenter defaultCenter] postNotificationName:GBADidDisableSavingNotification object:GBAEmulatorBridge.sharedBridge];
    }
    else if (memcmp(recentBytes, altstoreMagicNumber, magicNumberMaxLength) == 0)
    {
        // ALTSTORE detected, which means we want to enable saving.
        
        // Load previous save file back into memory, in case user saved while saving was disabled.
        NSURL *saveURL = [[GBAEmulatorBridge.sharedBridge.gameURL URLByDeletingPathExtension] URLByAppendingPathExtension:@"sav"];
        [GBAEmulatorBridge.sharedBridge loadGameSaveFromURL:saveURL];
        
        GBAEmulatorBridge.sharedBridge.isSavingDisabled = false;
        [[NSNotificationCenter defaultCenter] postNotificationName:GBADidEnableSavingNotification object:GBAEmulatorBridge.sharedBridge];
    }
    else
    {
        return;
    }
    
    // Reset recentBytes buffer.
    memset(recentBytes, 0, magicNumberMaxLength);
}
