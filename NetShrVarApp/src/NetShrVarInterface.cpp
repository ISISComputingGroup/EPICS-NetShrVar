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
#include <windows.h>

#include <string>
#include <vector>
#include <map>
#include <list>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>

#include <cvirte.h>		
#include <userint.h>
#include <cvinetv.h>

#include <atlbase.h>
#include <comutil.h>

#include <shareLib.h>
#include <macLib.h>
#include <epicsGuard.h>
#include <epicsString.h>
#include <errlog.h>

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
	enum { Read=0x1, Write=0x2, BufferedRead=0x4, BufferedWrite=0x8 } NvAccessMode;   ///< possible access modes to network shared variable
	std::string nv_name;   ///< full path to network shared variable 
	std::string type;   ///< type as specified in the XML file e.g. float64array
	int field; ///< if we refer to a struct, this is the index of the field (starting at 0), otherwise it is -1 
	unsigned access; ///< combination of #NvAccessMode
	int id; ///< asyn parameter id, -1 if not assigned
	std::vector<char> array_data; ///< only used for array parameters, contains cached copy of data as this is not stored in usual asyn parameter map
	CNVSubscriber subscriber;
	CNVBufferedSubscriber b_subscriber;
	CNVWriter writer;
	CNVBufferedWriter b_writer;
	NvItem(const char* nv_name_, const char* type_, unsigned access_, int field_) : nv_name(nv_name_), type(type_), access(access_),
		field(field_), id(-1), subscriber(0), b_subscriber(0), writer(0), b_writer(0)
	{ 
	    std::replace(nv_name.begin(), nv_name.end(), '/', '\\'); // we accept / as well as \ in the XML file for path to variable
	}
	/// helper for asyn driver report function
	void report(const std::string& name, FILE* fp)
	{
	    fprintf(fp, "Report for asyn parameter \"%s\" type \"%s\" network variable \"%s\"\n", name.c_str(), type.c_str(), nv_name.c_str());
		if (array_data.size() > 0)
		{
			fprintf(fp, "  Current array size: %d\n", array_data.size());
		}
		if (field != -1)
		{
			fprintf(fp, "  Network variable structure index: %d\n", field);
		}
	    report(fp, "subscriber", subscriber, false);
	    report(fp, "buffered subscriber", b_subscriber, true);
	    report(fp, "writer", writer, false);
	    report(fp, "buffered writer", b_writer, true);
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
	int error, running;
	CallbackData* cb_data;
	int waitTime = 3000;
	int clientBufferMaxItems = 200;
    error = CNVVariableEngineIsRunning(&running); 
	ERROR_CHECK("CNVVariableEngineIsRunning", error);
    if (running == 0)
    {
		std::cerr << "connectVars: Variable engine is not running" << std::endl;
    }
	for(params_t::const_iterator it=m_params.begin(); it != m_params.end(); ++it)
	{
		NvItem* item = it->second;
	    cb_data = new CallbackData(this, item->nv_name, item->id);
		
		// check variable exists and create if not???
		
		// create either reader or buffered reader
		std::cerr << "connectVars: connecting to \"" << item->nv_name << "\"" << std::endl;
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

/// called when new data is available on a subscriber connection
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
void NetShrVarInterface::updateParamValue(int param_index, T val, bool do_asyn_param_callbacks)
{
	const char *paramName = NULL;
	m_driver->getParamName(param_index, &paramName);
	m_driver->lock();
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
void NetShrVarInterface::updateParamArrayValue(int param_index, T* val, size_t nElements)
{
	const char *paramName = NULL;
	m_driver->getParamName(param_index, &paramName);
	m_driver->lock();
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

template <typename T> 
void NetShrVarInterface::readArrayValue(const char* paramName, T* value, size_t nElements, size_t* nIn)
{
	std::vector<char>& array_data =  m_params[paramName]->array_data;
	size_t n = array_data.size() / sizeof(T);
	if (n > nElements)
	{
	    n = nElements;
	}
	*nIn = n;
	memcpy(value, &(array_data[0]), n * sizeof(T));
}

template<CNVDataType cnvType>
void NetShrVarInterface::updateParamCNVImpl(int param_index, CNVData data, CNVDataType type, unsigned int nDims, bool do_asyn_param_callbacks)
{
	if (nDims == 0)
	{
	    CNV2C<cnvType>::ctype val;
	    int status = CNVGetScalarDataValue (data, type, &val);
	    ERROR_CHECK("CNVGetScalarDataValue", status);
	    updateParamValue(param_index, val, do_asyn_param_callbacks);
        CNV2C<cnvType>::free(val);
	}
	else
	{
	    CNV2C<cnvType>::ctype* val;
	    size_t dimensions[10];
	    int status = CNVGetArrayDataDimensions(data, nDims, dimensions);
	    ERROR_CHECK("CNVGetArrayDataDimensions", status);
		size_t nElements = 1;
		for(unsigned i=0; i<nDims; ++i)
		{
		    nElements *= dimensions[i];
		}
		val = new CNV2C<cnvType>::ctype[nElements];
		status = CNVGetArrayDataValue(data, type, val, nElements);
	    ERROR_CHECK("CNVGetArrayDataValue", status);
	    updateParamArrayValue(param_index, val, nElements);
		delete[] val;
	}
}

void NetShrVarInterface::updateParamCNV (int param_index, CNVData data, bool do_asyn_param_callbacks)
{
	unsigned int	nDims;
	unsigned int	serverError;
	CNVDataType		type;
    CNVDataQuality quality;
    unsigned __int64 timestamp;
    int year, month, day, hour, minute, good;
    double second;
	int status;
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
    status = CNVGetDataUTCTimestamp(data, &timestamp);
	ERROR_CHECK("CNVGetDataUTCTimestamp", status);
    status = CNVGetTimestampInfo(timestamp, &year, &month, &day, &hour, &minute, &second);
	ERROR_CHECK("CNVGetTimestampInfo", status);
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

/// called when status of a network shared variable changes
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
    char* dummy_argv[2] = { "NetShrVarInterface", NULL };
	if (InitCVIRTE (0, dummy_argv, 0) == 0)
		throw std::runtime_error("InitCVIRTE");
}

void NetShrVarInterface::DomFromCOM()
{
	m_pxmldom = NULL;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	HRESULT hr=CoCreateInstance(CLSID_DOMDocument, NULL, CLSCTX_SERVER, IID_IXMLDOMDocument2, (void**)&m_pxmldom);
	if (FAILED(hr))
	{
		throw std::runtime_error("Cannot load DomFromCom");
	}
	if (m_pxmldom != NULL)
	{
		m_pxmldom->put_async(VARIANT_FALSE);
		m_pxmldom->put_validateOnParse(VARIANT_FALSE);
		m_pxmldom->put_resolveExternals(VARIANT_FALSE); 
	}
	else
	{
		throw std::runtime_error("Cannot load DomFromCom");
	}
}

/// \param[in] configSection @copydoc initArg1
/// \param[in] configFile @copydoc initArg2
/// \param[in] options @copydoc initArg4
NetShrVarInterface::NetShrVarInterface(const char *configSection, const char* configFile, int options) : 
				m_configSection(configSection), m_options(options)		
{
	epicsThreadOnce(&onceId, initCV, NULL);
	DomFromCOM();
	short sResult = FALSE;
	char* configFile_expanded = macEnvExpand(configFile);
	m_configFile = configFile_expanded;
	HRESULT hr = m_pxmldom->load(_variant_t(configFile_expanded), &sResult);
	free(configFile_expanded);
	if(FAILED(hr))
	{
		throw std::runtime_error("Cannot load XML \"" + m_configFile + "\" (expanded from \"" + std::string(configFile) + "\"): load failure");
	}
	if (sResult != VARIANT_TRUE)
	{
		throw std::runtime_error("Cannot load XML \"" + m_configFile + "\" (expanded from \"" + std::string(configFile) + "\"): load failure");
	}
	std::cerr << "Loaded XML config file \"" << m_configFile << "\" (expanded from \"" << configFile << "\")" << std::endl;
//	m_extint = doPath("/lvinput/extint/@path").c_str();
	epicsAtExit(epicsExitFunc, this);
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

long NetShrVarInterface::nParams()
{
	long n = 0;
	char control_name_xpath[MAX_PATH_LEN];
	_snprintf(control_name_xpath, sizeof(control_name_xpath), "/netvar/section[@name='%s']/param", m_configSection.c_str());
	IXMLDOMNodeList* pXMLDomNodeList = NULL;
	HRESULT hr = m_pxmldom->selectNodes(_bstr_t(control_name_xpath), &pXMLDomNodeList);
	if (SUCCEEDED(hr) && pXMLDomNodeList != NULL)
	{
		pXMLDomNodeList->get_length(&n);
		pXMLDomNodeList->Release();
	}
	return n;
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
	_snprintf(control_name_xpath, sizeof(control_name_xpath), "/netvar/section[@name='%s']/param", m_configSection.c_str());
	IXMLDOMNodeList* pXMLDomNodeList = NULL;
	HRESULT hr = m_pxmldom->selectNodes(_bstr_t(control_name_xpath), &pXMLDomNodeList);
	if (FAILED(hr) || pXMLDomNodeList == NULL)
	{
	    std::cerr << "getParams failed" << std::endl;
		return;
	}
	IXMLDOMNode *pNode, *pAttrNode1, *pAttrNode2, *pAttrNode3, *pAttrNode4, *pAttrNode5;
	long n = 0;
	int field;
	unsigned access_mode;
	char *last_str = NULL;
	char *access_str, *str;
	pXMLDomNodeList->get_length(&n);
	for(long i=0; i<n; ++i)
	{
		pNode = NULL;
		hr = pXMLDomNodeList->get_item(i, &pNode);
		if (SUCCEEDED(hr) && pNode != NULL)
		{
			IXMLDOMNamedNodeMap *attributeMap = NULL;
			pAttrNode1 = pAttrNode2 = pAttrNode3 = pAttrNode4 = pAttrNode5 = NULL;
			pNode->get_attributes(&attributeMap);
			hr = attributeMap->getNamedItem(_bstr_t("name"), &pAttrNode1);
			hr = attributeMap->getNamedItem(_bstr_t("type"), &pAttrNode2);
			hr = attributeMap->getNamedItem(_bstr_t("access"), &pAttrNode3);
			hr = attributeMap->getNamedItem(_bstr_t("netvar"), &pAttrNode4);
			hr = attributeMap->getNamedItem(_bstr_t("field"), &pAttrNode5);
			BSTR bstrValue1 = NULL, bstrValue2 = NULL, bstrValue3 = NULL, bstrValue4 = NULL, bstrValue5 = NULL;
			hr=pAttrNode1->get_text(&bstrValue1);
			hr=pAttrNode2->get_text(&bstrValue2);
			hr=pAttrNode3->get_text(&bstrValue3);
			hr=pAttrNode4->get_text(&bstrValue4);
			if (pAttrNode5 != NULL)
			{
			    hr=pAttrNode5->get_text(&bstrValue5);
				field = atoi(COLE2CT(bstrValue5));
			}
			else
			{
			    field = -1;
			}
			access_str = strdup(COLE2CT(bstrValue3));
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
				    std::cerr << "getParams: Unknown access mode \"" << str << "\" for param " << COLE2CT(bstrValue1) << std::endl;
				}
			    str = epicsStrtok_r(NULL, ",", &last_str);
			}
			free(access_str);
			m_params[std::string(COLE2CT(bstrValue1))] = new NvItem(COLE2CT(bstrValue4),COLE2CT(bstrValue2),access_mode,field);
			SysFreeString(bstrValue1);
			SysFreeString(bstrValue2);
			SysFreeString(bstrValue3);
			SysFreeString(bstrValue4);
			if (bstrValue5 != NULL)
			{
			    SysFreeString(bstrValue5);
			}
			pAttrNode1->Release();
			pAttrNode2->Release();
			pAttrNode3->Release();
			pAttrNode4->Release();
			if (pAttrNode5 != NULL)
			{
			    pAttrNode5->Release();
			}
			attributeMap->Release();
			pNode->Release();
		}
	}	
	pXMLDomNodeList->Release();
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
    int wait_ms = CNVWaitForever/*CNVDoNotWait*/, b_wait_ms = CNVDoNotWait/*CNVWaitForever*/;
	NvItem* item = m_params[name];
	int error = 0;
	if (item->field != -1)
	{
        throw std::runtime_error("setValueCNV: unable to update struct variable via param \"" + name + "\"");
	}
	if (item->access & NvItem::Write)
	{
	    error = CNVWrite(item->writer, value, wait_ms);
	}
	else if (item->access & NvItem::BufferedWrite)
	{
	    error = CNVPutDataInBuffer(item->b_writer, value, b_wait_ms);
	}
	else
	{
        throw std::runtime_error("setValueCNV: param \""  + name + "\" does not define a writer for \"" + item->nv_name + "\"");
	}
	ERROR_CHECK("setValue", error);
}

// update values from buffered subscribers
void NetShrVarInterface::updateValues()
{
    CNVBufferDataStatus dataStatus;
	int status;
	m_driver->lock();
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
			ERROR_CHECK("CNVGetDataFromBuffer", status);
			if (dataStatus == CNVNewData || dataStatus == CNVDataWasLost)
			{
			    updateParamCNV(item->id, value, false);
			}
			if (dataStatus == CNVDataWasLost)
			{
			    std::cerr << "updateValues: data was lost for param \"" << it->first << "\" (" << item->nv_name << ")" << std::endl;
			}
		}
		else
		{
		    ; // we have not explicitly defined a reader
		}
	}
	m_driver->callParamCallbacks();
	m_driver->unlock();
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
