.. _alif-system-off-sample:

Alif System Off Demo
#####################

Supported Targets
-----------------
- alif_e3_dk_rtss_he
- alif_e7_dk_rtss_he
- alif_e1c_dk_rtss_he
- alif_b1_dk_rtss_he

Overview
********

This sample can be used for basic power measurement and as an example of
subsystem off of RTSS cores in Alif SoC. The functional behavior is:

* Set RUN profile for the subsystem (sets Clock source, power domains, CPU freq etc)
* Display the last reset/wakeup reason
* Set OFF profile for the subsystem (sets VTOR/RTC wakeup event/Retention blocks/Power Domains)
* Sleep for 3secs(NORMAL_SLEEP_IN_USEC) so that system goes to idle task(Normal sleep)
* After waking up, again sleep for 20sec(DEEP_SLEEP_IN_USEC), which will make sure the subsystem
  goes to OFF state
* For the SoC to transition to global states(IDLE/STANDBY/STOP), it requires voting
  from all the remaining subsystem in the SoC
* Subsystem reboots once the wakeup interrupt triggers and the above steps continue

Note:
*****
* This application runs from ITCM by default. To run from the MRAM, set CONFIG_XIP=y
  in the prj.conf.
* CONFIG_CORTEX_M_SYSTICK_IDLE_TIMER is set by default. RTC0 will be used as the idle timer when
  it goes to subsystem Off state. User may disable this along with updating the 'min-residency-us'
  in the overlay file with a higher value so that the subsystem won't go to OFF state unless
  requested using pm_state_force or sys_poweroff.
* User should use the SETOOLS package which can be downloaded from our website
  for flashing the binaries to MRAM. See :ref:`programming_an_application` for more information.
* If using a USB hub to connect the UART, it is advised to set the
  BOOT_DELAY to make sure UART logs are not missed in the PC after powercycle.
* Debugger should be disconnected while performing this test. If connected,
  it will prevent the core to go to OFF state.
