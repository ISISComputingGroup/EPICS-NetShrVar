/*************************************************************************\ 
* Copyright (c) 2013 Science and Technology Facilities Council (STFC), GB. 
* All rights reverved. 
* This file is distributed subject to a Software License Agreement found 
* in the file LICENSE.txt that is included with this distribution. 
\*************************************************************************/ 

/// @file convertToString.cpp Templated number to string conversion functions.
/// @author Freddie Akeroyd, STFC ISIS Facility, GB

#include <string>
#include <cstdio>
#include "convertToString.h"

#ifdef _WIN32
#define snprintf _snprintf
#endif /* _WIN32 */

/// Convert a numeric type to a string.
template<typename T>
std::string convertToString(T t)
{
	should_never_get_called(t);
	return "";
}

template<>
std::string convertToString(double t)
{
	char buffer[30];
	snprintf(buffer, sizeof(buffer), "%f", t);
	return buffer;
}

template<>
std::string convertToString(int t)
{
	char buffer[30];
	snprintf(buffer, sizeof(buffer), "%d", t);
	return buffer;
}
