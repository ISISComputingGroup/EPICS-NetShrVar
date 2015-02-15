## @file st.cmd IOC startup file

## if you are using the NiNetVar binary distribution you may need to manually edit "envPaths"
< envPaths

cd ${TOP}

## Register all support components
dbLoadDatabase "dbd/TestNINetVar.dbd"
TestNINetVar_registerRecordDeviceDriver pdbbase

## main args are:  portName, configSection, configFile, pollPeriod, options (see NINetVarConfigure() documentation in NINetVarDriver.cpp)
##
## portName ("nsv" below) refers to the asyn driver port name - it is the external name used in epics DB files to refer to the driver instance
## configSection ("sec1" below) refers to the section of configFile ("netvarconfig.xml" below) where settings are read from
## configFile is the path to the main configuration file (netvarconfig.xml)
## pollPeriod (100) is the interval (ms) at which the driver will pull values from variables accessed via a BufferedReader connection 
## options (0 below) is currently unused but would map to values in #NINetVarOptions    
NINetVarConfigure("nsv", "sec1", "$(TOP)/TestNINetVarApp/src/netvarconfig.xml", 100, 0)

## Load our record instances
dbLoadRecords("db/TestNINetVar.db","P=TEST:")

cd ${TOP}/iocBoot/${IOC}
iocInit
