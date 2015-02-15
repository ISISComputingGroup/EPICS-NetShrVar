TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#=============================
# Build the IOC application TestNINetVar
# We actually use $(APPNAME) below so this file can be included by multiple IOCs

PROD_IOC = $(APPNAME)
# TestNINetVar.dbd will be created and installed
DBD += $(APPNAME).dbd

# TestNINetVar.dbd will be made up from these files:
$(APPNAME)_DBD += base.dbd
$(APPNAME)_DBD += NINetVar.dbd

# Add all the support libraries needed by this IOC
$(APPNAME)_LIBS += NINetVar
$(APPNAME)_LIBS += asyn
$(APPNAME)_LIBS += autosave
#$(APPNAME)_LIBS += pugixml

# TestNINetVar_registerRecordDeviceDriver.cpp derives from TestNINetVar.dbd
$(APPNAME)_SRCS += $(APPNAME)_registerRecordDeviceDriver.cpp

# Build the main IOC entry point on workstation OSs.
$(APPNAME)_SRCS_DEFAULT += $(APPNAME)Main.cpp
$(APPNAME)_SRCS_vxWorks += -nil-

# Add support from base/src/vxWorks if needed
#$(APPNAME)_OBJS_vxWorks += $(EPICS_BASE_BIN)/vxComLibrary

# Finally link to the EPICS Base libraries
$(APPNAME)_LIBS += $(EPICS_BASE_IOC_LIBS)

$(APPNAME)_SYS_LIBS_WIN32 += msxml2 comsuppw

ifneq ($(findstring windows,$(EPICS_HOST_ARCH)),)
CVILIB = $(TOP)/NINetVarApp/src/O.$(EPICS_HOST_ARCH)/CVI/extlib/msvc64
else
CVILIB = $(TOP)/NINetVarApp/src/O.$(EPICS_HOST_ARCH)/CVI/extlib/msvc
endif
$(APPNAME)_SYS_LIBS_WIN32 += $(CVILIB)/cvinetv $(CVILIB)/cvisupp $(CVILIB)/cvirt

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE
