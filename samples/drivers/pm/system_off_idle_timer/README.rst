.. _alif-system-off-sample-using-systick-idle-time:

Alif System Off demo using Systick Idle Timer
#############################################

Supported Targets
-----------------
- alif_e3_dk_rtss_he

Overview
********

This sample can be used for basic power measurement and as an example of
subsystem off of RTSS cores in Alif SoC. The functional behavior is:

* Display the last reset/wakeup reason
* Set RUN profile for the subsystem (sets Clock source, power domains, CPU freq etc)
* Set OFF profile for the subsystem (sets VTOR/RTC wakeup event/Retention blocks/Power Domains)
* Set k_sleep of 10sec(DEEP_SLEEP_IN_MSEC) so that it can go to subsystem off
* For the SoC to transition to global states(IDLE/STANDBY/STOP), it requires voting
  from all the remaining subsystem in the SoC
* Subsystem reboots once the RTC interrupt triggers and the above steps continue

Note:
*****
* This application runs from ITCM by default. To run from the MRAM, set CONFIG_XIP=y
  in the prj.conf.
* User should use the SETOOLS package which can be downloaded from our website
  for flashing the binaries to MRAM. See :ref:`programming_an_application` for more information.
* If using a USB hub to connect the UART, it is advised to set the
  BOOT_DELAY to make sure UART logs are not missed in the PC after powercycle.
* Debugger should be disconnected while performing this test. If connected,
  it will prevent the core to go to OFF state.
