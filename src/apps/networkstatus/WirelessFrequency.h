/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef WIRELESS_FREQUENCY_H
#define WIRELESS_FREQUENCY_H


#include <SupportDefs.h>


//!	The centre frequency (MHz) of a wireless interface's channel, or 0.
uint32 wireless_frequency(const char* interfaceName);


#endif	// WIRELESS_FREQUENCY_H
