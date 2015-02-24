TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#==================================================
# build a support library

USR_INCLUDES += -ICVI/include
USR_CXXFLAGS_Linux += -std=c++11

LIBRARY_IOC += NetShrVar

# xxxRecord.h will be created from xxxRecord.dbd
#DBDINC += xxxRecord
# install NetShrVar.dbd into <top>/dbd
DBD += NetShrVar.dbd

# specify all source files to be compiled and added to the library
NetShrVar_SRCS += convertToString.cpp cnvconvert.cpp NetShrVarDriver.cpp NetShrVarInterface.cpp pugixml.cpp

NetShrVar_LIBS += asyn autosave
NetShrVar_LIBS += $(EPICS_BASE_IOC_LIBS)

ifneq ($(findstring windows,$(EPICS_HOST_ARCH)),)
CVILIB = CVI/extlib/msvc64
else
CVILIB = CVI/extlib/msvc
endif
NetShrVar_SYS_LIBS_WIN32 += $(CVILIB)/cvinetv $(CVILIB)/cvisupp $(CVILIB)/cvirt 

NetShrVar_SYS_LIBS_WIN32 += user32

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

# we ned to make a copy due to spaces in the absolute path
../cnvconvert.cpp : CVI
CVI :
	-mkdir CVI
ifeq (WIN32,$(OS_CLASS))
	xcopy /D /I /E /Q /Y /R $(NICVIDIR)\include $@\include
	xcopy /D /I /E /Q /Y /R $(NICVIDIR)\extlib $@\extlib
endif
ifeq (Linux,$(OS_CLASS))
	cp -r $(NICVIDIR)/include $@
	cp -r $(NICVIDIR)/extlib $@
endif

