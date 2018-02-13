/*************************************************************************\ 
* Copyright (c) 2013 Science and Technology Facilities Council (STFC), GB. 
* All rights reverved. 
* This file is distributed subject to a Software License Agreement found 
* in the file LICENSE.txt that is included with this distribution. 
\*************************************************************************/ 

/// @file NetShrVarDriver.h Header for #NetShrVarDriver class.
/// @author Freddie Akeroyd, STFC ISIS Facility, GB

#ifndef NETSHRVARDRIVER_H
#define NETSHRVARDRIVER_H

#include "asynPortDriver.h"

class NetShrVarInterface;

/// EPICS Asyn port driver class. 
class NetShrVarDriver : public asynPortDriver 
{
public:
	NetShrVarDriver(NetShrVarInterface* netvarint, int poll_ms, const char *portName);

	// These are the methods that we override from asynPortDriver
	virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
	virtual asynStatus writeFloat64(asynUser *pasynUser, epicsFloat64 value);
	virtual asynStatus writeOctet(asynUser *pasynUser, const char *value, size_t maxChars, size_t *nActual);
	virtual asynStatus writeInt8Array(asynUser *pasynUser, epicsInt8 *value, size_t nElements); 
	virtual asynStatus writeInt16Array(asynUser *pasynUser, epicsInt16 *value, size_t nElements); 
	virtual asynStatus writeInt32Array(asynUser *pasynUser, epicsInt32 *value, size_t nElements); 
	virtual asynStatus writeFloat32Array(asynUser *pasynUser, epicsFloat32 *value, size_t nElements); 
	virtual asynStatus writeFloat64Array(asynUser *pasynUser, epicsFloat64 *value, size_t nElements); 
	virtual asynStatus readInt32(asynUser *pasynUser, epicsInt32 *value);
	virtual asynStatus readFloat64(asynUser *pasynUser, epicsFloat64 *value);
	virtual asynStatus readOctet(asynUser *pasynUser, char *value, size_t maxChars, size_t *nActual, int *eomReason);
	virtual asynStatus readFloat32Array(asynUser *pasynUser, epicsFloat32 *value, size_t nElements, size_t *nIn);
	virtual asynStatus readFloat64Array(asynUser *pasynUser, epicsFloat64 *value, size_t nElements, size_t *nIn);
	virtual asynStatus readInt8Array(asynUser *pasynUser, epicsInt8 *value, size_t nElements, size_t *nIn);
	virtual asynStatus readInt16Array(asynUser *pasynUser, epicsInt16 *value, size_t nElements, size_t *nIn);
	virtual asynStatus readInt32Array(asynUser *pasynUser, epicsInt32 *value, size_t nElements, size_t *nIn);
	virtual void report(FILE* fp, int details);
	int pollTime() { return m_poll_ms; }
	void updateValues()
	{
		m_netvarint->updateValues();
	}	
	static void epicsExitFunc(void* arg);
	void shuttingDown(bool state) { m_shutting_down = state; }
	bool shuttingDown() { return m_shutting_down; }

private:
	NetShrVarInterface* m_netvarint;
	int m_poll_ms;
	bool m_shutting_down;

	asynStatus readValue(asynUser *pasynUser, const char* functionName);
	template<typename T> asynStatus writeValue(asynUser *pasynUser, const char* functionName, T value);
	template<typename T> asynStatus writeArrayValue(asynUser *pasynUser, const char* functionName, T* value, size_t nElements);
	template<typename T> asynStatus readArrayValue(asynUser *pasynUser, const char* functionName, T *value, size_t nElements, size_t *nIn);

	static void NetShrVarTask(void* arg);
};

#endif /* NETSHRVARDRIVER_H */
