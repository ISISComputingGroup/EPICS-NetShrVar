/*************************************************************************\ 
* Copyright (c) 2013 Science and Technology Facilities Council (STFC), GB. 
* All rights reverved. 
* This file is distributed subject to a Software License Agreement found 
* in the file LICENSE.txt that is included with this distribution. 
\*************************************************************************/ 

/// @file cnvconvert.cpp Network shared variable type convertion routine.
/// @author Freddie Akeroyd, STFC ISIS Facility, GB

#include <stdexcept>

#include <cvirte.h>		
#include <userint.h>
#include <cvinetv.h>

#include <asynPortDriver.h>

#include "cnvconvert.h"

const char* CNV2C<CNVBool>::desc = "bool";
const char* CNV2C<CNVString>::desc = "char*";
const char* CNV2C<CNVSingle>::desc = "float";
const char* CNV2C<CNVDouble>::desc = "double";
const char* CNV2C<CNVInt8>::desc = "char";
const char* CNV2C<CNVUInt8>::desc = "unsigned char";
const char* CNV2C<CNVInt16>::desc = "short";
const char* CNV2C<CNVUInt16>::desc = "unsigned short";
const char* CNV2C<CNVInt32>::desc = "int";
const char* CNV2C<CNVUInt32>::desc = "unsigned int";
const char* CNV2C<CNVInt64>::desc = "__int64";
const char* CNV2C<CNVUInt64>::desc = "unsigned __int64";

const char* C2CNV<bool>::desc = "CNVBool";
const char* C2CNV<char*>::desc = "CNVString";
const char* C2CNV<float>::desc = "CNVSingle";
const char* C2CNV<double>::desc = "CNVDouble";
const char* C2CNV<char>::desc = "CNVInt8";
const char* C2CNV<unsigned char>::desc = "CNVUInt8";
const char* C2CNV<short>::desc = "CNVInt16";
const char* C2CNV<unsigned short>::desc = "CNVUInt16";
const char* C2CNV<int>::desc = "CNVInt32";
const char* C2CNV<unsigned int>::desc = "CNVUInt32";
const char* C2CNV<__int64>::desc = "CNVInt64";
const char* C2CNV<unsigned __int64>::desc = "CNVUInt64";

// asyn callbacks take signed types only
asynStatus (asynPortDriver::*C2CNV<double>::asyn_callback)(double* value, size_t nElements, int reason, int addr) = &asynPortDriver::doCallbacksFloat64Array;
asynStatus (asynPortDriver::*C2CNV<float>::asyn_callback)(float* value, size_t nElements, int reason, int addr) = &asynPortDriver::doCallbacksFloat32Array;
asynStatus (asynPortDriver::*C2CNV<int>::asyn_callback)(int* value, size_t nElements, int reason, int addr) = &asynPortDriver::doCallbacksInt32Array;
asynStatus (asynPortDriver::*C2CNV<short>::asyn_callback)(short* value, size_t nElements, int reason, int addr) = &asynPortDriver::doCallbacksInt16Array;
asynStatus (asynPortDriver::*C2CNV<char>::asyn_callback)(char* value, size_t nElements, int reason, int addr) = &asynPortDriver::doCallbacksInt8Array;
asynStatus (asynPortDriver::*C2CNV<unsigned int>::asyn_callback)(int* value, size_t nElements, int reason, int addr) = &asynPortDriver::doCallbacksInt32Array;
asynStatus (asynPortDriver::*C2CNV<unsigned short>::asyn_callback)(short* value, size_t nElements, int reason, int addr) = &asynPortDriver::doCallbacksInt16Array;
asynStatus (asynPortDriver::*C2CNV<unsigned char>::asyn_callback)(char* value, size_t nElements, int reason, int addr) = &asynPortDriver::doCallbacksInt8Array;
asynStatus (asynPortDriver::*C2CNV<bool>::asyn_callback)(char* value, size_t nElements, int reason, int addr) = &asynPortDriver::doCallbacksInt8Array;
