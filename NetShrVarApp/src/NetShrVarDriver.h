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

/// An STL exception describing a Win32 Structured Exception. 
/// Code needs to be compiled with /EHa if you wish to use this via _set_se_translator().
/// Note that _set_se_translator() needs to be called on a per thread basis
class Win32StructuredException : public std::runtime_error
{
public:
	explicit Win32StructuredException(const std::string& message) : std::runtime_error(message) { }
	explicit Win32StructuredException(unsigned int code, EXCEPTION_POINTERS *pExp) : std::runtime_error(win32_message(code, pExp)) { }
private:
	static std::string win32_message(unsigned int code, EXCEPTION_POINTERS * pExp);
};


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

	template<typename T> asynStatus writeValue(asynUser *pasynUser, const char* functionName, T value);
	template<typename T> asynStatus writeArrayValue(asynUser *pasynUser, const char* functionName, T* value, size_t nElements);
	template<typename T> asynStatus readArrayValue(asynUser *pasynUser, const char* functionName, T *value, size_t nElements, size_t *nIn);

	static void NetShrVarTask(void* arg);
};

#endif /* NETSHRVARDRIVER_H */
