# OpenJDK_PatchRunningCode

Demonstrates a problem with OpenJDK's `NativeJump::patch_verified_entry`.

The problem occurs on specific CPU's such as `Pentium Dual-Core E5200`.

Presumably this is caused by CPU bug AI83 or AI33 from this list:  
http://download.intel.com/design/processor/specupdt/313279.pdf

Reported as https://bugs.openjdk.java.net/browse/JDK-8213961
