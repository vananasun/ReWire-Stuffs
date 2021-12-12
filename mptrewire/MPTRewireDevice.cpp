#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <vector>
#include <ReWireDeviceAPI.h>
#include <RWDEFAPI.h>
#include "MPTRewirePanel.h"
#include "MPTRewireDebugUtils.h"


using namespace ReWire;


#ifndef MIXING_SCALEF
#define MIXING_SCALEF 134217728.0f
#endif
#define PIPE_SIZE_EVENTS 32
#define PIPE_SIZE_RT  (8192 * 2 * sizeof(int32_t)) // realtime audio thread


LARGE_INTEGER g_PerfFrequency;  // for QueryPerformanceCounter
TRWDPortHandle g_DevicePortHandle = 0;
ReWireAudioInfo g_AudioInfo = { 0 };
uint8_t g_IncomingData[PIPE_SIZE_RT];
uint8_t g_IncomingEvent[PIPE_SIZE_EVENTS];
HANDLE g_EventToPanel = NULL;
HANDLE g_EventFromPanel = NULL;
bool g_ReWireOpen = false;
#ifdef DEBUG
int g_LastFramesToRender = 0;
#endif


static void PollAndHandleEvents(ReWireDriveAudioOutputParams *outputParams);




/*******************************************************************************
 *
 * Opening & closing
 *
 ******************************************************************************/

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    return TRUE;
}

void RWDEFGetDeviceNameAndVersion(ReWire_int32_t* codedForReWireVersion, ReWire_char_t* name) {
	*codedForReWireVersion = REWIRE_DEVICE_DLL_API_VERSION;
	strcpy(name, "OpenMPT");
}

void RWDEFGetDeviceInfo(ReWireDeviceInfo* info) {

	RWDEFGetDeviceNameAndVersion(&info->fCodedForReWireVersion, info->fName);
	info->fChannelCount = kReWireAudioChannelCount;

    // Name channels
    for (uint16_t i = 0; i < kReWireAudioChannelCount / 2; i++)
        sprintf(info->fChannelNames[i], "Channel %i", (int)i / 2 + 1);
    for (uint16_t i = kReWireAudioChannelCount / 2; i < kReWireAudioChannelCount - 2; i++)
        sprintf(info->fChannelNames[i], "Plugin %i", (int)i / 2 - 63);
	const char strPreviewChannel[] = "Preview";
	strcpy(info->fChannelNames[126], strPreviewChannel);
	strcpy(info->fChannelNames[127], strPreviewChannel);

    // Mark all channels as stereo
    for (uint16_t i = 0; i < kReWireAudioChannelCount / 2; i++) {
        ReWireSetBitInBitField(info->fStereoPairsBitField, i);
    }

	info->fMaxEventOutputBufferSize = 512;
}

ReWireError RWDEFOpenDevice(const ReWireOpenInfo* openInfo) {

#ifdef DEBUG
    AllocConsole();
	if(!freopen("CONOUT$", "w", stderr)) return kReWireError_UnableToOpenDevice;
#endif

    // Open device (only do this once)
	ReWireError status; 
    if (!g_ReWireOpen) {
		status = RWDOpen();
		if(kReWireError_NoError != status)
			return status;
		g_ReWireOpen = true;
    }

    // Open communication ports
    ReWirePipeInfo pipeInfo[2];
	ReWirePreparePipeInfo(&pipeInfo[PIPE_EVENTS], PIPE_SIZE_EVENTS, PIPE_SIZE_EVENTS);
    ReWirePreparePipeInfo(&pipeInfo[PIPE_RT]    , PIPE_SIZE_RT    , PIPE_SIZE_RT);
	status = RWDComCreate("OMPT", 2, pipeInfo, &g_DevicePortHandle);
	if (kReWireError_NoError != status) {
        DEBUG_PRINT("DEVICE: RWDComCreate returned %i\n", (int)status);
		RWDClose();
        return status;
	}

    // Tell panel the sample rate and audio buffer size across idle thread pipe
    g_AudioInfo = openInfo->fAudioInfo;
    DEBUG_PRINT("DEVICE: RWDEFOpenDevice: fSampleRate = %i, fMaxBufferSize = %i.\n", g_AudioInfo.fSampleRate, g_AudioInfo.fMaxBufferSize);

    // Open / create inter-process events
    g_EventToPanel = CreateEventA(NULL, FALSE, FALSE, "OPENMPT_REWIRE_DEVICE_TO_PANEL");

    QueryPerformanceFrequency(&g_PerfFrequency); // for QueryPerformanceCounter
	return kReWireError_NoError;
}

ReWire_char_t RWDEFIsCloseOK() {
    // @TODO: We might be able to fix the panel hang when stopping while mixer is crashing bug through this!
	return 1;
}

void RWDEFCloseDevice() {
    if (g_DevicePortHandle) RWDComDestroy(g_DevicePortHandle);
    CloseHandle(g_EventToPanel);
}

static void RestartDevice() {
	RWDEFCloseDevice();
	ReWireOpenInfo openInfo;
	ReWirePrepareOpenInfo(&openInfo, g_AudioInfo.fSampleRate, g_AudioInfo.fMaxBufferSize);
	RWDEFOpenDevice(&openInfo);
}




/*******************************************************************************
 *
 * Audio driving
 *
 ******************************************************************************/

static bool SendRenderRequestToPanel(const ReWireDriveAudioInputParams* inputParams, ReWireDriveAudioOutputParams* outputParams) {
    MPTAudioRequest request;
    request.sampleRate = g_AudioInfo.fSampleRate;
    request.maxBufferSize = g_AudioInfo.fMaxBufferSize;
    request.framesToRender = inputParams->fFramesToRender;

    ReWireError status = RWDComSend(g_DevicePortHandle, PIPE_RT, sizeof(request), (ReWire_uint8_t*)&request);
    switch (status) {
        case kReWireError_NoError:
            SetEvent(g_EventToPanel);
            return true; // success

        case kReWireError_PortNotConnected:
            break; // this is fine; the panel wasn't connected
		case kReWireImplError_InvalidParameter:
            break; // panel had quit abruptly, just do nothing
		case kReWireError_BufferFull:
			DEBUG_PRINT("DEVICE: RWDComSend returned kReWireError_BufferFull. Recovering...");
			RestartDevice();
			break;
        default: DEBUG_PRINT("DEVICE: RWDComSend returned %i.\n", status);
    }
    return false;
}

static void SwallowRemainingAudioMessages() {
	ReWire_uint16_t messageSize = 0;
	ReWireError status;
	do {
		status = RWDComRead(g_DevicePortHandle, PIPE_RT, &messageSize, g_IncomingData);
	} while(kReWireError_NoError == status);  // while messages were still in the pipe
}

static bool MakeSureWeCanWaitForPanel() {
    if (g_EventFromPanel) return true; // nothing to load

    g_EventFromPanel = OpenEventA(SYNCHRONIZE, FALSE, "OPENMPT_REWIRE_PANEL_TO_DEVICE");
    if (!g_EventFromPanel) {
        DEBUG_PRINT("DEVICE: OpenEventA failed, error=%i.", (int)GetLastError());
        return false;
    }
    return true;
}

// true: success, false: failure
static bool WaitForPanel(const int milliseconds = 100) {

    switch (WaitForSingleObject(g_EventFromPanel, milliseconds)) {
    case WAIT_ABANDONED:
    case WAIT_OBJECT_0:
        return true; // success; an audio channel awaits!
    case WAIT_TIMEOUT:
		// The panel may have quit abruptly, test whether this is the case
		if(kReWireError_PortStale == RWDComCheckConnection(g_DevicePortHandle)) {
			RestartDevice();
		}
		break;
    case WAIT_FAILED: // 6 = INVALID_HANDLE
        DEBUG_PRINT("DEVICE: AwaitAudioChannelsFromPanel WAIT_FAILED, error=%i.\n", (int)GetLastError());
    }
    return false; // timeout or error

}

static bool DownloadResponseHeader(MPTAudioResponseHeader* pResponseHeader) {
	if (!WaitForPanel()) return false;

    // Read the actual audio request header
    uint16_t msgSize;
	ReWireError status = RWDComRead(g_DevicePortHandle, PIPE_RT, &msgSize, g_IncomingData);

	if(kReWireError_NoError != status) {
		DEBUG_PRINT("DEVICE: DownloadResponseHeader RWDComRead returned %i.\n", status);
		return false;
	}

    if (msgSize != sizeof(MPTAudioResponseHeader)) {
		DEBUG_PRINT(
            "DEVICE: DownloadResponseHeader msg size %i instead of %i: Discrepancy between expected message type and read message type.\n",
            msgSize, (int)sizeof(MPTAudioResponseHeader)
        );
        return false;
    }
    
    memcpy((uint8_t *)pResponseHeader, g_IncomingData, sizeof(MPTAudioResponseHeader));

	SetEvent(g_EventToPanel);
	return true;
}

static bool DownloadAudioChannelFromPanel(const ReWireDriveAudioInputParams* inputParams) {

    // We presume that there's an audio channel message waiting for us
    uint16_t messageSize = 0;
    ReWireError status = RWDComRead(g_DevicePortHandle, PIPE_RT, &messageSize, g_IncomingData);
    if (kReWireError_NoError != status) {
        // big problem
        DEBUG_PRINT("DEVICE: DownloadAudioChannelFromPanel RWDComRead returned %i.\n", status);
        return false;
    }

    // Make sure the message is of expected size
    size_t szExpectedMin = sizeof(MPTAudioResponse) + (size_t)inputParams->fFramesToRender * 2 * sizeof(int32_t);
    size_t szExpectedMax = sizeof(MPTAudioResponse) + (size_t)g_AudioInfo.fMaxBufferSize * 2 * sizeof(int32_t);
    if (!(messageSize == szExpectedMin || messageSize == szExpectedMax)) {
        DEBUG_PRINT("DEVICE: DownloadAudioChannelFromPanel message was of size %li, expected %li or %li.\n",
            (long)messageSize, (long)szExpectedMin, (long)szExpectedMax);
        return false;
    }

    return true; // success
}

static void UploadAudioChannelToMixer(const ReWireDriveAudioInputParams* inputParams, ReWireDriveAudioOutputParams* outputParams)
{
    // Mark channel as served
    MPTAudioResponse* msg = reinterpret_cast<MPTAudioResponse*>(g_IncomingData);
    ReWireSetBitInBitField(outputParams->fServedChannelsBitField, 2 * msg->channelIndex);
    ReWireSetBitInBitField(outputParams->fServedChannelsBitField, 2 * msg->channelIndex + 1);

    // Upload deinterleaved interleaved channel into mixer's buffers
	int *pServedChannel = reinterpret_cast<int *>(g_IncomingData + sizeof(MPTAudioResponse));
    float* pOutL = inputParams->fAudioBuffers[2 * msg->channelIndex];
    float* pOutR = inputParams->fAudioBuffers[2 * msg->channelIndex + 1];
    for (uint32_t s = 0; s < inputParams->fFramesToRender; s++) {
        *pOutL++ = static_cast<float>(*pServedChannel++) / MIXING_SCALEF;
        *pOutR++ = static_cast<float>(*pServedChannel++) / MIXING_SCALEF;
    }

}


static void ZeroAudioChannel(int channelIndex, const ReWireDriveAudioInputParams *inputParams)
{
	float *pOutL = inputParams->fAudioBuffers[2 * channelIndex];
	float *pOutR = inputParams->fAudioBuffers[2 * channelIndex + 1];
	for(uint32_t s = 0; s < inputParams->fFramesToRender; s++)
	{
		*pOutL++ = 0.0f;
	}
	for(uint32_t s = 0; s < inputParams->fFramesToRender; s++)
	{
		*pOutR++ = 0.0f;
	}
}



void RWDEFDriveAudio(const ReWireDriveAudioInputParams* inputParams, ReWireDriveAudioOutputParams* outputParams)
{
#ifdef DEBUG
    if (g_LastFramesToRender != inputParams->fFramesToRender) {
        g_LastFramesToRender = inputParams->fFramesToRender;
        DEBUG_PRINT("DEVICE: RWDEFDriveAudio inputParams->fFramesToRender = %i.\n", (int)inputParams->fFramesToRender);
	}
#endif

	SwallowRemainingAudioMessages();

    if (!SendRenderRequestToPanel(inputParams, outputParams))
        return; // port not connected or error

    if (!MakeSureWeCanWaitForPanel())
        return; // this should never happen

    // Receive audio response header
	MPTAudioResponseHeader responseHeader;
	if (!DownloadResponseHeader(&responseHeader))
        return;

    // Poll and process audio buffers
    for (int channel = 0; channel < kReWireAudioChannelCount / 2; channel++)
    {
		// Only download channels rendered by OpenMPT
		if(!ReWireIsBitInBitFieldSet(responseHeader.servedChannelsBitfield, channel)) {
			ZeroAudioChannel(channel, inputParams);
			continue;
        }

        // Await audio channel packets from panel
		if(!WaitForPanel()) return;

        // Process the received audio channel
        if (!DownloadAudioChannelFromPanel(inputParams)) return;
        UploadAudioChannelToMixer(inputParams, outputParams);

        // Signal to the panel that we have received and processed the channel
        SetEvent(g_EventToPanel);
    }

	PollAndHandleEvents(outputParams);

}


/**
 * Gets called when the mixer app changes the sample rate or max buffer size.
**/
void RWDEFSetAudioInfo(const ReWireAudioInfo *audioInfo)
{
	DEBUG_PRINT("DEVICE: RWDEFSetAudioInfo: fSampleRate = %i, fMaxBufferSize = %i.\n", audioInfo->fSampleRate, audioInfo->fMaxBufferSize);

	// A change in the audio info struct will automatically be noticed by the panel during audio requests
	ReWirePrepareAudioInfo(&g_AudioInfo, audioInfo->fSampleRate, audioInfo->fMaxBufferSize);
}




/*******************************************************************************
 *
 * Miscellaneous functions
 *
 ******************************************************************************/

void RWDEFIdle() {}

ReWireError RWDEFLaunchPanelApp() {
	return kReWireError_NoError; // OpenMPT should already be running
}

char RWDEFIsPanelAppLaunched() {
    char connectedToPanel = 0; // 0 = no, 1 = yes, 2 = disconnected
    ReWireError status = RWDComCheckConnection(g_DevicePortHandle);
    switch (status) {
    case kReWireError_PortConnected: // port is healthy and panel is connected
        connectedToPanel = 1;
        break;
    case kReWireError_PortStale: // the client panel has crashed/quit unexpectedly
        connectedToPanel = 2;
        break;
    case kReWireError_PortNotConnected: // not yet connected
        connectedToPanel = 0;
        break;
	}
	return (char)connectedToPanel;
}

ReWireError RWDEFQuitPanelApp() {
	// TODO: Implement quitting of OpenMPT here.
	return kReWireError_NoError;
}




/*******************************************************************************
 *
 * Events
 *
 ******************************************************************************/

static void MakePlayEvent(ReWireDriveAudioOutputParams *outputParams, ReWireEvent *event)
{
	ReWireConvertToRequestPlayEvent(event);

    // Force set the BPM
	ReWireRequestTempoEvent *tempoEvent = ReWireConvertToRequestTempoEvent(
        &outputParams->fEventOutBuffer.fEventBuffer[outputParams->fEventOutBuffer.fCount]
    );
	tempoEvent->fTempo = reinterpret_cast<MPTPlayRequest*>(&g_IncomingEvent)->tempo;
	outputParams->fEventOutBuffer.fCount++;
}

static void MakeRepositionEvent(ReWireEvent *event) {

	ReWireRequestRepositionEvent *repositionEvent = ReWireConvertToRequestRepositionEvent(event);
	MPTRepositionRequest *req = reinterpret_cast<MPTRepositionRequest *>(g_IncomingEvent);
	repositionEvent->fPPQ15360Pos = req->position15360PPQ;

	// Make up for latency between now (when COM pipe is read) and when the event was issued at mix-time
	/*LARGE_INTEGER ticksNow;
	QueryPerformanceCounter(&ticksNow);
	double diffInSeconds = (ticksNow.QuadPart - req->timeIssued.QuadPart) / (double)g_PerfFrequency.QuadPart;
	int32_t offsetInPPQ15360 = static_cast<int32_t>(diffInSeconds / 60.0 * req->bpm * 15360);

	DEBUG_PRINT(
        "Repositioning, latency: %4.fms\toffsetInPPQ15360: %li\tBPM:%2.f\n",
		(diffInSeconds * 1000.0), offsetInPPQ15360, req->bpm
    );*/
}

static void MakeTempoEvent(ReWireEvent *event) {
	ReWireRequestTempoEvent *tempoEvent = ReWireConvertToRequestTempoEvent(event);
	tempoEvent->fTempo = *reinterpret_cast<uint32_t *>(&g_IncomingEvent[1]);
	DEBUG_PRINT("Changing tempo to %i.\n", tempoEvent->fTempo);
}



static void PollAndHandleEvents(ReWireDriveAudioOutputParams *outputParams) {
    for (;;) {
		uint16_t messageSize;
		ReWireError status = RWDComRead(g_DevicePortHandle, PIPE_EVENTS, &messageSize, g_IncomingEvent);
		if(kReWireError_NoError != status || 0 == messageSize) {
			break;
		}

		ReWireEvent *event = &outputParams->fEventOutBuffer.fEventBuffer[outputParams->fEventOutBuffer.fCount];
		outputParams->fEventOutBuffer.fCount++;

        DEBUG_PRINT("Incoming event of type %i.\n", g_IncomingEvent[0]);

        switch (g_IncomingEvent[0]) {
			case(uint8_t)MPTPanelEvent::Play:
                MakePlayEvent(outputParams, event);
				//ReWireConvertToRequestPlayEvent(event);
				break;
			case(uint8_t)MPTPanelEvent::Stop:
				ReWireConvertToRequestStopEvent(event);
				break;
			case(uint8_t)MPTPanelEvent::ChangeBPM:
				MakeTempoEvent(event);
				break;
			case(uint8_t)MPTPanelEvent::Reposition:
				MakeRepositionEvent(event);  
				break;

            // @TODO: loop start/stop events
			default:
				outputParams->fEventOutBuffer.fCount--; // effectively cancels event
        }

    }

}


void RWDEFGetEventInfo(ReWireEventInfo* eventInfo) {
	// If we don't have any buses or channels, we must not touch eventInfo
}

void RWDEFGetEventBusInfo(unsigned short busIndex, ReWireEventBusInfo* eventBusInfo) {
	// If we don't have any buses or channels, we must not touch eventBusInfo
}

void RWDEFGetEventChannelInfo(const ReWireEventTarget* eventTarget, ReWireEventChannelInfo* eventChannelInfo) {
	// If we don't have any buses or channels, we must not touch eventBusInfo
}

void RWDEFGetEventControllerInfo(const ReWireEventTarget* eventTarget, ReWire_uint16_t controllerIndex, ReWireEventControllerInfo* controllerInfo) {
	// If we don't have any buses or channels, we must not touch eventBusInfo
}

void RWDEFGetEventNoteInfo(const ReWireEventTarget* eventTarget, ReWire_uint16_t noteIndex, ReWireEventNoteInfo* noteInfo) {
	// If we don't have any buses or channels, we must not touch eventBusInfo
}
