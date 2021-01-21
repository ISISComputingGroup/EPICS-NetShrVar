/*************************************************************************\ 
* Copyright (c) 2013 Science and Technology Facilities Council (STFC), GB. 
* All rights reverved. 
* This file is distributed subject to a Software License Agreement found 
* in the file LICENSE.txt that is included with this distribution. 
\*************************************************************************/ 

/// @file NetShrVarInterface.cpp Implementation of #NetShrVarInterface class.
/// @author Freddie Akeroyd, STFC ISIS Facility, GB

#include <stdio.h>

//#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <comutil.h>
#else
#include <math.h>
#include <unistd.h>
#endif /* _WIN32 */

#include <string>
#include <vector>
#include <map>
#include <list>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <limits>

#include <cvirte.h>		
#include <userint.h>
#include <cvinetv.h>

#include <shareLib.h>
#include <macLib.h>
#include <epicsGuard.h>
#include <epicsString.h>
#include <errlog.h>
#include <cantProceed.h>
#include <epicsTime.h>
#include <alarm.h>

#include "pugixml.hpp"

#include "asynPortDriver.h"

#include <epicsExport.h>

#include "NetShrVarInterface.h"
#include "cnvconvert.h"

#define MAX_PATH_LEN 256

static const char *driverName="NetShrVarInterface"; ///< Name of driver for use in message printing 

struct lv_timestamp
{
    int64_t sec_from_epoch; ///< from 01/01/1904 00:00:00.00 UTC
    uint64_t frac;
};

/// An STL exception object encapsulating a shared variable error message
class NetShrVarException : public std::runtime_error
{
public:
	explicit NetShrVarException(const std::string& message) : std::runtime_error(message) { }
	explicit NetShrVarException(const std::string& function, int code) : std::runtime_error(ni_message(function, code)) { }
	static std::string ni_message(const std::string& function, int code)
	{
	    return function + ": " + CNVGetErrorDescription(code);
	}
};

#define ERROR_CHECK(__func, __code) \
    if (__code < 0) \
	{ \
	    throw NetShrVarException(__func, __code); \
	}

#define ERROR_PRINT_CONTINUE(__func, __code) \
    if (__code < 0) \
	{ \
	    std::cerr << NetShrVarException::ni_message(__func, __code); \
		continue; \
	}

/// connection status of a network shared variable
static const char* connectionStatus(CNVConnectionStatus status)
{
		switch (status)
		{
			case CNVConnecting:
				return "Connecting...";
				break;
			case CNVConnected:
				return "Connected";
				break;
			case CNVDisconnected:
				return "Disconnected";
				break;
			default:
				return "UNKNOWN";
				break;			
		}
}

/// A CNVData item that automatically "disposes" itself
class ScopedCNVData
{
	CNVData m_value;	
	public:
	ScopedCNVData(const CNVData& d) : m_value(d) { }
	ScopedCNVData() : m_value(0) { }
	ScopedCNVData(const ScopedCNVData& d) : m_value(d.m_value) { }
	CNVData* operator&() { return &m_value; }
	operator CNVData*() { return &m_value; }
	operator CNVData() { return m_value; }
	ScopedCNVData& operator=(const ScopedCNVData& d) { m_value = d.m_value; return *this; }
	ScopedCNVData& operator=(const CNVData& d) { m_value = d; return *this; }
	bool operator==(CNVData d) const { return m_value == d; }
	bool operator!=(CNVData d) const { return m_value != d; }
	void dispose()
	{
        int status = 0;
		if (m_value != 0)
		{
			status = CNVDisposeData(m_value);
			m_value = 0;
		}
		ERROR_CHECK("CNVDisposeData", status);
	}
    ~ScopedCNVData() { dispose(); }
};

/// details about a network shared variable we have connected to an asyn parameter
struct NvItem
{
	enum { Read=0x1, Write=0x2, BufferedRead=0x4, BufferedWrite=0x8, SingleRead=0x10 } NvAccessMode;   ///< possible access modes to network shared variable
	std::string nv_name;   ///< full path to network shared variable 
	std::string type;   ///< type as specified in the XML file e.g. float64array
	unsigned access; ///< combination of #NvAccessMode
	int field; ///< if we refer to a struct, this is the index of the field (starting at 0), otherwise it is -1 
	int id; ///< asyn parameter id, -1 if not assigned
    std::string ts_param; ///< parameter that is timestamp source
	bool connected_alarm;
	std::vector<char> array_data; ///< only used for array parameters, contains cached copy of data as this is not stored in usual asyn parameter map
	CNVSubscriber subscriber;
	CNVBufferedSubscriber b_subscriber;
	CNVWriter writer;
	CNVReader reader;
	CNVBufferedWriter b_writer;
	epicsTimeStamp epicsTS; ///< timestamp of shared variable update
	NvItem(const std::string& nv_name_, const char* type_, unsigned access_, int field_, const std::string& ts_param_) : nv_name(nv_name_), type(type_), access(access_),
		field(field_), ts_param(ts_param_), id(-1), subscriber(0), b_subscriber(0), writer(0), b_writer(0), reader(0), connected_alarm(false) 
	{ 
	    memset(&epicsTS, 0, sizeof(epicsTS));
	    std::replace(nv_name.begin(), nv_name.end(), '/', '\\'); // we accept / as well as \ in the XML file for path to variable
	}
	/// helper for asyn driver report function
	void report(const std::string& name, FILE* fp)
	{
	    fprintf(fp, "Report for asyn parameter \"%s\" type \"%s\" network variable \"%s\"\n", name.c_str(), type.c_str(), nv_name.c_str());
		if (array_data.size() > 0)
		{
			fprintf(fp, "  Current array size: %d\n", (int)array_data.size());
		}
		if (field != -1)
		{
			fprintf(fp, "  Network variable structure index: %d\n", field);
		}
		char tbuffer[60];
		if (epicsTimeToStrftime(tbuffer, sizeof(tbuffer), "%Y-%m-%d %H:%M:%S.%06f", &epicsTS) <= 0)
		{
			strcpy(tbuffer, "<unknown>");
		}
		fprintf(fp, "  Update time: %s\n", tbuffer);
	    report(fp, "subscriber", subscriber, false);
	    report(fp, "buffered subscriber", b_subscriber, true);
	    report(fp, "writer", writer, false);
	    report(fp, "buffered writer", b_writer, true);
	    report(fp, "reader", reader, false);
	}
	void report(FILE* fp, const char* conn_type, void* handle, bool buffered)
	{
		int error, conn_error;
		CNVConnectionStatus status;
		if (handle == 0)
		{
		    return;
		}
		fprintf(fp, "  Connection type: %s", conn_type);
		try
		{
			error = CNVGetConnectionAttribute(handle, CNVConnectionStatusAttribute, &status);
			ERROR_CHECK("CNVGetConnectionAttribute", error);
			fprintf(fp, "  status: %s", connectionStatus(status));
			error = CNVGetConnectionAttribute(handle, CNVConnectionErrorAttribute, &conn_error);
			ERROR_CHECK("CNVGetConnectionAttribute", error);
			if (conn_error < 0)
			{
				fprintf(fp, " error present: %s", CNVGetErrorDescription(conn_error));
			}
			if (buffered)
			{
				int nitems, maxitems;
				error = CNVGetConnectionAttribute(handle, CNVClientBufferNumberOfItemsAttribute, &nitems);
				ERROR_CHECK("CNVGetConnectionAttribute", error);
				error = CNVGetConnectionAttribute(handle, CNVClientBufferMaximumItemsAttribute, &maxitems);
				ERROR_CHECK("CNVGetConnectionAttribute", error);
				fprintf(fp, "  Client buffer: %d items (buffer size = %d)", nitems, maxitems);
			}
			fprintf(fp, "\n");
		}
		catch (const std::exception& ex)
		{
			fprintf(fp, "  Unable to get connection status: %s\n", ex.what());
		}
	}
};

/// Stores information to be passed back via a shared variable callback on a subscriber connection
struct CallbackData
{
	NetShrVarInterface* intf;
    std::string nv_name;
    int param_index;
	CallbackData(NetShrVarInterface* intf_, const std::string& nv_name_, int param_index_) : intf(intf_), nv_name(nv_name_), param_index(param_index_) { } 
};

static void CVICALLBACK DataCallback (void * handle, CNVData data, void * callbackData);
static void CVICALLBACK StatusCallback (void * handle, CNVConnectionStatus status, int error, void * callbackData);
static void CVICALLBACK DataTransferredCallback(void * handle, int error, void * callbackData);

/// used to perform an initial read of a subscribed variable
void NetShrVarInterface::readVarInit(NvItem* item)
{
    int waitTime = 3000; // in milliseconds, or CNVWaitForever 
    CNVReader reader;
	try {
	    int error = CNVCreateReader(item->nv_name.c_str(), NULL, NULL, waitTime, 0, &reader);
	    ERROR_CHECK("CNVCreateReader", error);
	    ScopedCNVData cvalue;
	    int status = CNVRead(reader, 10, &cvalue);
	    ERROR_CHECK("CNVRead", status);
	    if (cvalue != 0)
	    {
		    updateParamCNV(item->id, cvalue, NULL, true);
	    }
	    CNVDispose(reader);
	}
	catch(const std::exception& ex)
	{
		std::cerr << "Unable to read initial value from \"" << item->nv_name << "\": " << ex.what() << std::endl;
		setParamStatus(item->id, asynError);
	}
}

static const char* getBrowseType(CNVBrowseType browseType)
{
	switch(browseType)
	{
		case CNVBrowseTypeUndefined:
		    return "The item's browse type is not defined.";
			break;
			
		case CNVBrowseTypeMachine:
			return "The item is a computer.";
			break;
			
		case CNVBrowseTypeProcess:
			return "This item is a process.";
			break;
			
		case CNVBrowseTypeFolder:
			return "The item is a folder.";
			break;
			
		case CNVBrowseTypeItem:
			return "The item is a variable.";
			break;
			
		case CNVBrowseTypeItemRange:
			return "The item is a range of variables. ";
			break;
			
		case CNVBrowseTypeImplicitItem:
			return "The item is an implict item.";
			break;
			
		default:
			return "unknown.";
			break;
	}
}
			
// this looks to see if a path can be browsed
bool NetShrVarInterface::pathExists(const std::string& path)
{
#ifdef _WIN32
	CNVBrowser browser = NULL;
	char* item = NULL;
	int leaf, error;
	CNVBrowseType browseType = CNVBrowseTypeUndefined;
	error = CNVCreateBrowser(&browser);
	ERROR_CHECK("CNVCreateBrowser", error);
	error = CNVBrowse(browser, path.c_str()); // error < 0 = not found
	if (error < 0)
	{
		CNVDisposeBrowser(browser);
		return false;
	}
	if (false)
	{
	    error = CNVBrowseNextItem(browser, &item, &leaf, &browseType, NULL);
		std::cerr << "error " << error << " leaf " << leaf << " type " << getBrowseType(browseType) << std::endl;
		// if error > 0 then item != NULL and is the browsed next item
		if (item != NULL)
		{
			std::cerr << item << std::endl;
			CNVFreeMemory(item);
		}
	}
	CNVDisposeBrowser(browser);
	return true;
#else
	return true;
#endif
}

// this only works for localhost variables
bool NetShrVarInterface::varExists(const std::string& path)
{	
#ifdef _WIN32
		int error, exists = 0;
		size_t proc_pos = path.find('\\', 2); // 2 for after \\ in \\localhost\proc\var
		size_t var_pos = path.rfind('\\');

		if (proc_pos != std::string::npos && var_pos != std::string::npos)
		{
		    std::string host_name = path.substr(2, proc_pos - 2);
			std::string proc_name = path.substr(proc_pos + 1, var_pos - proc_pos - 1);
			std::string var_name = path.substr(var_pos + 1);
			if (host_name == "localhost")
			{
		        error = CNVVariableExists(proc_name.c_str(), var_name.c_str(), &exists);
	            ERROR_CHECK("CNVVariableExists", error);
			    if (exists != 0)
			    {
					return true;
			    }
				else
			    {
					return false;
			    }
			}
			else
			{
				return false;
			}
		}
		else
		{
			std::cerr << "varExists: cannot parse \"" << path << "\"" << std::endl;
			return false;
		}
#else
	return false;
#endif
}

void NetShrVarInterface::connectVars()
{
	int error;
	CallbackData* cb_data;
	int waitTime = 3000; // in milliseconds, or CNVWaitForever 
	int clientBufferMaxItems = 200;
#ifdef _WIN32
    int running = 0;
    error = CNVVariableEngineIsRunning(&running); 
	ERROR_CHECK("CNVVariableEngineIsRunning", error);
    if (running == 0)
    {
		std::cerr << "connectVars: NI Variable engine is not running" << std::endl;
    }
    char** processes = NULL;
	int numberOfProcesses = 0;
	int isRunning = 0;
    error = CNVGetProcesses(&processes, &numberOfProcesses);
	ERROR_CHECK("CNVGetProcesses", error);
	std::cerr << "connectVars: NSV processes on machine:";
	for(int i=0; i<numberOfProcesses; ++i)
	{
		error = CNVProcessIsRunning(processes[i], &isRunning);
	    ERROR_CHECK("CNVProcessIsRunning", error);
		std::cerr << " \"" << processes[i] << "\" (" << (isRunning != 0 ? "RUNNING" : "NOT RUNNING") << ")";
	}
	std::cerr << std::endl;
	CNVFreeMemory(processes);
#endif

    // look for alarm network variables
	static const char* alarm_fields[] = { "Hi", "HiHi", "Lo", "LoLo" };
	params_t new_params;
	for(params_t::const_iterator it=m_params.begin(); it != m_params.end(); ++it)
	{
		NvItem* item = it->second;
		std::string param_name = it->first;
		if (pathExists(item->nv_name))
		{
			for(int i=0; i<sizeof(alarm_fields) / sizeof(const char*); ++i)
			{
				std::string prefix = item->nv_name + "\\Alarms\\" + alarm_fields[i] + "\\";
				if (pathExists(prefix + "Enable"))
				{
					std::cerr << "Adding " << alarm_fields[i] << " alarm field for " << item->nv_name << " (asyn parameter: " << param_name << ")" << std::endl;
					item->connected_alarm = true;
					new_params[param_name + "_" + alarm_fields[i] + "_Enable"] = new NvItem(prefix + "Enable", "boolean", NvItem::Read|NvItem::Write, -1, "");
					new_params[param_name + "_" + alarm_fields[i] + "_Set"] = new NvItem(prefix + "Set", "boolean", NvItem::Read, -1, "");
					new_params[param_name + "_" + alarm_fields[i] + "_Ack"] = new NvItem(prefix + "Ack", "boolean", NvItem::Read, -1, "");
					new_params[param_name + "_" + alarm_fields[i] + "_AckType"] = new NvItem(prefix + "AckType", "int32", NvItem::Read|NvItem::Write, -1, "");
					new_params[param_name + "_" + alarm_fields[i] + "_level"] = new NvItem(prefix + "level", "float64", NvItem::Read|NvItem::Write, -1, "");
					new_params[param_name + "_" + alarm_fields[i] + "_deadband"] = new NvItem(prefix + "deadband", "float64", NvItem::Read|NvItem::Write, -1, "");
				}
			}
		}
	}
	m_params.insert(new_params.begin(), new_params.end());
	
	initAsynParamIds();

	// now connect vars
	for(params_t::const_iterator it=m_params.begin(); it != m_params.end(); ++it)
	{
		NvItem* item = it->second;
	    cb_data = new CallbackData(this, item->nv_name, item->id);
		
		std::cerr << "connectVars: connecting to \"" << item->nv_name << "\"" << std::endl;
		
		// create either reader or buffered reader
		if (item->access & NvItem::Read)
		{
	        error = CNVCreateSubscriber(item->nv_name.c_str(), DataCallback, StatusCallback, cb_data, waitTime, 0, &(item->subscriber));
	        ERROR_PRINT_CONTINUE("CNVCreateSubscriber", error);
			readVarInit(item);
		}
		else if (item->access & NvItem::BufferedRead)
		{
	        error = CNVCreateBufferedSubscriber(item->nv_name.c_str(), StatusCallback, cb_data, clientBufferMaxItems, waitTime, 0, &(item->b_subscriber));
	        ERROR_PRINT_CONTINUE("CNVCreateBufferedSubscriber", error);
			readVarInit(item);
		}
		else if (item->access & NvItem::SingleRead)
		{
	        error = CNVCreateReader(item->nv_name.c_str(), StatusCallback, cb_data, waitTime, 0, &(item->reader));
	        ERROR_PRINT_CONTINUE("CNVCreateReader", error);
		}
		// create either writer or buffered writer
		if (item->access & NvItem::Write)
		{
	        error = CNVCreateWriter(item->nv_name.c_str(), StatusCallback, cb_data, waitTime, 0, &(item->writer));
	        ERROR_PRINT_CONTINUE("CNVCreateWriter", error);
		}
		else if (item->access & NvItem::BufferedWrite)
		{
	        error = CNVCreateBufferedWriter(item->nv_name.c_str(), DataTransferredCallback, StatusCallback, cb_data, clientBufferMaxItems, waitTime, 0, &(item->b_writer));
	        ERROR_PRINT_CONTINUE("CNVCreateBufferedWriter", error);
		}
	}
}

/// the quality of the data in a network shared variable
static std::string dataQuality(CNVDataQuality quality)
{
    std::string res;
    char* description = NULL;
    int error = CNVGetDataQualityDescription(quality, ";", &description); 
    if (error == 0)
    {
        res = description;
        CNVFreeMemory(description);
    }
    else
    {
        res = std::string("CNVGetDataQualityDescription: ") + CNVGetErrorDescription(error);
    }
    return res;
}

/// called when data has been transferred to the variable
static void CVICALLBACK DataTransferredCallback(void * handle, int error, void * callbackData)
{
	CallbackData* cb_data = (CallbackData*)callbackData;
	cb_data->intf->dataTransferredCallback(handle, error, cb_data);
}

/// called when data has been transferred to the variable
void NetShrVarInterface::dataTransferredCallback (void * handle, int error, CallbackData* cb_data)
{
	if (error < 0)
	{
		std::cerr << "dataTransferredCallback: \"" << cb_data->nv_name << "\": " << CNVGetErrorDescription(error) << std::endl;
		setParamStatus(cb_data->param_index, asynError);
	}
//	else
//	{
//		std::cerr << "dataTransferredCallback: " << cb_data->nv_name << " OK " << std::endl;
//	}
}

/// called when new data is available on a subscriber connection
static void CVICALLBACK DataCallback (void * handle, CNVData data, void * callbackData)
{
	try
	{
	    CallbackData* cb_data = (CallbackData*)callbackData;
	    cb_data->intf->dataCallback(handle, data, cb_data);
	    CNVDisposeData (data);
	}
	catch(const std::exception& ex)
	{
		std::cerr << "DataCallback: ERROR : " << ex.what() << std::endl; 
	}
	catch(...)
	{
		std::cerr << "DataCallback: ERROR" << std::endl; 
	}	
}

/// called by DataCallback() when new data is available on a subscriber connection
void NetShrVarInterface::dataCallback (void * handle, CNVData data, CallbackData* cb_data)
{
//    std::cerr << "dataCallback: index " << cb_data->param_index << std::endl; 
    try
	{
        updateParamCNV(cb_data->param_index, data, NULL, true);
	}
	catch(const std::exception& ex)
	{
		std::cerr << "dataCallback: ERROR updating param index " << cb_data->param_index << ": " << ex.what() << std::endl; 
	}
	catch(...)
	{
		std::cerr << "dataCallback: ERROR updating param index " << cb_data->param_index << std::endl; 
	}
}

void NetShrVarInterface::updateConnectedAlarmStatus(const std::string& paramName, int value, const std::string& alarmStr, epicsAlarmCondition stat, epicsAlarmSeverity sevr)
{
	asynStatus status;
	int connected_param_index = -1;
	const char *conectedParamName = NULL;
	std::string suffix = "_" + alarmStr + "_Set";
	if ( (paramName.size() > suffix.size()) && (paramName.substr(paramName.size() - suffix.size()) == suffix) )
	{
		std::string connectedParamName = paramName.substr(0, paramName.size() - suffix.size());
	    if (m_driver->findParam(connectedParamName.c_str(), &connected_param_index) == asynSuccess)
		{
			// check if param is in error, if so don't update alarm sttaus
			if ( (m_driver->getParamStatus(connected_param_index, &status) == asynSuccess) && (status == asynSuccess) )
			{
				std::cerr << "Alarm type " << alarmStr << (value != 0 ? " raised" : " cleared") << " for asyn parameter " << connectedParamName << std::endl;
				if (value != 0)
				{
			        setParamStatus(connected_param_index, asynSuccess, stat, sevr);
				}
				else
				{
			        setParamStatus(connected_param_index, asynSuccess);
				}
			}
		}
	}
}	

template<typename T>
void NetShrVarInterface::updateParamValue(int param_index, T val, epicsTimeStamp* epicsTS, bool do_asyn_param_callbacks)
{
	const char *paramName = NULL;
	m_driver->lock();
	m_driver->getParamName(param_index, &paramName);
	m_driver->setTimeStamp(epicsTS);
    m_params[paramName]->epicsTS = *epicsTS;
	if (m_params[paramName]->type == "float64" ||  m_params[paramName]->type == "ftimestamp")
	{
	    m_driver->setDoubleParam(param_index, convertToScalar<double>(val));
	}
	else if (m_params[paramName]->type == "int32" || m_params[paramName]->type == "boolean")
	{
		int intVal = convertToScalar<int>(val);
	    m_driver->setIntegerParam(param_index, intVal);
	    updateConnectedAlarmStatus(paramName, intVal, "Hi", epicsAlarmHigh, epicsSevMinor);
	    updateConnectedAlarmStatus(paramName, intVal, "HiHi", epicsAlarmHiHi, epicsSevMajor);
	    updateConnectedAlarmStatus(paramName, intVal, "Lo", epicsAlarmLow, epicsSevMinor);
	    updateConnectedAlarmStatus(paramName, intVal, "LoLo", epicsAlarmLoLo, epicsSevMajor);
	}
	else if (m_params[paramName]->type == "string" || m_params[paramName]->type == "timestamp")
	{
	    m_driver->setStringParam(param_index, convertToPtr<char>(val));
	}
	else
	{
	    std::cerr << "updateParamValue: unknown type \"" << m_params[paramName]->type << "\" for param \"" << paramName << "\"" << std::endl;
	}
	if (do_asyn_param_callbacks)
	{
		m_driver->callParamCallbacks();
	}
	m_driver->unlock();
}

template<typename T,typename U>
void NetShrVarInterface::updateParamArrayValueImpl(int param_index, T* val, size_t nElements)
{
	const char *paramName = NULL;
	m_driver->getParamName(param_index, &paramName);
	std::vector<char>& array_data =  m_params[paramName]->array_data;
	U* eval = convertToPtr<U>(val);
	if (eval != 0)
	{
		array_data.resize(nElements * sizeof(T));
		memcpy(&(array_data[0]), eval, nElements * sizeof(T));
		(m_driver->*C2CNV<U>::asyn_callback)(reinterpret_cast<U*>(&(array_data[0])), nElements, param_index, 0);
	}
	else
	{
		std::cerr << "updateParamArrayValue: cannot update param \"" << paramName << "\": shared variable data type incompatible \"" << C2CNV<T>::desc << "\"" << std::endl;
	}
}

// labview timestamp is seconds since 01-01-1904 00:00:00
// epics timestamp epoch is seconds since 01-01-1990 00:00:00
static void convertLabviewTimeToEpicsTime(uint64_t* lv_time, epicsTimeStamp* epicsTS)
{
    static const uint64_t epoch_diff = 2713996800u; // seconds from 01-01-1904 to 01-01-1990
    static const uint64_t to_nsec = std::numeric_limits<uint64_t>::max() / 1000000000u;
    epicsTS->secPastEpoch = lv_time[0] - epoch_diff;
    epicsTS->nsec = lv_time[1] / to_nsec;
}

template<typename T>
void NetShrVarInterface::updateParamArrayValue(int param_index, T* val, size_t nElements, epicsTimeStamp* epicsTS, bool do_asyn_param_callbacks)
{
	const char *paramName = NULL;
	m_driver->getParamName(param_index, &paramName);
	m_driver->lock();
	m_driver->setTimeStamp(epicsTS);
	m_params[paramName]->epicsTS = *epicsTS;
	if (m_params[paramName]->type == "float64array")
	{
		updateParamArrayValueImpl<T,epicsFloat64>(param_index, val, nElements);
	}
	else if (m_params[paramName]->type == "float32array")
	{
		updateParamArrayValueImpl<T,epicsFloat32>(param_index, val, nElements);
	}
	else if (m_params[paramName]->type == "int32array")
	{
		updateParamArrayValueImpl<T,epicsInt32>(param_index, val, nElements);
	}
	else if (m_params[paramName]->type == "int16array")
	{
		updateParamArrayValueImpl<T,epicsInt16>(param_index, val, nElements);
	}
	else if (m_params[paramName]->type == "int8array")
	{
		updateParamArrayValueImpl<T,epicsInt8>(param_index, val, nElements);
	}
	else if (m_params[paramName]->type == "timestamp" || m_params[paramName]->type == "ftimestamp") // this is an array of two uint64 elements 
	{
        if ( nElements == 2 && sizeof(T) == sizeof(uint64_t) )
        {
            uint64_t* time_data = reinterpret_cast<uint64_t*>(val);
            convertLabviewTimeToEpicsTime(time_data, epicsTS);
	        // we do not need to call m_driver->setTimeStamp(epicsTS) etc as this is done in updateParamValue
            if (m_params[paramName]->type == "timestamp")
            {                
                char time_buffer[40]; // max size of epics simple string type
                epicsTimeToStrftime(time_buffer, sizeof(time_buffer), "%Y-%m-%dT%H:%M:%S.%06f", epicsTS);
                updateParamValue(param_index, time_buffer, epicsTS, do_asyn_param_callbacks);
            }
            else
            {
                double dval = epicsTS->secPastEpoch + epicsTS->nsec / 1e9;
                updateParamValue(param_index, dval, epicsTS, do_asyn_param_callbacks);
            }
        }
        else
        {
	        std::cerr << "updateParamArrayValue: timestamp param \"" << paramName << "\" not given UInt64[2] array" << std::endl;
        }
	}
	else
	{
	    std::cerr << "updateParamArrayValue: unknown type \"" << m_params[paramName]->type << "\" for param \"" << paramName << "\"" << std::endl;
	}
	m_driver->unlock();
}

/// called externally with m_driver locked
template <typename T> 
void NetShrVarInterface::readArrayValue(const char* paramName, T* value, size_t nElements, size_t* nIn)
{
	NvItem* item = m_params[paramName];
	if (item->access & NvItem::SingleRead)
	{
        ScopedCNVData cvalue;
		if (item->reader != NULL)
		{
			m_driver->unlock(); // to allow DataCallback to work while we try and read
			int status = CNVRead(item->reader, 10, &cvalue);
			m_driver->lock();
			ERROR_CHECK("CNVRead", status);
			if (status > 0) // 0 means no new value, 1 means a new value since last read
			{
				updateParamCNV(item->id, cvalue, NULL, false);  ///< @todo or true?	and set timestamp below?	
			}
		}
		else
		{
			std::cerr << "NetShrVarInterface::readArrayValue: Param \"" << paramName << "\" (" << item->nv_name << ") is not valid" << std::endl;
		}
	}
	std::vector<char>& array_data =  item->array_data;
	size_t n = array_data.size() / sizeof(T);
	if (n > nElements)
	{
	    n = nElements;
	}
	*nIn = n;
	memcpy(value, &(array_data[0]), n * sizeof(T));
	m_driver->setTimeStamp(&(m_params[paramName]->epicsTS));
}

/// read a value and update corresponding asyn parameter
void NetShrVarInterface::readValue(const char* param)
{
	NvItem* item = m_params[param];
	if (item->access & NvItem::SingleRead)
	{
        ScopedCNVData cvalue;
		if (item->reader != NULL)
		{
			m_driver->unlock(); // to allow DataCallback to work while we try and read
			int status = CNVRead(item->reader, 10, &cvalue);
			m_driver->lock();
			ERROR_CHECK("CNVRead", status);
			if (cvalue != 0)
			{
				updateParamCNV(item->id, cvalue, NULL, true);
			}
		}
		else
		{
			std::cerr << "NetShrVarInterface::readValue: Param \"" << param << "\" (" << item->nv_name << ") is not valid" << std::endl;
		}
	}
//	m_driver->setTimeStamp(&(m_params[paramName]->epicsTS)); // don't think this is needed
}

template<CNVDataType cnvType>
void NetShrVarInterface::updateParamCNVImpl(int param_index, CNVData data, CNVDataType type, unsigned int nDims, 
                   epicsTimeStamp* epicsTS, bool do_asyn_param_callbacks)
{
	static const int maxDims = 10;
	if (nDims == 0)
	{
	    typename CNV2C<cnvType>::ctype val;
	    int status = CNVGetScalarDataValue (data, type, &val);
	    ERROR_CHECK("CNVGetScalarDataValue", status);
	    updateParamValue(param_index, val, epicsTS, do_asyn_param_callbacks);
        CNV2C<cnvType>::free(val);
	}
	else if (nDims <= maxDims)
	{
	    typename CNV2C<cnvType>::ctype* val = NULL;
	    size_t dimensions[maxDims];
	    int status = CNVGetArrayDataDimensions(data, nDims, dimensions);
	    ERROR_CHECK("CNVGetArrayDataDimensions", status);
		size_t nElements = 1;
		for(unsigned i=0; i<nDims; ++i)
		{
		    nElements *= dimensions[i];
		}
		if (nElements > 0)
		{
		    val = new typename CNV2C<cnvType>::ctype[nElements];
			if (val != NULL)
			{
		        status = CNVGetArrayDataValue(data, type, val, nElements);
	            ERROR_CHECK("CNVGetArrayDataValue", status);
	            updateParamArrayValue(param_index, val, nElements, epicsTS, do_asyn_param_callbacks);
		        delete[] val;
			}
		}
	}
}

/// convert a timestamp obtained from CNVGetDataUTCTimestamp() into an EPICS timestamp
/// timestamp has 100ns granuality
bool NetShrVarInterface::convertTimeStamp(unsigned __int64 timestamp, epicsTimeStamp *epicsTS)
{
    int year, month, day, hour, minute;
    double second;
    int status = CNVGetTimestampInfo(timestamp, &year, &month, &day, &hour, &minute, &second);
    if (status < 0)
    {
//	std::cerr << "convertTimestamp " << status << ": " << CNVGetErrorDescription(status) << std::endl;
        return false;
    }
	struct tm tms;
	memset(&tms, 0, sizeof(tms));
	tms.tm_year = year - 1900;
	tms.tm_mon = month - 1;
    tms.tm_mday = day;
    tms.tm_hour = hour;
    tms.tm_min = minute;
    tms.tm_sec = static_cast<int>(floor(second));
	unsigned long nanosec = static_cast<unsigned long>(floor((second - floor(second)) * 1.e9 + 0.5));
	epicsTimeFromGMTM(epicsTS, &tms, nanosec);
// debugging check
//	char buffer[60];
//	epicsTimeToStrftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.%06f", epicsTS);
    return true;
}

void NetShrVarInterface::updateParamCNV (int param_index, CNVData data, epicsTimeStamp* epicsTS, bool do_asyn_param_callbacks)
{
	unsigned int	nDims = 0;
	unsigned int	serverError;
	CNVDataType		type;
    CNVDataQuality quality;
	int good, status;
	unsigned short numberOfFields = 0;
	const char *paramName = NULL;
    unsigned __int64 timestamp;
    epicsTimeStamp epicsTSLocal;
	m_driver->getParamName(param_index, &paramName);
	if (paramName == NULL)
	{
		return;
	}
	std::string paramNameStr = paramName;
	if (data == 0)
	{
//        std::cerr << "updateParamCNV: no data for param " << paramName << std::endl;
		return;
    }
	status = CNVGetDataType (data, &type, &nDims);
	ERROR_CHECK("CNVGetDataType", status);
    // the update time for an item in a shared variable structure/cluster is the upadate time of the structure variable
    // so we need to propagate the structure time when we recurse into its fields
    if (epicsTS == NULL)
    {
        const std::string& ts_param = m_params[paramName]->ts_param;
        if (ts_param.size() > 0)
        {
            epicsTS = &(m_params[ts_param]->epicsTS);
        }
        else
        {
            status = CNVGetDataUTCTimestamp(data, &timestamp);
	        ERROR_CHECK("CNVGetDataUTCTimestamp", status);
	        if (!convertTimeStamp(timestamp, &epicsTSLocal))
            {
                epicsTimeGetCurrent(&epicsTSLocal);
            }
            epicsTS = &epicsTSLocal;
        }
    }
	if (type == CNVStruct)
	{
		int field = m_params[paramName]->field;
	    status = CNVGetNumberOfStructFields(data, &numberOfFields);
		ERROR_CHECK("CNVGetNumberOfStructFields", status);
		if (numberOfFields == 0)
		{
			throw std::runtime_error("number of fields");
		}
		if (field < 0 || field >= numberOfFields)
		{
			throw std::runtime_error("field index");
		}
	    CNVData* fields = new CNVData[numberOfFields];
	    status = CNVGetStructFields(data, fields, numberOfFields);
		ERROR_CHECK("CNVGetStructFields", status);
		// loop round all params interested in this structure
		// i.e. not just param_index and field
		const std::string& this_nv = m_params[paramName]->nv_name;
		// do timestamp fields first as we may use them to sync
		std::vector<const NvItem*> items_left;
		items_left.reserve(numberOfFields);
		for (params_t::iterator it = m_params.begin(); it != m_params.end(); ++it)
		{
			NvItem* item = it->second;
			if (item->field != -1 && item->nv_name == this_nv)
			{
				if (item->type == "timestamp" || item->type == "ftimestamp")
				{
					updateParamCNV(item->id, fields[item->field], NULL, do_asyn_param_callbacks);
					epicsTS = &(item->epicsTS); // use timestamp from this record for other items
				}
				else
				{
					items_left.push_back(item);
				}
			}
		}
		for (std::vector<const NvItem*>::const_iterator it = items_left.begin(); it != items_left.end(); ++it)
		{
			const NvItem* item = *it;
			updateParamCNV(item->id, fields[item->field], epicsTS, do_asyn_param_callbacks);
		}
		delete[] fields;
		return;
	}
    status = CNVGetDataQuality(data, &quality);
	ERROR_CHECK("CNVGetDataQuality", status);
    status = CNVCheckDataQuality(quality, &good);
	ERROR_CHECK("CNVCheckDataQuality", status);
	asynStatus p_stat;
	int p_alarmStat, p_alarmSevr;
	getParamStatus(param_index, p_stat, p_alarmStat, p_alarmSevr);
	if (good == 1 && p_stat != asynSuccess)
	{
        std::cerr << "updateParamCNV: data for param " << paramName << " is good quality again" << std::endl;
	    setParamStatus(param_index, asynSuccess);
	}
	// no else here as we don't want to check quality for alarms if good == 0 but do if good == 1
    if (good == 0)
    {
        std::cerr << "updateParamCNV: data for param " << paramName << " is not good quality: " << dataQuality(quality) << std::endl;
	    setParamStatus(param_index, asynError);
    }
	else if ( quality & (CNVDataQualityLowLimited | CNVDataQualityHighLimited) )
	{
		std::cerr << "NV has signaled CNVDataQualityLowLimited / CNVDataQualityHighLimited for " << paramName << std::endl;
		if (p_stat == asynSuccess && p_alarmStat == epicsAlarmNone && p_alarmSevr == epicsSevNone)
		{
	        setParamStatus(param_index, asynSuccess, epicsAlarmHwLimit, epicsSevMinor);
		}
	}
	else if ( quality & CNVDataQualityInAlarm )
	{
		// we should get the EPICS alarm set via our connected alarms
		// we did try alarming here if not otherwise in alarm, but the connected alarms do not repeat
		// so you can get race conditions and conflict especially if you gaev buffered readers for one
		// and readers for the other
		if (!(m_params[paramName]->connected_alarm))
		{
		    if (p_stat == asynSuccess && p_alarmStat == epicsAlarmNone && p_alarmSevr == epicsSevNone)
		    {
				std::cerr << "Unexpected Alarm for " << m_params[paramName]->nv_name << " - Alarming enabled after IOC started?" << std::endl;
 			    std::cerr << "Raising generic HWLIMIT/MINOR Alarm for \"" << paramName << "\"" << std::endl;
 			    std::cerr << "(For more specific HI/LOW etc alarms start this IOC after enabling Alarming)" << std::endl;
	            setParamStatus(param_index, asynSuccess, epicsAlarmHwLimit, epicsSevMinor);
		    }
		}
	}
	else
	{
		// we only clear a hwLimit alarm here, others some as connected alarms
		if (p_stat == asynSuccess && p_alarmStat == epicsAlarmHwLimit)
		{
		    std::cerr << "Clearing HWLIMIT Alarm for \"" << paramName << "\"" << std::endl;
	        setParamStatus(param_index, asynSuccess);
		}
	}
    switch(type)
	{
		case CNVEmpty:
			break;
		
		case CNVBool:
			updateParamCNVImpl<CNVBool>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
			
		case CNVString:
			updateParamCNVImpl<CNVString>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;

		case CNVSingle:
			updateParamCNVImpl<CNVSingle>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		case CNVDouble:
			updateParamCNVImpl<CNVDouble>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		case CNVInt8:
			updateParamCNVImpl<CNVInt8>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		case CNVUInt8:
			updateParamCNVImpl<CNVUInt8>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		case CNVInt16:
			updateParamCNVImpl<CNVInt16>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		case CNVUInt16:
			updateParamCNVImpl<CNVUInt16>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		case CNVInt32:
			updateParamCNVImpl<CNVInt32>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		case CNVUInt32:
			updateParamCNVImpl<CNVUInt32>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		case CNVInt64:
			updateParamCNVImpl<CNVInt64>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		case CNVUInt64:
			updateParamCNVImpl<CNVUInt64>(param_index, data, type, nDims, epicsTS, do_asyn_param_callbacks);
			break;
				
		default:
			std::cerr << "updateParamCNV: unknown type " << type << " for param " << paramName << std::endl;
			break;
	}
	status = CNVGetDataServerError(data, &serverError);
	if (status == 0 && serverError != 0)
	{
	    std::cerr << "updateParamCNV: Server error: " << serverError << std::endl;
	}
	else if (status < 0)
	{
	    std::cerr << "updateParamCNV: CNVGetDataServerError: " << CNVGetErrorDescription(status) << std::endl;
	}
}


/// called when status of a network shared variable changes
static void CVICALLBACK StatusCallback (void * handle, CNVConnectionStatus status, int error, void * callbackData)
{
	CallbackData* cb_data = (CallbackData*)callbackData;
	cb_data->intf->statusCallback(handle, status, error, cb_data);
}

/// called by StatusCallback() when status of a network shared variable changes
void NetShrVarInterface::statusCallback (void * handle, CNVConnectionStatus status, int error, CallbackData* cb_data)
{
	if (error < 0)
	{
		std::cerr << "StatusCallback: " << cb_data->nv_name << ": " << CNVGetErrorDescription(error) << std::endl;
		setParamStatus(cb_data->param_index, asynError);
	}
	else
	{
		std::cerr << "StatusCallback: " << cb_data->nv_name << " is " << connectionStatus(status) << std::endl;
	    if (status != CNVConnected)
	    {
		    setParamStatus(cb_data->param_index, asynDisconnected);
		}
	}
}

static epicsThreadOnceId onceId = EPICS_THREAD_ONCE_INIT;

static void initCV(void*)
{
#ifdef _WIN32
    char* dummy_argv[2] = { strdup("NetShrVarInterface"), NULL };
	if (InitCVIRTE (0, dummy_argv, 0) == 0)
		throw std::runtime_error("InitCVIRTE");
#endif
}

/// expand epics environment strings using previously saved environment  
/// based on EPICS macEnvExpand()
char* NetShrVarInterface::envExpand(const char *str)
{
    long destCapacity = 128;
    char *dest = NULL;
    int n;
    do {
        destCapacity *= 2;
        /*
         * Use free/malloc rather than realloc since there's no need to
         * keep the original contents.
         */
        free(dest);
        dest = static_cast<char*>(mallocMustSucceed(destCapacity, "NetShrVarInterface::envExpand"));
        n = macExpandString(m_mac_env, str, dest, destCapacity);
    } while (n >= (destCapacity - 1));
    if (n < 0) {
        free(dest);
        dest = NULL;
    } else {
        size_t unused = destCapacity - ++n;

        if (unused >= 20)
            dest = static_cast<char*>(realloc(dest, n));
    }
    return dest;
}

/// \param[in] configSection @copydoc initArg1
/// \param[in] configFile @copydoc initArg2
/// \param[in] options @copydoc initArg4
NetShrVarInterface::NetShrVarInterface(const char *configSection, const char* configFile, int options) : 
				m_configSection(configSection), m_options(options), m_mac_env(NULL), 
				m_writer_wait_ms(5000/*also CNVWaitForever or CNVDoNotWait*/), 
				m_b_writer_wait_ms(CNVDoNotWait/*also CNVWaitForever or CNVDoNotWait*/)
{
	epicsThreadOnce(&onceId, initCV, NULL);
	// load current environment into m_mac_env, this is so we can create a macEnvExpand() equivalent 
	// but tied to the environment at a specific time. It is useful if we want to load the same 
	// XML file twice but with a macro defined differently in each case 
	if (macCreateHandle(&m_mac_env, NULL) != 0)
	{
		throw std::runtime_error("Cannot create mac handle");
	}
	for(char** cp = environ; *cp != NULL; ++cp)
	{
		char* str_tmp = strdup(*cp);
		char* equals_loc = strchr(str_tmp, '='); // split   name=value   string
		if (equals_loc != NULL)
		{
		    *equals_loc = '\0';
		    macPutValue(m_mac_env, str_tmp, equals_loc + 1);
		}
		free(str_tmp);
	}
	char* configFile_expanded = envExpand(configFile);
	m_configFile = configFile_expanded;
	epicsAtExit(epicsExitFunc, this);

    pugi::xml_parse_result result = m_xmlconfig.load_file(configFile_expanded);
	free(configFile_expanded);
	if (result)
	{
	    std::cerr << "Loaded XML config file \"" << m_configFile << "\" (expanded from \"" << configFile << "\")" << std::endl;
	}
    else
    {
		throw std::runtime_error("Cannot load XML \"" + m_configFile + "\" (expanded from \"" + std::string(configFile) + "\"): load failure: "
		    + result.description());
    }
}

// need to be careful here as might get called at wrong point. May need to check with driver.
void NetShrVarInterface::epicsExitFunc(void* arg)
{
//	NetShrVarInterface* netvarint = static_cast<NetShrVarInterface*>(arg);
//	if (netvarint == NULL)
//	{
//		return;
//	}
//	if ( netvarint->checkOption(Something) )
//	{
//	}
    CNVFinish();
}

size_t NetShrVarInterface::nParams()
{
	char control_name_xpath[MAX_PATH_LEN];
	epicsSnprintf(control_name_xpath, sizeof(control_name_xpath), "/netvar/section[@name='%s']/param", m_configSection.c_str());
	try
	{
        pugi::xpath_node_set params = m_xmlconfig.select_nodes(control_name_xpath);
        return params.size();
	}
	catch(const std::exception& ex)
	{
	    std::cerr << "nparams failed " << ex.what() << std::endl;
	    return 0;
	}
}

void NetShrVarInterface::initAsynParamIds()
{
    static const char* functionName = "initAsynParamIds";
	for(params_t::iterator it=m_params.begin(); it != m_params.end(); ++it)
	{
		NvItem* item = it->second;
		if (item->id != -1)
		{
			continue; // already initialised
		}
		if (item->type == "float64" || item->type == "ftimestamp")
		{
			m_driver->createParam(it->first.c_str(), asynParamFloat64, &(item->id));
		}
		else if (item->type == "int32" || item->type == "boolean")
		{
			m_driver->createParam(it->first.c_str(), asynParamInt32, &(item->id));
		}
		else if (item->type == "string" || item->type == "timestamp")
		{
			m_driver->createParam(it->first.c_str(), asynParamOctet, &(item->id));
		}
		else if (item->type == "float64array")
		{
			m_driver->createParam(it->first.c_str(), asynParamFloat64Array, &(item->id));
		}
		else if (item->type == "float32array")
		{
			m_driver->createParam(it->first.c_str(), asynParamFloat32Array, &(item->id));
		}
		else if (item->type == "int32array")
		{
			m_driver->createParam(it->first.c_str(), asynParamInt32Array, &(item->id));
		}
		else if (item->type == "int16array")
		{
			m_driver->createParam(it->first.c_str(), asynParamInt16Array, &(item->id));
		}
		else if (item->type == "int8array")
		{
			m_driver->createParam(it->first.c_str(), asynParamInt8Array, &(item->id));
		}
		else
		{
			errlogSevPrintf(errlogMajor, "%s:%s: unknown type %s for parameter %s\n", driverName, 
			                functionName, item->type.c_str(), it->first.c_str());
		}
	}
}

void NetShrVarInterface::createParams(asynPortDriver* driver)
{
    static const char* functionName = "createParams";
    m_driver = driver;
	getParams();
	connectVars();
}

void NetShrVarInterface::getParams()
{
	m_params.clear();
	char control_name_xpath[MAX_PATH_LEN];
	epicsSnprintf(control_name_xpath, sizeof(control_name_xpath), "/netvar/section[@name='%s']/param", m_configSection.c_str());
    pugi::xpath_node_set params;
	try
	{
	    params = m_xmlconfig.select_nodes(control_name_xpath);
	    if (params.size() == 0)
	    {
	        std::cerr << "getParams failed" << std::endl;
		    return;
	    }
	}
	catch(const std::exception& ex)
	{
	    std::cerr << "getParams failed " << ex.what() << std::endl;
		return;
	}
	int field;
	unsigned access_mode;
	char *last_str = NULL;
	char *access_str, *str;
	for (pugi::xpath_node_set::const_iterator it = params.begin(); it != params.end(); ++it)
	{
		pugi::xpath_node node = *it;	
		std::string attr1 = node.node().attribute("name").value();
		std::string attr2 = node.node().attribute("type").value();
		std::string attr3 = node.node().attribute("access").value();
		std::string attr4 = envExpand(node.node().attribute("netvar").value());
		std::string attr5 = node.node().attribute("field").value();	
		std::string attr6 = node.node().attribute("ts_param").value();
		if (attr5.size() == 0)
		{
			field = -1;
		}
		else
		{
			field = atoi(attr5.c_str());
		}
		access_str = strdup(attr3.c_str());
		access_mode = 0;
		str = epicsStrtok_r(access_str, ",", &last_str);
		while( str != NULL )
		{
			if (!strcmp(str, "R"))
			{
				access_mode |= NvItem::Read;
			}
			else if (!strcmp(str, "BR"))
			{
				access_mode |= NvItem::BufferedRead;
			}
			else if (!strcmp(str, "SR"))
			{
				access_mode |= NvItem::SingleRead;
			}
			else if (!strcmp(str, "W"))
			{
				access_mode |= NvItem::Write;
			}
			else if (!strcmp(str, "BW"))
			{
				access_mode |= NvItem::BufferedWrite;
			}
			else
			{
				std::cerr << "getParams: Unknown access mode \"" << str << "\" for param " << attr1 << std::endl;
			}
			str = epicsStrtok_r(NULL, ",", &last_str);
		}
		free(access_str);
		if (attr6.size() > 0 && m_params.find(attr6) == m_params.end())
		{
			std::cerr << "getParams: Unable to link unknown \"" << attr6 << "\" as ts_param for " << attr1 << std::endl;
			attr6 = "";
		}
		m_params[attr1] = new NvItem(attr4.c_str(),attr2.c_str(),access_mode,field,attr6);
	}	
}

template <>
void NetShrVarInterface::setValue(const char* param, const std::string& value)
{
    ScopedCNVData cvalue;
	int status = CNVCreateScalarDataValue(&cvalue, CNVString, value.c_str());
	ERROR_CHECK("CNVCreateScalarDataValue", status);
	setValueCNV(param, cvalue);
}

template <typename T>
void NetShrVarInterface::setValue(const char* param, const T& value)
{
    ScopedCNVData cvalue;
	int status = CNVCreateScalarDataValue(&cvalue, static_cast<CNVDataType>(C2CNV<T>::nvtype), value);
	ERROR_CHECK("CNVCreateScalarDataValue", status);
	setValueCNV(param, cvalue);
}

template <typename T>
void NetShrVarInterface::setArrayValue(const char* param, const T* value, size_t nElements)
{
    ScopedCNVData cvalue;
	size_t dimensions[1] = { nElements };
    int status = CNVCreateArrayDataValue(&cvalue, static_cast<CNVDataType>(C2CNV<T>::nvtype), value, 1, dimensions);
	ERROR_CHECK("CNVCreateArrayDataValue", status);
	setValueCNV(param, cvalue);
}

void NetShrVarInterface::setValueCNV(const std::string& name, CNVData value)
{
	NvItem* item = m_params[name];
	int error = 0;
	if (item->field != -1)
	{
        throw std::runtime_error("setValueCNV: unable to update struct variable via param \"" + name + "\"");
	}
	if (item->access & NvItem::Write)
	{
		m_driver->unlock(); // to allow DataCallback to work while we try and write
	    error = CNVWrite(item->writer, value, m_writer_wait_ms);
		m_driver->lock();
	}
	else if (item->access & NvItem::BufferedWrite)
	{
		m_driver->unlock(); // to allow DataCallback to work while we try and write
	    error = CNVPutDataInBuffer(item->b_writer, value, m_b_writer_wait_ms);
		m_driver->lock();
	}
	else
	{
        throw std::runtime_error("setValueCNV: param \""  + name + "\" does not define a writer for \"" + item->nv_name + "\"");
	}
	ERROR_CHECK("setValue", error);
}

void NetShrVarInterface::setParamStatus(int param_id, asynStatus status, epicsAlarmCondition alarmStat, epicsAlarmSeverity alarmSevr)
{
	m_driver->lock();
	m_driver->setParamStatus(param_id, status);
	m_driver->setParamAlarmStatus(param_id, alarmStat);
	m_driver->setParamAlarmSeverity(param_id, alarmSevr);
	m_driver->unlock();	
}

void NetShrVarInterface::getParamStatus(int param_id, asynStatus& status, int& alarmStat, int& alarmSevr)
{
	m_driver->lock();
	m_driver->getParamStatus(param_id, &status);
	m_driver->getParamAlarmStatus(param_id, &alarmStat);
	m_driver->getParamAlarmSeverity(param_id, &alarmSevr);
	m_driver->unlock();	
}

/// This is called from a polling loop in the driver to 
/// update values from buffered subscribers
void NetShrVarInterface::updateValues()
{
    CNVBufferDataStatus dataStatus;
	int status;
	for(params_t::const_iterator it=m_params.begin(); it != m_params.end(); ++it)
	{
		const NvItem* item = it->second;
		if (item->access & NvItem::Read)
		{
		    ;  // we are a subscriber so automatically get updates on changes
		}
		else if (item->access & NvItem::BufferedRead)
		{
			ScopedCNVData value;
			if (item->b_subscriber != NULL)
			{
				status = CNVGetDataFromBuffer(item->b_subscriber, &value, &dataStatus);
				if (status < 0)
				{
	                std::cerr << NetShrVarException::ni_message("CNVGetDataFromBuffer", status);
					setParamStatus(item->id, asynError);
				}
				if (dataStatus == CNVDataWasLost)
				{
					std::cerr << "NetShrVarInterface::updateValues: BufferedReader: data was lost for param \"" << it->first << "\" (" << item->nv_name << ") - is poll frequency too low?" << std::endl;
					// set an alarm status?
				}
				if (dataStatus == CNVNewData || dataStatus == CNVDataWasLost)  // returns CNVStaleData if value unchanged frm last read
				{
					updateParamCNV(item->id, value, NULL, true);
				}
			}
			else
			{
				std::cerr << "NetShrVarInterface::updateValues: BufferedReader: param \"" << it->first << "\" (" << item->nv_name << ") is not valid" << std::endl;
			}
		}
		else
		{
		    ; // we have not explicitly defined a reader
		}
	}
// we used to pass false to updateParamCNV and do callParamCallbacks here
//	m_driver->lock();
//	m_driver->callParamCallbacks();
//	m_driver->unlock();
}

/// Helper for EPICS driver report function
void NetShrVarInterface::report(FILE* fp, int details)
{
	fprintf(fp, "XML ConfigFile: \"%s\"\n", m_configFile.c_str());
	fprintf(fp, "XML ConfigFile section: \"%s\"\n", m_configSection.c_str());
	fprintf(fp, "NetShrVarConfigure() Options: %d\n", m_options);
	for(params_t::iterator it=m_params.begin(); it != m_params.end(); ++it)
	{
		NvItem* item = it->second;
		item->report(it->first, fp);
	}
}

#ifndef DOXYGEN_SHOULD_SKIP_THIS

template void NetShrVarInterface::setValue(const char* param, const double& value);
template void NetShrVarInterface::setValue(const char* param, const int& value);

template void NetShrVarInterface::setArrayValue(const char* param, const double* value, size_t nElements);
template void NetShrVarInterface::setArrayValue(const char* param, const float* value, size_t nElements);
template void NetShrVarInterface::setArrayValue(const char* param, const int* value, size_t nElements);
template void NetShrVarInterface::setArrayValue(const char* param, const short* value, size_t nElements);
template void NetShrVarInterface::setArrayValue(const char* param, const char* value, size_t nElements);

template void NetShrVarInterface::readArrayValue(const char* paramName, double* value, size_t nElements, size_t* nIn);
template void NetShrVarInterface::readArrayValue(const char* paramName, float* value, size_t nElements, size_t* nIn);
template void NetShrVarInterface::readArrayValue(const char* paramName, int* value, size_t nElements, size_t* nIn);
template void NetShrVarInterface::readArrayValue(const char* paramName, short* value, size_t nElements, size_t* nIn);
template void NetShrVarInterface::readArrayValue(const char* paramName, char* value, size_t nElements, size_t* nIn);

#endif /* DOXYGEN_SHOULD_SKIP_THIS */
