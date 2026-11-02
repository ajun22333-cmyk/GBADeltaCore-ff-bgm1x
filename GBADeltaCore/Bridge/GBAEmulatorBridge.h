//
//  GBAEmulatorBridge.h
//  GBADeltaCore
//
//  Created by Riley Testut on 6/3/16.
//  Copyright © 2016 Riley Testut. All rights reserved.
//

#import <Foundation/Foundation.h>
#include <stdint.h>

@protocol DLTAEmulatorBridging;

NS_ASSUME_NONNULL_BEGIN

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything" // Silence "Cannot find protocol definition" warning due to forward declaration.
@interface GBAEmulatorBridge : NSObject <DLTAEmulatorBridging>
#pragma clang diagnostic pop

@property (class, nonatomic, readonly) GBAEmulatorBridge *sharedBridge;

// When non-nil, Delta looks here for patches with same name as ROM.
@property (nonatomic, copy, nullable) NSURL *patchesDirectoryURL;

- (void)mixBGMIntoBuffer:(uint16_t *)buffer length:(int)length;

@end

NS_ASSUME_NONNULL_END
