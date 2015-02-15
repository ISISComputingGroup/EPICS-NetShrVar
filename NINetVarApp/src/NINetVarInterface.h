/*************************************************************************\ 
* Copyright (c) 2013 Science and Technology Facilities Council (STFC), GB. 
* All rights reverved. 
* This file is distributed subject to a Software License Agreement found 
* in the file LICENSE.txt that is included with this distribution. 
\*************************************************************************/ 

/// @file NINetVarInterface.h header for #NINetVarInterface class. 
/// @author Freddie Akeroyd, STFC ISIS Facility, GB

#ifndef NINETVAR_INTERFACE_H
#define NINETVAR_INTERFACE_H

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

#include <epicsMutex.h>
#include <epicsThread.h>
#include <epicsExit.h>

#include <msxml2.h>

#include <cvirte.h>		
#include <userint.h>
#include <cvinetv.h>


/// option argument in NINetVarConfigure() of @link st.cmd @endlink not used at present
enum NINetVarOptions { NVNothing = 0, NVSomething=1 };

struct NvItem;
class asynPortDriver;
struct CallbackData;

/// Manager class for the NetVar Interaction. Parses an @link netvarconfig.xml @endlink file and provides access to the 9variables described within. 
class NINetVarInterface
{
public:
	NINetVarInterface(const char* configSection, const char *configFile, int options);
	long nParams();
	~NINetVarInterface() { if (m_pxmldom != NULL) { m_pxmldom->Release(); m_pxmldom = 0; } }
	void updateValues();
	void createParams(asynPortDriver* driver);
	void report(FILE* fp, int details);
	template<typename T> void setValue(const char* param, const T& value);
	template<typename T> void setArrayValue(const char* param, const T* value, size_t nElements);
	void dataTransferredCallback (void * handle, int error, CallbackData* cb_data);
	void dataCallback (void * handle, CNVData data, CallbackData* cb_data);
	void statusCallback (void * handle, CNVConnectionStatus status, int error, CallbackData* cb_data);
	template <typename T> void readArrayValue(const char* paramName, T* value, size_t nElements, size_t* nIn);
    
private:
	std::string m_configSection;  ///< section of \a configFile to load information from
	std::string m_configFile;   
	int m_options; ///< the various #NINetVarOptions currently in use
//	epicsMutex m_lock;
	IXMLDOMDocument2 *m_pxmldom;
	asynPortDriver* m_driver;
	typedef std::map<std::string,NvItem*> params_t;
	params_t m_params;

	void getParams();
	void DomFromCOM();
	void setValueCNV(const std::string& name, CNVData value);
	static void epicsExitFunc(void* arg);
	bool checkOption(NINetVarOptions option) { return ( m_options & static_cast<int>(option) ) != 0; }
	void connectVars();
	template<typename T> void updateParamValue(int param_index, T val, bool do_asyn_param_callbacks);
	template<typename T> void updateParamArrayValue(int param_index, T* val, size_t nElements);
	void updateParamCNV (int param_index, CNVData data, bool do_asyn_param_callbacks);
	template<CNVDataType cnvType> void updateParamCNVImpl(int param_index, CNVData data, CNVDataType type, unsigned int nDims, bool do_asyn_param_callbacks);
	template<typename T,typename U> void updateParamArrayValueImpl(int param_index, T* val, size_t nElements);
};

#endif /* NINETVAR_INTERFACE_H */
