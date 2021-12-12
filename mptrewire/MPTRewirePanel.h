#pragma once
#include <chrono>
#include <thread>
#include <string>
#include <stdint.h>

#define PIPE_EVENTS 0
#define PIPE_RT     1  // realtime audio thread


enum class MPTPanelStatus
{
	Ok = 0,
	MixerNotRunning = 1,
	UnableToRegisterDevice = 2,
	UnknownDeviceProblem = 3,
	ReWireProblem = 4,
	DeviceNotInstalled = 5,
	FirstTime = 6,
};

// These get sent to the device as commands to the mixer
enum class MPTPanelEvent
{
	Play = 0,
	Stop = 1,
	ChangeBPM = 2,
	Reposition = 3
};

typedef struct
{
	uint8_t type;
	uint32_t tempo;
} MPTPlayRequest;

typedef struct
{
	uint8_t type;
	uint32_t position15360PPQ;
	//LARGE_INTEGER timeIssued; //uint64_t timeIssued;
	//double bpm;
} MPTRepositionRequest;

typedef struct
{
	int32_t sampleRate;
	int32_t maxBufferSize;
	uint32_t framesToRender;
} MPTAudioRequest;

typedef struct
{
	int32_t sampleRate;
	int32_t maxBufferSize;
} MPTAudioInfoRequest;

typedef struct
{
	uint32_t servedChannelsBitfield[4];  // 128 bits, one stereo channel for each bit
} MPTAudioResponseHeader;                // sent once before a bunch of MPTAudioResponse packets are sent

typedef struct
{
	uint16_t channelIndex;  // @TODO: optimize this away for performance reasons, then this struct becomes:
							//        typedef int32_t* MPTAudioResponse;
	// <interleaved audio channel (2 * fFramesToRender * sizeof(int))>
} MPTAudioResponse;

typedef bool (*MPTRenderCallback)(unsigned int framesToRender, void *userData);
typedef void (*MPTAudioInfoCallback)(unsigned int sampleRate, unsigned int maxBufferSize, void *userData);
typedef void (*MPTMixerQuitCallback)(void *userData);
typedef void *TRWPPortHandle;
typedef void *HANDLE;


class MPTRewirePanel
{
private:
	std::thread m_Thread;
	bool m_Running = false;
	bool m_MixerQuit = false;
	const char *m_DeviceName = "OpenMPT";

	void *m_CallbackUserData = nullptr;
	MPTRenderCallback m_RenderCallback = nullptr;
	MPTAudioInfoCallback m_AudioInfoCallback = nullptr;
	MPTMixerQuitCallback m_MixerQuitCallback = nullptr;

	MPTAudioResponse *m_AudioResponseBuffer = nullptr;
	TRWPPortHandle m_PanelPortHandle = nullptr;
	uint8_t m_Message[8192];
	uint32_t m_ServedChannelsBitfield[4];  // 128 bits

	// Signals to device whenever an audio buffer was sent by us.
	HANDLE m_EventToDevice;

	// Lets us know when an audio buffer sent by us was received and processed by the device.
	HANDLE m_EventFromDevice;


	void deallocateBuffers();
	void reallocateBuffers(int32_t maxBufferSize);
	void checkComConnection();
	void handleAudioInfoChange(int sampleRate, int maxBufferSize);
	void pollAudioRequests();
	bool waitForEventFromDevice(const int milliseconds = 100);
	void swallowRemainingMessages();
	void generateAudioAndUploadToDevice(MPTAudioRequest incomingRequest);
	bool sendAudioResponseHeaderToDevice();



public:
	bool m_Errored = false;
	int m_SampleRate = 0;
	int m_MaxBufferSize = 0;
	int **m_AudioBuffers = nullptr;



	MPTRewirePanel();
	~MPTRewirePanel();
	MPTPanelStatus open(
		MPTRenderCallback renderCallback,
		MPTAudioInfoCallback audioInfoCallback,
		MPTMixerQuitCallback mixerQuitCallback,
		void *callbackUserData
	);
	bool close();
	void threadProc();
	bool isRunning() { return m_Running; }
	void stop() { m_Running = false; }
	inline void markChannelAsRendered(int index) {
		m_ServedChannelsBitfield[index >> 5] |= 1 << (index & 0x1f);
	}

	void signalPlay(double bpm);
	void signalStop();
	void signalBPMChange(double bpm);
	void signalReposition(double bpm, int nFrames);
	// void signalLoop();

};
