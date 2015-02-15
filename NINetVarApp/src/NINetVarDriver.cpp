/*************************************************************************\ 
* Copyright (c) 2013 Science and Technology Facilities Council (STFC), GB. 
* All rights reverved. 
* This file is distributed subject to a Software License Agreement found 
* in the file LICENSE.txt that is included with this distribution. 
\*************************************************************************/ 

/// @file NINetVarDriver.cpp Implementation of #NINetVarDriver class and NINetVarConfigure() iocsh command
/// @author Freddie Akeroyd, STFC ISIS Facility, GB

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <exception>
#include <iostream>

#include <shareLib.h>
#include <epicsTypes.h>
#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsString.h>
#include <epicsTimer.h>
#include <epicsMutex.h>
#include <epicsEvent.h>
#include <errlog.h>
#include <iocsh.h>

#include <windows.h>

#include "convertToString.h"
#include "NINetVarInterface.h"
#include "NINetVarDriver.h"

#include <epicsExport.h>

/// Helper function to map a win32 structured exception into a C++ standard exception
std::string Win32StructuredException::win32_message(unsigned int code, EXCEPTION_POINTERS * pExp)
{
	char buffer[256];
	_snprintf(buffer, sizeof(buffer), "Win32StructuredException code 0x%x pExpCode 0x%x pExpAddress %p", code, pExp->ExceptionRecord->ExceptionCode, pExp->ExceptionRecord->ExceptionAddress);
	buffer[sizeof(buffer)-1] = '\0';
	return std::string(buffer);
}

static const char *driverName="NINetVarDriver"; ///< Name of driver for use in message printing 

/// Function to translate a Win32 structured exception into a standard C++ exception. 
/// This is registered via registerStructuredExceptionHandler()
static void seTransFunction(unsigned int u, EXCEPTION_POINTERS* pExp)
{
	throw Win32StructuredException(u, pExp);
}

/// Register a handler for Win32 strcutured exceptions. This needs to be done on a per thread basis.
static void registerStructuredExceptionHandler()
{
	_set_se_translator(seTransFunction);
}

/// write a value to the driver
/// @tparam T data type of \a value
/// @param[in] pasynUser pointer to AsynUser instance
/// @param[in] functionName Name of overloaded ASYN driver function that called us, used for diagnostics
/// @param[in] value Value to write
template<typename T>
asynStatus NINetVarDriver::writeValue(asynUser *pasynUser, const char* functionName, T value)
{
	int function = pasynUser->reason;
	asynStatus status = asynSuccess;
	const char *paramName = NULL;
	registerStructuredExceptionHandler();
	getParamName(function, &paramName);
	try
	{
		if (m_netvarint == NULL)
		{
			throw std::runtime_error("m_netvarint is NULL");
		}
		m_netvarint->setValue(paramName, value);
		asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
			"%s:%s: function=%d, name=%s, value=%s\n", 
			driverName, functionName, function, paramName, convertToString(value).c_str());
		return asynSuccess;
	}
	catch(const std::exception& ex)
	{
		epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize, 
			"%s:%s: status=%d, function=%d, name=%s, value=%s, error=%s", 
			driverName, functionName, status, function, paramName, convertToString(value).c_str(), ex.what());
		return asynError;
	}
}

/// write an array to the driver
/// @tparam T Data type of \a value
/// @param[in] pasynUser pointer to AsynUser instance
/// @param[in] functionName Name of overloaded ASYN driver function that called us, used for diagnostics
/// @param[in] value Value to write
/// @param[in] nElements number of array elements
template<typename T>
asynStatus NINetVarDriver::writeArrayValue(asynUser *pasynUser, const char* functionName, T* value, size_t nElements)
{
	int function = pasynUser->reason;
	asynStatus status = asynSuccess;
	const char *paramName = NULL;
	registerStructuredExceptionHandler();
	getParamName(function, &paramName);
	try
	{
		if (m_netvarint == NULL)
		{
			throw std::runtime_error("m_netvarint is NULL");
		}
		m_netvarint->setArrayValue(paramName, value, nElements);
		asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
			"%s:%s: function=%d, name=%s, nElements=%d\n", 
			driverName, functionName, function, paramName, nElements);
		return asynSuccess;
	}
	catch(const std::exception& ex)
	{
		epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize, 
			"%s:%s: status=%d, function=%d, name=%s, nElements=%d, error=%s", 
			driverName, functionName, status, function, paramName, nElements, ex.what());
		return asynError;
	}
}

/// write a float to the driver
/// @param[in] pasynUser pointer to AsynUser instance
/// @param[in] value Value to write
asynStatus NINetVarDriver::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
	asynStatus status = writeValue(pasynUser, "writeFloat64", value);
	return (status == asynSuccess ? asynPortDriver::writeFloat64(pasynUser, value) : status);
}

asynStatus NINetVarDriver::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
	asynStatus status = writeValue(pasynUser, "writeInt32", value);
	return (status == asynSuccess ? asynPortDriver::writeInt32(pasynUser, value) : status);
}

asynStatus NINetVarDriver::writeOctet(asynUser *pasynUser, const char *value, size_t maxChars, size_t *nActual)
{
	int function = pasynUser->reason;
	asynStatus status = asynSuccess;
	const char *paramName = NULL;
	registerStructuredExceptionHandler();
	getParamName(function, &paramName);
	const char* functionName = "writeOctet";
	std::string value_s(value, maxChars);
	try
	{
		if (m_netvarint == NULL)
		{
			throw std::runtime_error("m_netvarint is NULL");
		}
		m_netvarint->setValue(paramName, value_s);
		asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
			"%s:%s: function=%d, name=%s, value=%s\n", 
			driverName, functionName, function, paramName, value_s.c_str());
		*nActual = value_s.size();
		return asynPortDriver::writeOctet(pasynUser, value_s.c_str(), maxChars, nActual);
	}
	catch(const std::exception& ex)
	{
		epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize, 
			"%s:%s: status=%d, function=%d, name=%s, value=%s, error=%s", 
			driverName, functionName, status, function, paramName, value_s.c_str(), ex.what());
		*nActual = 0;
		return asynError;
	}
}

asynStatus NINetVarDriver::readFloat64Array(asynUser *pasynUser, epicsFloat64 *value, size_t nElements, size_t *nIn)
{
	return readArrayValue(pasynUser, "readFloat64Array", value, nElements, nIn);
}

asynStatus NINetVarDriver::readFloat32Array(asynUser *pasynUser, epicsFloat32 *value, size_t nElements, size_t *nIn)
{
	return readArrayValue(pasynUser, "readFloat32Array", value, nElements, nIn);
}

asynStatus NINetVarDriver::readInt32Array(asynUser *pasynUser, epicsInt32 *value, size_t nElements, size_t *nIn)
{
	return readArrayValue(pasynUser, "readInt32Array", value, nElements, nIn);
}

asynStatus NINetVarDriver::readInt16Array(asynUser *pasynUser, epicsInt16 *value, size_t nElements, size_t *nIn)
{
	return readArrayValue(pasynUser, "readInt16Array", value, nElements, nIn);
}

asynStatus NINetVarDriver::readInt8Array(asynUser *pasynUser, epicsInt8 *value, size_t nElements, size_t *nIn)
{
	return readArrayValue(pasynUser, "readInt8Array", value, nElements, nIn);
}

template<typename T>
asynStatus NINetVarDriver::readArrayValue(asynUser *pasynUser, const char* functionName, T *value, size_t nElements, size_t *nIn)
{
	int function = pasynUser->reason;
	asynStatus status = asynSuccess;
	const char *paramName = NULL;
	registerStructuredExceptionHandler();
	getParamName(function, &paramName);
	try
	{
		if (m_netvarint == NULL)
		{
			throw std::runtime_error("m_netvarint is NULL");
		}
		m_netvarint->readArrayValue(paramName, value, nElements, nIn);
		asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
			"%s:%s: function=%d, name=%s, size=%d\n", 
			driverName, functionName, function, paramName, nElements);
		return asynSuccess;
	}
	catch(const std::exception& ex)
	{
		*nIn = 0;
		epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize, 
			"%s:%s: status=%d, function=%d, name=%s, size=%d, error=%s", 
			driverName, functionName, status, function, paramName, nElements, ex.what());
		return asynError;
	}
}

asynStatus NINetVarDriver::writeInt32Array(asynUser *pasynUser, epicsInt32 *value, size_t nElements)
{
    return writeArrayValue(pasynUser, "writeInt32Array", value, nElements);
}

asynStatus NINetVarDriver::writeInt16Array(asynUser *pasynUser, epicsInt16 *value, size_t nElements)
{
    return writeArrayValue(pasynUser, "writeInt16Array", value, nElements);
}

asynStatus NINetVarDriver::writeInt8Array(asynUser *pasynUser, epicsInt8 *value, size_t nElements)
{
    return writeArrayValue(pasynUser, "writeInt8Array", value, nElements);
}

asynStatus NINetVarDriver::writeFloat64Array(asynUser *pasynUser, epicsFloat64 *value, size_t nElements)
{
    return writeArrayValue(pasynUser, "writeFloat64Array", value, nElements);
}

asynStatus NINetVarDriver::writeFloat32Array(asynUser *pasynUser, epicsFloat32 *value, size_t nElements)
{
    return writeArrayValue(pasynUser, "writeFloat32Array", value, nElements);
}

/// EPICS driver report function for iocsh dbior command
void NINetVarDriver::report(FILE* fp, int details)
{
	if (m_netvarint != NULL)
	{
		m_netvarint->report(fp, details);
	}
	else
	{
		fprintf(fp, "NetVarInt pointer is NULL\n");
	}
	asynPortDriver::report(fp, details);
}


/// Constructor for the #NINetVarDriver class.
/// Calls constructor for the asynPortDriver base class and sets up driver parameters.
///
/// @param[in] netvarint  interface pointer created by NINetVarConfigure()
/// @param[in] poll_ms  @copydoc initArg0
/// @param[in] portName @copydoc initArg3
NINetVarDriver::NINetVarDriver(NINetVarInterface* netvarint, int poll_ms, const char *portName) 
	: asynPortDriver(portName, 
	0, /* maxAddr */ 
	netvarint->nParams(),
	asynInt32Mask | asynInt32ArrayMask | asynFloat64Mask | asynFloat64ArrayMask | asynOctetMask | asynDrvUserMask, /* Interface mask */
	asynInt32Mask | asynInt32ArrayMask | asynFloat64Mask | asynFloat64ArrayMask | asynOctetMask,  /* Interrupt mask */
	ASYN_CANBLOCK, /* asynFlags.  This driver can block but it is not multi-device */
	1, /* Autoconnect */
	0, /* Default priority */
	0),	/* Default stack size*/
	m_netvarint(netvarint), m_poll_ms(poll_ms), m_shutting_down(false)
{
	const char *functionName = "NINetVarDriver";
	
	m_netvarint->createParams(this);
	if (poll_ms == 0)
	{
	    std::cerr << "Warning: driver is not polling for buffered reads, only subscribers will see changes" << std::endl;
	}
    epicsAtExit(epicsExitFunc, this);

	// Create the thread for background tasks (not used at present, could be used for I/O intr scanning) 
	if (epicsThreadCreate("NINetVarDriverTask",
		epicsThreadPriorityMedium,
		epicsThreadGetStackSize(epicsThreadStackMedium),
		(EPICSTHREADFUNC)NINetVarTask, this) == 0)
	{
		printf("%s:%s: epicsThreadCreate failure\n", driverName, functionName);
		return;
	}
}

void NINetVarDriver::epicsExitFunc(void* arg)
{
	NINetVarDriver* driver = static_cast<NINetVarDriver*>(arg);
	if (driver == NULL)
	{
		return;
	}
	driver->shuttingDown(true);
}


void NINetVarDriver::NINetVarTask(void* arg) 
{ 
	NINetVarDriver* driver = (NINetVarDriver*)arg; 	
	registerStructuredExceptionHandler();
	int poll_ms = driver->pollTime();
	if (poll_ms > 0)
	{
		while(!driver->shuttingDown())
		{
			driver->updateValues();
			epicsThreadSleep(static_cast<double>(poll_ms) / 1000.0);
		}
	}
}

extern "C" {

	/// EPICS iocsh callable function to call constructor of NINetVarInterface().
	/// The function is registered via NINetVarRegister().
	///
	/// @param[in] portName @copydoc initArg0
	/// @param[in] configSection @copydoc initArg1
	/// @param[in] configFile @copydoc initArg2
	/// @param[in] pollPeriod @copydoc initArg3
	/// @param[in] options @copydoc initArg4
	int NINetVarConfigure(const char *portName, const char* configSection, const char *configFile, int pollPeriod, int options)
	{
		registerStructuredExceptionHandler();
		try
		{
			NINetVarInterface* netvarint = new NINetVarInterface(configSection, configFile, options);
			if (netvarint != NULL)
			{
				new NINetVarDriver(netvarint, pollPeriod, portName);
				return(asynSuccess);
			}
			else
			{
				errlogSevPrintf(errlogFatal, "NINetVarConfigure failed (NULL)\n");
				return(asynError);
			}

		}
		catch(const std::exception& ex)
		{
			errlogSevPrintf(errlogFatal, "NINetVarConfigure failed: %s\n", ex.what());
			return(asynError);
		}
	}

	// EPICS iocsh shell commands 

	static const iocshArg initArg0 = { "portName", iocshArgString};			///< The name of the asyn driver port we will create
	static const iocshArg initArg1 = { "configSection", iocshArgString};	///< section name of \a configFile to use to configure this asyn port
	static const iocshArg initArg2 = { "configFile", iocshArgString};		///< Path to the XML input file to load configuration information from
	static const iocshArg initArg3 = { "pollPeriod", iocshArgInt};			    ///< poll period (ms)
	static const iocshArg initArg4 = { "options", iocshArgInt};			    ///< options as per #NINetVarOptions enum

	static const iocshArg * const initArgs[] = { &initArg0,
		&initArg1,
		&initArg2,
		&initArg3,
		&initArg4 };

	static const iocshFuncDef initFuncDef = {"NINetVarConfigure", sizeof(initArgs) / sizeof(iocshArg*), initArgs};

	static void initCallFunc(const iocshArgBuf *args)
	{
		NINetVarConfigure(args[0].sval, args[1].sval, args[2].sval, args[3].ival, args[4].ival);
	}
	
	/// Register new commands with EPICS IOC shell
	static void NINetVarRegister(void)
	{
		iocshRegister(&initFuncDef, initCallFunc);
	}

	epicsExportRegistrar(NINetVarRegister);

}

