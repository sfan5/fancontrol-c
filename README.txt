fancontrol-c
============

This is a port of the "fancontrol" script from lm_sensors to C.
It should function identically with the exception of these missing features:
* It won't warn you about outdated configs properly
* Multiple fans per PWM output are not supported (yet)

Lastly, if you intend to use this on Arch Linux a PKGBUILD is provided in the pkg/ subfolder.

