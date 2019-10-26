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
#include <alarm.h>

#include "asynPortDriver.h"

#include "NetShrVarInterface.h"

int main(int argc, char* argv[])
{
	if (argc > 1)
	{
		std::string var = argv[1];
		if (NetShrVarInterface::pathExists(var))
		{
			std::cout << var << " exists" << std::endl;
		}
		else
		{
			std::cout << var << " does NOT exist" << std::endl;
		}
	}
	return 0;	
}
