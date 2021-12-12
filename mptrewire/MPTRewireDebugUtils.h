#pragma once
#ifdef DEBUG

#include <stdint.h>
#include <math.h>
#include <ReWire.h>
#include <chrono>

// Logging function
#define DEBUG_PRINT(format, ...) fprintf(stderr, format, __VA_ARGS__)



/*******************************************************************************
 * 
 * Test wave fillers
 * 
 ******************************************************************************/

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef MIXING_SCALEF
#define MIXING_SCALEF 134217728.0f
#endif

int g_DummyDataIndexInt   = 0;
int g_DummyDataIndexFloat = 0;

static void FillWithDummyAudioDataInt32(int32_t *channel, uint16_t framesToRender, int sampleRate) {
	for(int32_t *p = channel; p < &channel[framesToRender * 2];) {
		*p++ = static_cast<int32_t>(sin(2.0 * M_PI * 432.0 * g_DummyDataIndexInt / sampleRate) * MIXING_SCALEF);
		*p++ = static_cast<int32_t>(sin(2.0 * M_PI * 120.0 * g_DummyDataIndexInt / sampleRate) * MIXING_SCALEF);
		g_DummyDataIndexInt++;
	}
}

static void FillWithDummyAudioDataFloat(float *channel, uint16_t framesToRender, int sampleRate) {
	for(float *p = channel; p < &channel[framesToRender * 2];) {
		*p++ = static_cast<float>(sin(2.0 * M_PI * 432.0 * g_DummyDataIndexFloat / sampleRate));
		*p++ = static_cast<float>(sin(2.0 * M_PI * 120.0 * g_DummyDataIndexFloat / sampleRate));
		g_DummyDataIndexFloat++;
	}
}


/*******************************************************************************
 * 
 * Bit-fields
 * 
 ******************************************************************************/

static void PrintBitField(const uint32_t *bitfield, const uint16_t bitCount = kReWireAudioChannelCount) {
	char* b = new char[static_cast<size_t>(bitCount) + 2];
	for(uint16_t i = 0; i < bitCount; i++) {
		b[i] = (1 == ReWire::ReWireIsBitInBitFieldSet(bitfield, i)) ? '1' : '0';
	}
	b[bitCount] = '\n';
	b[bitCount + 1] = '\0';
	DEBUG_PRINT(b);
	delete [] b;
}

static bool CompareBitFields(const uint32_t *bitfieldA, const uint32_t *bitfieldB, const uint16_t bitCount = kReWireAudioChannelCount) {
	for (int byte = 0; byte < REWIRE_BITFIELD_SIZE(bitCount); byte++) {
		if(bitfieldA[byte] != bitfieldB[byte])
			return false;
	}
	return true;
}


/*******************************************************************************
 * 
 * Profiling
 * 
 ******************************************************************************/
LARGE_INTEGER g_DebugPerfFreq, g_DebugTicksNow, g_DebugTicksStart;
double g_DebugDiffMs;

#define DEBUG_PROFILING_START()\
	{\
		QueryPerformanceCounter(&g_DebugTicksStart);\
	}

#define DEBUG_PROFILING_STOP()\
	{\
		QueryPerformanceCounter(&g_DebugTicksNow);\
		QueryPerformanceFrequency(&g_DebugPerfFreq);\
		g_DebugDiffMs = (g_DebugTicksNow.QuadPart - g_DebugTicksStart.QuadPart) / (g_PerfFrequency.QuadPart / 1000.0);\
		DEBUG_PRINT(\
			"Profiling took %f ms.\n",\
			g_DebugDiffMs\
		);\
	}


#else
#define DEBUG_PRINT(...) {}
#define DEBUG_PROFILING_START() {}
#define DEBUG_PROFILING_STOP() {}
#endif