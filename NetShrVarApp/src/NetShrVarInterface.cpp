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

#include "pugixml.hpp"

#include "asynPortDriver.h"

#include "NetShrVarInterface.h"
#include "cnvconvert.h"

#define MAX_PATH_LEN 256

static const char *driverName="NetShrVarInterface"; ///< Name of driver for use in message printing 

/// An STL exception object encapsulating a shared variable error message
class NetShrVarException : public std::runtime_error
{
public:
	explicit NetShrVarException(const std::string& message) : std::runtime_error(message) { }
	explicit NetShrVarException(const std::string& function, int code) : std::runtime_error(ni_message(function, code)) { }
private:
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
	std::vector<char> array_data; ///< only used for array parameters, contains cached copy of data as this is not stored in usual asyn parameter map
	CNVSubscriber subscriber;
	CNVBufferedSubscriber b_subscriber;
	CNVWriter writer;
	CNVReader reader;
	CNVBufferedWriter b_writer;
	epicsTimeStamp epicsTS; ///< timestamp of shared variable update
	NvItem(const char* nv_name_, const char* type_, unsigned access_, int field_) : nv_name(nv_name_), type(type_), access(access_),
		field(field_), id(-1), subscriber(0), b_subscriber(0), writer(0), b_writer(0), reader(0)
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
		fprintf(fp, "  Connection type: %s", conn_type);
		if (handle == 0)
		{
		    fprintf(fp, " Status: <not being used>\n");
		    return;
		}
		error = CNVGetConnectionAttribute(handle, CNVConnectionStatusAttribute, &status);
		ERROR_CHECK("CNVGetConnectionAttribute", error);
		fprintf(fp, " status: %s", connectionStatus(status));
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
			fprintf(fp, " Client buffer: %d items (buffer size = %d)", nitems, maxitems);
		}
		fprintf(fp, "\n");
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
		std::cerr << "connectVars: Variable engine is not running" << std::endl;
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
	
	for(params_t::const_iterator it=m_params.begin(); it != m_params.end(); ++it)
	{
		NvItem* item = it->second;
	    cb_data = new CallbackData(this, item->nv_name, item->id);
		
		std::cerr << "connectVars: connecting to \"" << item->nv_name << "\"" << std::endl;
#ifdef _WIN32
		// create if not exists??
		int exists = 0;
		size_t proc_pos = item->nv_name.find('\\', 2); // 2 for after \\ in \\localhost\proc\var
		size_t var_pos = item->nv_name.rfind('\\');
		if (proc_pos != std::string::npos && var_pos != std::string::npos)
		{
		    std::string host_name = item->nv_name.substr(2, proc_pos - 2);
			std::string proc_name = item->nv_name.substr(proc_pos + 1, var_pos - proc_pos - 1);
			std::string var_name = item->nv_name.substr(var_pos + 1);
			if (host_name == "localhost") 
			{
		        error = CNVVariableExists(proc_name.c_str(), var_name.c_str(), &exists);
	            ERROR_CHECK("CNVVariableExists", error);
			    if (exists == 0)
			    {
				    std::cerr << "connectVars: process \"" << proc_name << "\" variable \"" << var_name << "\" does not exist on localhost" << std::endl;
			    }
			}
		}
		else
		{
			std::cerr << "connectVars: cannot parse \"" << item->nv_name << "\"" << std::endl;
		}
#endif
		// create either reader or buffered reader
		if (item->access & NvItem::Read)
		{
	        error = CNVCreateSubscriber(item->nv_name.c_str(), DataCallback, StatusCallback, cb_data, waitTime, 0, &(item->subscriber));
	        ERROR_CHECK("CNVCreateSubscriber", error);
		}
		else if (item->access & NvItem::BufferedRead)
		{
	        error = CNVCreateBufferedSubscriber(item->nv_name.c_str(), StatusCallback, cb_data, clientBufferMaxItems, waitTime, 0, &(item->b_subscriber));
	        ERROR_CHECK("CNVCreateBufferedSubscriber", error);
		}
		else if (item->access & NvItem::SingleRead)
		{
	        error = CNVCreateReader(item->nv_name.c_str(), StatusCallback, cb_data, waitTime, 0, &(item->reader));
	        ERROR_CHECK("CNVCreateReader", error);
		}
		// create either writer or buffered writer
		if (item->access & NvItem::Write)
		{
	        error = CNVCreateWriter(item->nv_name.c_str(), StatusCallback, cb_data, waitTime, 0, &(item->writer));
	        ERROR_CHECK("CNVCreateWriter", error);
		}
		else if (item->access & NvItem::BufferedWrite)
		{
	        error = CNVCreateBufferedWriter(item->nv_name.c_str(), DataTransferredCallback, StatusCallback, cb_data, clientBufferMaxItems, waitTime, 0, &(item->b_writer));
	        ERROR_CHECK("CNVCreateBufferedWriter", error);
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
	}
//	else
//	{
//		std::cerr << "dataTransferredCallback: " << cb_data->nv_name << " OK " << std::endl;
//	}
}

/// called when new data is available on a subscriber connection
static void CVICALLBACK DataCallback (void * handle, CNVData data, void * callbackData)
{
	CallbackData* cb_data = (CallbackData*)callbackData;
	cb_data->intf->dataCallback(handle, data, cb_data);
	CNVDisposeData (data);
}

/// called by DataCallback() when new data is available on a subscriber connection
void NetShrVarInterface::dataCallback (void * handle, CNVData data, CallbackData* cb_data)
{
//    std::cerr << "dataCallback: index " << cb_data->param_index << std::endl; 
    try
	{
        updateParamCNV(cb_data->param_index, data, true);
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

template<typename T>
void NetShrVarInterface::updateParamValue(int param_index, T val, epicsTimeStamp* epicsTS, bool do_asyn_param_callbacks)
{
	const char *paramName = NULL;
	m_driver->getParamName(param_index, &paramName);
	m_driver->lock();
	m_driver->setTimeStamp(epicsTS);
	if (m_params[paramName]->type == "float64")
	{
	    m_driver->setDoubleParam(param_index, convertToScalar<double>(val));
	}
	else if (m_params[paramName]->type == "int32" || m_params[paramName]->type == "boolean")
	{
	    m_driver->setIntegerParam(param_index, convertToScalar<int>(val));
	}
	else if (m_params[paramName]->type == "string")
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

template<typename T>
void NetShrVarInterface::updateParamArrayValue(int param_index, T* val, size_t nElements, epicsTimeStamp* epicsTS)
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
		m_driver->unlock(); // to allow DataCallback to work while we try and read
		int status = CNVRead(item->reader, 10, &cvalue);
		m_driver->lock();
		ERROR_CHECK("CNVRead", status);
        if (status > 0)
        {
            updateParamCNV(item->id, cvalue, false);  ///< @todo or true?	and set timestamp below?	
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
		m_driver->unlock(); // to allow DataCallback to work while we try and read
		int status = CNVRead(item->reader, 10, &cvalue);
		m_driver->lock();
		ERROR_CHECK("CNVRead", status);
        if (cvalue != 0)
        {
            updateParamCNV(item->id, cvalue, true);
		}			
	}
//	m_driver->setTimeStamp(&(m_params[paramName]->epicsTS)); // don't think this is needed
}

template<CNVDataType cnvType>
void NetShrVarInterface::updateParamCNVImpl(int param_index, CNVData data, CNVDataType type, unsigned int nDims, bool do_asyn_param_callbacks)
{
    unsigned __int64 timestamp;
	epicsTimeStamp epicsTS;
    int status = CNVGetDataUTCTimestamp(data, &timestamp);
	ERROR_CHECK("CNVGetDataUTCTimestamp", status);
	if (!convertTimeStamp(timestamp, &epicsTS))
    {
            epicsTimeGetCurrent(&epicsTS);
    }
	if (nDims == 0)
	{
	    typename CNV2C<cnvType>::ctype val;
	    int status = CNVGetScalarDataValue (data, type, &val);
	    ERROR_CHECK("CNVGetScalarDataValue", status);
	    updateParamValue(param_index, val, &epicsTS, do_asyn_param_callbacks);
        CNV2C<cnvType>::free(val);
	}
	else
	{
	    typename CNV2C<cnvType>::ctype* val;
	    size_t dimensions[10];
	    int status = CNVGetArrayDataDimensions(data, nDims, dimensions);
	    ERROR_CHECK("CNVGetArrayDataDimensions", status);
		size_t nElements = 1;
		for(unsigned i=0; i<nDims; ++i)
		{
		    nElements *= dimensions[i];
		}
		val = new typename CNV2C<cnvType>::ctype[nElements];
		status = CNVGetArrayDataValue(data, type, val, nElements);
	    ERROR_CHECK("CNVGetArrayDataValue", status);
	    updateParamArrayValue(param_index, val, nElements, &epicsTS);
		delete[] val;
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

void NetShrVarInterface::updateParamCNV (int param_index, CNVData data, bool do_asyn_param_callbacks)
{
	unsigned int	nDims;
	unsigned int	serverError;
	CNVDataType		type;
    CNVDataQuality quality;
	int good, status;
	unsigned short numberOfFields = 0;
	const char *paramName = NULL;
	m_driver->getParamName(param_index, &paramName);
	if (data == 0)
	{
//        std::cerr << "updateParamCNV: no data for param " << paramName << std::endl;
		return;
    }
	status = CNVGetDataType (data, &type, &nDims);
	ERROR_CHECK("CNVGetDataType", status);
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
		updateParamCNV(param_index, fields[field], do_asyn_param_callbacks);
		delete[] fields;
		return;
	}
    status = CNVGetDataQuality(data, &quality);
	ERROR_CHECK("CNVGetDataQuality", status);
    status = CNVCheckDataQuality(quality, &good);
	ERROR_CHECK("CNVCheckDataQuality", status);
    if (good == 0)
    {
        std::cerr << "updateParamCNV: data for param " << paramName << " is not good quality: " << dataQuality(quality) << std::endl;
    }
    switch(type)
	{
		case CNVEmpty:
			break;
		
		case CNVBool:
			updateParamCNVImpl<CNVBool>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;
			
		case CNVString:
			updateParamCNVImpl<CNVString>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;

		case CNVSingle:
			updateParamCNVImpl<CNVSingle>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;
				
		case CNVDouble:
			updateParamCNVImpl<CNVDouble>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;
				
		case CNVInt8:
			updateParamCNVImpl<CNVInt8>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;
				
		case CNVUInt8:
			updateParamCNVImpl<CNVUInt8>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;
				
		case CNVInt16:
			updateParamCNVImpl<CNVInt16>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;
				
		case CNVUInt16:
			updateParamCNVImpl<CNVUInt16>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;
				
		case CNVInt32:
			updateParamCNVImpl<CNVInt32>(param_index, data, type, nDims, do_asyn_param_callbacks);
				break;
				
		case CNVUInt32:
			updateParamCNVImpl<CNVUInt32>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;
				
		case CNVInt64:
			updateParamCNVImpl<CNVInt64>(param_index, data, type, nDims, do_asyn_param_callbacks);
			break;
				
		case CNVUInt64:
			updateParamCNVImpl<CNVUInt64>(param_index, data, type, nDims, do_asyn_param_callbacks);
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
	}
	else
	{
		std::cerr << "StatusCallback: " << cb_data->nv_name << " is " << connectionStatus(status) << std::endl;
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

void NetShrVarInterface::createParams(asynPortDriver* driver)
{
    static const char* functionName = "createParams";
    m_driver = driver;
	getParams();
	for(params_t::iterator it=m_params.begin(); it != m_params.end(); ++it)
	{
		NvItem* item = it->second;
		if (item->type == "float64")
		{
			m_driver->createParam(it->first.c_str(), asynParamFloat64, &(item->id));
		}
		else if (item->type == "int32" || item->type == "boolean")
		{
			m_driver->createParam(it->first.c_str(), asynParamInt32, &(item->id));
		}
		else if (item->type == "string")
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
		m_params[attr1] = new NvItem(attr4.c_str(),attr2.c_str(),access_mode,field);
		
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
			status = CNVGetDataFromBuffer(item->b_subscriber, &value, &dataStatus);
			ERROR_CHECK("CNVGetDataFromBuffer", status); // may throw exception
			if (dataStatus == CNVNewData || dataStatus == CNVDataWasLost)
			{
			    updateParamCNV(item->id, value, true);
			}
			if (dataStatus == CNVDataWasLost)
			{
			    std::cerr << "updateValues: BufferedReader: data was lost for param \"" << it->first << "\" (" << item->nv_name << ") - is poll frequency too low?" << std::endl;
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
