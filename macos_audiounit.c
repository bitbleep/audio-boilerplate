// This setup uses the audio unit api of macOS to render audio.
// AFAIK this is one of the most efficient ways of passing samples
// calculated elsewhere to the audio hardware.
//
// The output format is 32-bit floating point and both the
// sample rate and number of channels is set using the AudioContext.
//
// build using:
// debug: clang -o macos_audiounit macos_audiounit.c -framework AudioUnit -g --std=c11
// optimizations: clang -o macos_audiounit macos_audiounit.c -framework AudioUnit -Os --std=c11

#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <AudioUnit/AudioUnit.h>

typedef struct
{
    Float64 generator;
    Float64 samplerate;
    int channels;
    AudioUnit audioUnit;
}
AudioContext;

// the callback function is called by the os when is needs a new chunk
// of samples for playback.
// ie. this is the place to implement the dsp code.
OSStatus callback(
    void *inRefCon,
    AudioUnitRenderActionFlags *ioActionFlags,
    const AudioTimeStamp *inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList *ioData)
{
    AudioContext *context = (AudioContext*)inRefCon;

    // note: implement audio rendering here
    //       this one generates a 440Hz sine
    const Float64 frequency = 440.0;
    Float64 generator = context->generator;
    const Float64 samplePeriod = context->samplerate / frequency;
    const int channels = context->channels;

    Float32* data = (Float32*)ioData->mBuffers[0].mData;
    for (int i=0; i<inNumberFrames; ++i) {
        generator += 1.0;
        if (generator > samplePeriod) {
            generator -= samplePeriod;
        }

        Float32 sample = sinf(2 * M_PI * generator / samplePeriod);
        for (int j=0; j<channels; ++j) {
            data[channels * i + j] = sample;
        }
    }

    context->generator = generator;

    return noErr;
}

// initializes the audio contexts and starts
// the rendering (on a separate thread)
int startAudio(AudioContext *context)
{
    OSErr err;

    // find the current default output
    // as configured by the user in system preferences
    AudioComponentDescription defaultOutputDescription;
    defaultOutputDescription.componentType = kAudioUnitType_Output;
    defaultOutputDescription.componentSubType = kAudioUnitSubType_DefaultOutput;
    defaultOutputDescription.componentManufacturer = kAudioUnitManufacturer_Apple;
    defaultOutputDescription.componentFlags = 0;
    defaultOutputDescription.componentFlagsMask = 0;
    AudioComponent defaultOutput = AudioComponentFindNext(NULL, &defaultOutputDescription);
    if (!defaultOutput) {
        return 1;
    }

    // create an audio unit instance using
    // the default output
    if (AudioComponentInstanceNew(defaultOutput, &context->audioUnit)) {
        return 2;
    }

    // set the input callback for the audio unit
    // (ie. the function that will actually render the audio)
    AURenderCallbackStruct input;
    input.inputProc = callback;
    input.inputProcRefCon = context;
    if (AudioUnitSetProperty(context->audioUnit,
                             kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input,
                             0,
                             &input,
                             sizeof(input))) {
        return 3;
    }

    // set the stream format to 32-bit, linear PCM, floating point
    // the number of channels is determined by `channels` on the context
    const int fourBytesPerFloat = 4;
    const int eightBitsPerByte = 8;
    AudioStreamBasicDescription streamFormat;
    streamFormat.mSampleRate = context->samplerate;
    streamFormat.mFormatID = kAudioFormatLinearPCM;
    streamFormat.mFormatFlags = kAudioFormatFlagIsFloat;
    streamFormat.mBytesPerPacket = context->channels * fourBytesPerFloat;
    streamFormat.mFramesPerPacket = 1;
    streamFormat.mBytesPerFrame = context->channels * fourBytesPerFloat;
    streamFormat.mChannelsPerFrame = context->channels;
    streamFormat.mBitsPerChannel = fourBytesPerFloat * eightBitsPerByte;
    if (AudioUnitSetProperty(context->audioUnit,
                             kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input,
                             0,
                             &streamFormat,
                             sizeof(AudioStreamBasicDescription))) {
        return 4;
    }

    // initialize the audio unit
    // after this call the audio unit will be
    // ready to render audio
    if (AudioUnitInitialize(context->audioUnit)) {
        return 5;
    }

    // start the audio unit
    // this uses a separate thread provided by the os
    if (AudioOutputUnitStart(context->audioUnit)) {
        return 6;
    }

    return 0;
}

// stops the audio unit and cleans up the context
void stopAudio(AudioContext *context)
{
    if (context->audioUnit) {
        AudioOutputUnitStop(context->audioUnit);
        context->audioUnit = NULL;
    }
}

// global variable that indicates that the application
// should stop rendering and exit
volatile bool shouldStop = false;

// signal handler that sets the shouldStop variable when
// initiated by the user
void handleStopSignal() {
    shouldStop = true;
}

int main(int argc, char *argv[])
{
    AudioContext context;
    context.generator = 0;
    context.samplerate = 44100;
    context.channels = 2;

    if (startAudio(&context)) {
        printf("Failed to start audio playback\n");
        return EXIT_FAILURE;
    }

    // hook up signal to do a graceful
    // stop when user presses ctrl-C
    signal(SIGINT, handleStopSignal);

    // since the audio unit is rendering
    // using another thread we'll keep
    // the main thread busy
    while (!shouldStop) {
        usleep(500000);
    }

    stopAudio(&context);

    return EXIT_SUCCESS;
}
