Hwsem Basic Test

 - This test checks the basic api testing of the Hwsem.

Hwsem Shared Peripheral Test

 - This test checks the scenario of sharing peripherals like led between multi-core soc.
It used the default node declared for the led and tries to toggle it, so in case of multiple cores
each core tries to access the led at the same time, while one core has acquired the led other core waits
once the acquired hwsem is released then only other core will be able to acquire it.

