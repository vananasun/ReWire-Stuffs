#ifdef WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include "ReWireAPI.h"
#include "ReWirePanelAPI.h"
#include "MPTRewirePanel.h"
#include "MPTRewireDebugUtils.h"
#include "../../mptrack/Reporting.h"
#include <algorithm>
#include <thread>
#include <string.h>
#include <stdlib.h>



using namespace ReWire;


static std::string getExecutableDirectory() {
	char buffer[MAX_PATH];
	GetModuleFileNameA(NULL, buffer, MAX_PATH);
	std::string path = std::string(buffer);
	std::string::size_type pos = path.find_last_of("\\/");
	path = path.substr(0, pos);
	return path;
}



/*******************************************************************************
 *
 * Main functions: Opening, closing, thread proc
 * 
 ******************************************************************************/

MPTRewirePanel::MPTRewirePanel()
{

	// Open ReWire
	ReWireError status = RWPOpen();
	if(kReWireError_NoError != status && kReWireImplError_ReWireAlreadyOpen != status)
	{
		DEBUG_PRINT("RWPOpen() status=%i\n", status);
		Reporting::Error("ReWire Sound Device: Failed to open ReWire itself.\nTry restarting your computer.");
		m_Errored = true;
		return;
	}

	// Register device
	std::string deviceDllPath = getExecutableDirectory() + "\\MPTRewire.dll";
	RWPUnregisterReWireDevice(deviceDllPath.c_str()); // forces the current device file path
	status = RWPRegisterReWireDevice(deviceDllPath.c_str());
	if(kReWireError_NoError != status && kReWireError_AlreadyExists != status)
	{
		DEBUG_PRINT("RWPRegisterReWireDevice status=%i\n", (int)status);
		Reporting::Error("ReWire Sound Device: Unable to register the device.\nHave you tried running OpenMPT as administrator?");
		m_Errored = true;
		return;
	}

	// Make sure there are allocated audio buffers at all times
	reallocateBuffers(8192);

}

MPTPanelStatus MPTRewirePanel::open(
	MPTRenderCallback renderCallback,
	MPTAudioInfoCallback audioInfoCallback,
	MPTMixerQuitCallback mixerQuitCallback,
	void *callbackUserData)
{

#if defined(WINDOWS) && defined(DEBUG)
	AllocConsole();
	if(!freopen("CONOUT$", "w", stderr)) return MPTPanelStatus::UnknownDeviceProblem;
#endif

	// Check whether mixer is running
	ReWire_char_t isRunning = 0;
	ReWireError status = RWPIsReWireMixerAppRunning(&isRunning);
	if(kReWireError_NoError != status || !isRunning)
	{
		return MPTPanelStatus::MixerNotRunning;
	}

	// Load the device
	// DEBUG_PRINT("Loading ReWire device at \"%s\".\n", deviceDllPath.c_str());
	m_EventToDevice = CreateEventA(NULL, FALSE, FALSE, "OPENMPT_REWIRE_PANEL_TO_DEVICE");
	status = RWPLoadDevice(m_DeviceName);
	if (kReWireError_UnableToOpenDevice == status) {
		return MPTPanelStatus::UnknownDeviceProblem;
	} else if(kReWireError_NoError != status)
	{
		DEBUG_PRINT("RWPLoadDevice status=%i\n", (int)status);
		return MPTPanelStatus::DeviceNotInstalled;
	}

	// Set up communications with the device
	m_EventFromDevice = OpenEventA(SYNCHRONIZE, FALSE, "OPENMPT_REWIRE_DEVICE_TO_PANEL");
	if(!m_EventFromDevice)
	{
		DEBUG_PRINT("OpenEventA failed, error=%i.\n", (int)GetLastError());
		return MPTPanelStatus::UnknownDeviceProblem;
	}

	status = RWPComConnect("OMPT", &m_PanelPortHandle);
	if(status != kReWireError_NoError) // @TODO: kReWireError_Busy (I think when connecting after mixer quit)
	{
		DEBUG_PRINT("RWPComConnect status=%i\n", (int)status);
		return MPTPanelStatus::ReWireProblem;
	}

	// Start audio thread
	m_CallbackUserData = callbackUserData;
	m_RenderCallback = renderCallback;
	m_AudioInfoCallback = audioInfoCallback;
	m_MixerQuitCallback = mixerQuitCallback;
	m_MixerQuit = false;
	m_Running = true;
	m_Thread = std::thread(&MPTRewirePanel::threadProc, this);
	return MPTPanelStatus::Ok;
}





MPTRewirePanel::~MPTRewirePanel()
{
	// Try to close panel API
	ReWire_char_t okFlag = 0;
	ReWireError status = RWPIsCloseOK(&okFlag);
	if(kReWireError_NoError == status && okFlag)
		RWPClose();
	else
		DEBUG_PRINT("RWPIsCloseOk status=%i okFlag=%i\n", (int)status, (int)okFlag);

	deallocateBuffers();
}

bool MPTRewirePanel::close()
{
	if(!m_Running) return true;
	m_Running = false;
	if(m_Thread.joinable()) m_Thread.join();
	CloseHandle(m_EventToDevice);

	ReWireError status = RWPComDisconnect(m_PanelPortHandle);
	if(kReWireError_NoError != status)
	{
		DEBUG_PRINT("RWPComDisconnect status=%i\n", (int)status);
	}

	// Unload device, unfortunately due to a bug in ReWire, if the mixer crashes, and the panel tries to unload
	// during the crash, the thread blocks for a good 15 seconds and then returns kReWireError_UnableToOpenDevice.
	// NOTE: In the future I might just write a check that tests if the ReWire device has a blocking thread.
	status = RWPUnloadDevice(m_DeviceName);
	if(kReWireError_NoError != status && kReWireImplError_ReWireNotOpen != status)
	{
		DEBUG_PRINT("RWPUnloadDevice status=%i\n", (int)status);
	}

	// deallocateBuffers();
	// m_MaxBufferSize = m_SampleRate = 0; // force re-allocate buffers when we open the device again
	return true;
}



/**
 * Panel thread that checks for audio requests and tells OpenMPT to render audio
**/
void MPTRewirePanel::threadProc()
{
	while(m_Running)
	{
		checkComConnection();
		pollAudioRequests();
	}
}



/*******************************************************************************
 *
 * Audio buffer functions
 * 
 ******************************************************************************/

void MPTRewirePanel::deallocateBuffers() {
	if(m_AudioBuffers)
	{
		for(int i = 0; i < kReWireAudioChannelCount / 2; i++)
			if(m_AudioBuffers[i])
				delete[] m_AudioBuffers[i];
		delete[] m_AudioBuffers;
		m_AudioBuffers = nullptr;
	}
	if(m_AudioResponseBuffer)
	{
		delete[] m_AudioResponseBuffer;
		m_AudioResponseBuffer = nullptr;
	}
}


void MPTRewirePanel::reallocateBuffers(int32_t maxBufferSize)
{
	m_MaxBufferSize = maxBufferSize;

	deallocateBuffers();
	m_AudioBuffers = new int *[kReWireAudioChannelCount / 2];
	for(int i = 0; i < kReWireAudioChannelCount / 2; i++)
		m_AudioBuffers[i] = new int[(size_t)m_MaxBufferSize * 2];
	m_AudioResponseBuffer = reinterpret_cast<MPTAudioResponse *>(new uint8_t[sizeof(MPTAudioResponse) + ((size_t)m_MaxBufferSize * 2 * sizeof(int32_t))]);
}




/*******************************************************************************
 *
 * Basic COM functions
 * 
 ******************************************************************************/

void MPTRewirePanel::checkComConnection()
{
	if (kReWireError_PortConnected == RWPComCheckConnection(m_PanelPortHandle)) {
		return; // all is fine; no action
	}
	if (!m_MixerQuit) {
		m_MixerQuit = true;
		m_MixerQuitCallback(m_CallbackUserData);
	}
}


void MPTRewirePanel::swallowRemainingMessages()
{
	ReWire_uint16_t messageSize = 0;
	ReWireError status;
	do
	{
		status = RWPComRead(m_PanelPortHandle, PIPE_RT, &messageSize, m_Message);
	} while(kReWireError_NoError == status);  // while a message was still in the pipe
}


bool MPTRewirePanel::waitForEventFromDevice(const int milliseconds)
{
	switch(WaitForSingleObject(m_EventFromDevice, milliseconds))
	{
		case WAIT_ABANDONED:
		case WAIT_OBJECT_0:
			return true;
		case WAIT_TIMEOUT:
			{
				// Detect if the mixer app has quit abruptly
				ReWire_char_t isRunning = 0;
				if(kReWireError_NoError != RWPIsReWireMixerAppRunning(&isRunning) || !isRunning)
				{
					if(!m_MixerQuit)
					{
						m_MixerQuit = true;
						m_MixerQuitCallback(m_CallbackUserData);
					}
				}
				break;
			}
		case WAIT_FAILED:  // 6 = INVALID_HANDLE
			DEBUG_PRINT("waitForEventFromDevice WAIT_FAILED, error=%i.\n", (int)GetLastError());
	}
	return false;  // timeout or error
}




/*******************************************************************************
 *
 * Audio requests
 * 
 ******************************************************************************/

void MPTRewirePanel::pollAudioRequests()
{
	// Wait for the device to request audio from us
	if(!waitForEventFromDevice()) return;

	// Read requested audio buffer properties
	MPTAudioRequest request;
	ReWire_uint16_t messageSize = 0;
	ReWireError status = RWPComRead(m_PanelPortHandle, PIPE_RT, &messageSize, m_Message);
	if(kReWireError_NoError != status)
	{
		if(kReWireError_NoMoreMessages != status)
			DEBUG_PRINT("RWPComRead returned %i.\n", (int)status);
		return;
	}

	if(messageSize < sizeof(MPTAudioRequest)) return;  // prevent potential access violation
	memcpy(&request, m_Message, messageSize);

	// Handle changes in samplerate and buffer size
	// This also happens after opening the panel to (re-)allocate the buffers
	if(m_SampleRate != request.sampleRate || m_MaxBufferSize != request.maxBufferSize)
	{
		handleAudioInfoChange(request.sampleRate, request.maxBufferSize);
	}

	swallowRemainingMessages();
	generateAudioAndUploadToDevice(request);
}



void MPTRewirePanel::generateAudioAndUploadToDevice(MPTAudioRequest request)
{
	// Let OpenMPT render the audio channels
	ReWireClearBitField(m_ServedChannelsBitfield, kReWireAudioChannelCount / 2);
	m_RenderCallback(request.framesToRender, m_CallbackUserData);

	// Inform the device that we are going to send audio packets
	sendAudioResponseHeaderToDevice();

	// Send response for each interleaved stereo channel
	uint16_t audioDataSize = (uint16_t)(request.framesToRender * 2 * sizeof(int32_t));
	uint16_t responseSize = (uint16_t)(sizeof(MPTAudioResponse) + audioDataSize);
	for(uint16_t channel = 0; channel < kReWireAudioChannelCount / 2; channel++)
	{

		// Only serve rendered channels
		if(!ReWireIsBitInBitFieldSet(m_ServedChannelsBitfield, channel))
			continue;

		// Send channel to device
		m_AudioResponseBuffer->channelIndex = channel;
		uint8_t *pDest = reinterpret_cast<uint8_t *>(m_AudioResponseBuffer) + sizeof(MPTAudioResponse);
		memcpy(pDest, reinterpret_cast<void *>(m_AudioBuffers[channel]), audioDataSize);

		ReWireError status = RWPComSend(m_PanelPortHandle, PIPE_RT, responseSize, (uint8_t *)m_AudioResponseBuffer);
		if(kReWireError_NoError != status)
		{
			DEBUG_PRINT("RWPComSend status=%i channel=%i\n", (int)status, (int)channel);
			break;
		}

		// Signal to device that we have just sent a channel
		SetEvent(m_EventToDevice);

		// ... (Device is going to process our channel) ...

		// Wait for device to signal that it received our channel
		if(!waitForEventFromDevice()) break;
	}
}



bool MPTRewirePanel::sendAudioResponseHeaderToDevice()
{
	MPTAudioResponseHeader packet;
	memcpy((uint8_t *)&packet.servedChannelsBitfield, m_ServedChannelsBitfield, sizeof(MPTAudioResponseHeader::servedChannelsBitfield));

	ReWireError status = RWPComSend(m_PanelPortHandle, PIPE_RT, sizeof(packet), (uint8_t *)&packet);
	if(kReWireError_NoError != status)
	{
		DEBUG_PRINT("sendAudioResponseHeaderToDevice(): RWPComSend status=%i\n", (int)status);
		return false;
	}

	SetEvent(m_EventToDevice);
	return waitForEventFromDevice();
}



void MPTRewirePanel::handleAudioInfoChange(int sampleRate, int maxBufferSize)
{
	DEBUG_PRINT("Samplerate = %i, MaxBufferSize = %i\n", sampleRate, maxBufferSize);
	reallocateBuffers(maxBufferSize);

	// If this function was called because we received our first audio request,
	// then we do not need to notify the ReWire sound device.
	if(0 != m_SampleRate)
	{
		m_AudioInfoCallback(sampleRate, maxBufferSize, m_CallbackUserData);
	}

	m_SampleRate = sampleRate;
}



/*******************************************************************************
 *
 * Event signalling functions that send requests to the mixer.
 * 
 ******************************************************************************/

void MPTRewirePanel::signalPlay(double bpm) {
	MPTPlayRequest req;
	req.type = (uint8_t)MPTPanelEvent::Play;
	req.tempo = static_cast<uint32_t>(bpm * 1000);
	RWPComSend(m_PanelPortHandle, PIPE_EVENTS, sizeof(req), reinterpret_cast<uint8_t *>(&req));
}

void MPTRewirePanel::signalStop() {
	uint8_t message[1];
	message[0] = (uint8_t)MPTPanelEvent::Stop;
	RWPComSend(m_PanelPortHandle, PIPE_EVENTS, sizeof(message), message);
}

void MPTRewirePanel::signalReposition(double bpm, int nFrames) {

	/*DEBUG_PRINT("Reposition, frames:%i\n",
		nFrames
	);*/

	MPTRepositionRequest req;
	req.type = (uint8_t)MPTPanelEvent::Reposition;
	double seconds = (double)nFrames / m_SampleRate;
	double bps = bpm / 60.0;
	double beatsPassed = seconds * bps;
	//req.position15360PPQ = 15360 * bps; //@TODO: * 4? // @TODO: minus one entire buffer time length?
	req.position15360PPQ = (15360 * 4 * 8) - (uint32_t)(15360 * beatsPassed);

	//MPTRepositionRequest req;
	//req.type = (uint8_t)MPTPanelEvent::Reposition;

	//uint32_t tickDifference = ticksInLastPattern - m_TicksAtDriveCall;
	//req.position15360PPQ = static_cast<uint32_t>(15360 * tickDifference / m_PlayState.TicksOnRow() / m_PlayState.m_nCurrentRowsPerBeat());
	////req.position15360PPQ = static_cast<uint32_t>((rowWithOffset / rowsPerBeat) * 15360);

	//// QueryPerformanceCounter(&req.timeIssued); // @TODO: remove
	//req.bpm = bpm;
	RWPComSend(m_PanelPortHandle, PIPE_EVENTS, sizeof(req), reinterpret_cast<uint8_t*>(&req));
}

void MPTRewirePanel::signalBPMChange(double bpm) {
	uint8_t message[5];
	message[0] = (uint8_t)MPTPanelEvent::ChangeBPM;
	*(uint32_t *)&message[1] = static_cast<uint32_t>(bpm * 1000);
	RWPComSend(m_PanelPortHandle, PIPE_EVENTS, sizeof(message), message);
}