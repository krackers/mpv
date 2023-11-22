//
//  TPPreciseTimer.m
//  Loopy
//
//  Created by Michael Tyson on 06/09/2011.
//  Copyright 2011 A Tasty Pixel. All rights reserved.
//

#import "TPPreciseTimer.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#import <pthread.h>

static NSString *kTimeKey = @"time";
static NSString *kBlockKey = @"block";

extern int cocoa_set_realtime(struct mp_log *log, double periodFraction);

@interface TPPreciseTimer () {
    double _spinLockTime;
    int _spinLockSleepRatio;
    bool _precision;
    struct mp_log *_log;
}
@end

@implementation TPPreciseTimer

- (id) initWithSpinLock:(double)spinLock spinLockSleepRatio:(int)sleep highPrecision:(BOOL)pre log:(struct mp_log *)log {
    if ( !(self = [super init]) ) return nil;
    
    _spinLockTime = spinLock;
    _spinLockSleepRatio = sleep;
    _precision = pre;
    _log = log;

    self->isRunning = YES;

    struct mach_timebase_info timebase;
    mach_timebase_info(&timebase);
    timebase_ratio = ((double)timebase.numer / (double)timebase.denom) * 1.0e-9;
    
    events = [[NSMutableArray alloc] init];
    condition = [[NSCondition alloc] init];
    
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // This is unneeded since real-time already puts it into a fixed priority bucket.
    // struct sched_param param;
    // param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    // pthread_attr_setschedparam(&attr, &param);
    // pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_create(&thread, &attr, thread_entry, (__bridge void*)self);

    return self;
}

-(int) terminate {
    [condition lock];
    self->isRunning = NO;
    [condition signal];
    [condition unlock];
    pthread_kill(thread, SIGALRM);
    return pthread_join(thread, NULL);
}


- (void)scheduleBlock:(void (^)(void))block atTime:(UInt64)time {
    [self addSchedule:[NSDictionary dictionaryWithObjectsAndKeys:
                       [block copy], kBlockKey,
                       [NSNumber numberWithUnsignedLongLong: time], kTimeKey,
                       nil]];
}

- (void)addSchedule:(NSDictionary*)schedule {
    [condition lock];
    if (!isRunning) {
        [condition unlock];
        return;
    }
    [events addObject:schedule];
    [events sortUsingDescriptors:[NSArray arrayWithObject:[NSSortDescriptor sortDescriptorWithKey:kTimeKey ascending:YES]]];
    BOOL mustSignal = [events count] > 1 && [events objectAtIndex:0] == schedule;
    [condition signal];
    [condition unlock];
    if ( mustSignal ) {
        pthread_kill(thread, SIGALRM); // Interrupt thread if it's performing a mach_wait_until and new schedule is earlier
    }
}

static void *thread_entry(void* argument) {
    [(__bridge TPPreciseTimer*)argument thread];
    return NULL;
}

static void thread_signal(int signal) {
    // Ignore
}

- (void)thread {
    pthread_setname_np("TPPreciseTimer");
    if (_precision) {
        cocoa_set_realtime(_log, 0.1);
    }

    signal(SIGALRM, thread_signal);
    [condition lock];

    while ( isRunning ) {
        while ( [events count] == 0 && isRunning ) {
            [condition wait];
        }
        if (!isRunning) {
            break;
        }
        NSDictionary *nextEvent = [events objectAtIndex:0];
        NSTimeInterval time = [[nextEvent objectForKey:kTimeKey] unsignedLongLongValue] * timebase_ratio;
        
        [condition unlock];
        
        mach_wait_until((uint64_t)((time - _spinLockTime) / timebase_ratio));
        
        if ( (double)(mach_absolute_time() * timebase_ratio) >= time-_spinLockTime ) {
            
            // Spin lock until it's time
            uint64_t end = time / timebase_ratio;
            //printf("---\n");
            while ( _spinLockTime > 0 && mach_absolute_time() < end ) {
                if (_spinLockSleepRatio > 0)
                    [NSThread sleepForTimeInterval:_spinLockTime/_spinLockSleepRatio];
            }
            
            void (^block)(void) = [nextEvent objectForKey:kBlockKey];
            if ( block ) {
                block();
            }
            
            [condition lock];
            [events removeObject:nextEvent];
        } else {
            [condition lock];
        }
    }

    [condition unlock];
}

@end