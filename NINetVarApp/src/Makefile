TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#==================================================
# build a support library

USR_INCLUDES += -ICVI/include
USR_CXXFLAGS += /EHa

LIBRARY_IOC += NINetVar

# xxxRecord.h will be created from xxxRecord.dbd
#DBDINC += xxxRecord
# install NINetVar.dbd into <top>/dbd
DBD += NINetVar.dbd

# specify all source files to be compiled and added to the library
NINetVar_SRCS += convertToString.cpp cnvconvert.cpp NINetVarDriver.cpp NINetVarInterface.cpp 

NINetVar_LIBS += asyn autosave
NINetVar_LIBS += $(EPICS_BASE_IOC_LIBS)

ifneq ($(findstring windows,$(EPICS_HOST_ARCH)),)
CVILIB = CVI/extlib/msvc64
else
CVILIB = CVI/extlib/msvc
endif
NINetVar_SYS_LIBS_WIN32 += $(CVILIB)/cvinetv $(CVILIB)/cvisupp $(CVILIB)/cvirt

NINetVar_SYS_LIBS_WIN32 += msxml2 comsuppw

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

# we ned to make a copy due to spaces in the absolute path
../cnvconvert.cpp : CVI
CVI :
	-mkdir CVI
	xcopy /D /I /E /Q /Y /R $(NICVIDIR)\include $@\include
	xcopy /D /I /E /Q /Y /R $(NICVIDIR)\extlib $@\extlib

