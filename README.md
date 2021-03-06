An EPICS support module to access [National Instruments Network Shared Variables](http://www.ni.com/white-paper/5484/en/) and expose them as EPICS process variables. The module can be run on either Windows or Linux operating systems. Shared variables are available in LabVIEW and some National Instruments hardware. The IOC provided works very much like the [NI EPICS I/O server](http://www.ni.com/white-paper/14144/en/) which is built upon the Shared Variable Engine (SVE) - the main difference is that with *NetShrVar* you have a standard EPICS IOC and thus full access to EPICS database fields and logic, you also do not require the Datalogging and Supervisory Control (DSC) module.

For full documentation see the [ISIS EPICS Homepage](http://epics.isis.stfc.ac.uk/)

See [README.build](https://github.com/ISISComputingGroup/EPICS-NetShrVar/blob/master/README.build) for brief building instructions
