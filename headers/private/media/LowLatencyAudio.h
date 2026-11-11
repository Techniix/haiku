/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LOW_LATENCY_AUDIO_H
#define _LOW_LATENCY_AUDIO_H


/*!	The low-latency audio mode (roadmap F-31), shared by the system mixer,
	the multi_audio add-on, BSoundPlayer and the desktop's audio controls.

	The design is PipeWire's "quantum": the sound card runs with the
	smallest period any running low-latency stream needs, and the default
	period otherwise.
	- The system mixer keeps a "Low latency" switch per application (a
	  discrete parameter of kind RENGA_LOW_LATENCY_KIND in each stream's
	  group, 0 off, 1 on), remembered by signature like volume and output.
	  A hidden text parameter of kind RENGA_LOW_LATENCY_APPS_KIND lists the
	  signatures switched on, one per line, so that BSoundPlayer can choose
	  its buffer size before it connects.
	- While a low-latency stream is connected, the mixer asks the default
	  output's multi_audio node for kLowLatencyPeriodFrames through the
	  node's hidden parameter of kind RENGA_PERIOD_REQUEST_KIND (0 when no
	  such stream is left). The node sets its device buffers up again and
	  changes the mixer connection's buffer size.
	- The multi_audio node's "Buffer size" parameter
	  (RENGA_BUFFER_FRAMES_KIND) lets the user fix the period; 0
	  ("Automatic") follows the requests.
	- BSoundPlayer, for an application switched on (or run with the
	  environment variable RENGA_AUDIO_LOW_LATENCY=1), uses buffers of one
	  period's duration unless the application asked for a buffer size, and
	  keeps that size for the whole connection.
*/


#define RENGA_LOW_LATENCY_KIND			"renga:low_latency"
#define RENGA_LOW_LATENCY_APPS_KIND		"renga:low_latency_apps"
#define RENGA_PERIOD_REQUEST_KIND		"renga:period_request"
#define RENGA_BUFFER_FRAMES_KIND		"renga:buffer_frames"
#define RENGA_LOW_LATENCY_ENVIRONMENT	"RENGA_AUDIO_LOW_LATENCY"


namespace BPrivate {

//!	The period the low-latency mode asks sound cards for, in frames.
static const int32 kLowLatencyPeriodFrames = 256;

//!	The frame rate kLowLatencyPeriodFrames is meant for (5.33 ms).
static const float kLowLatencyPeriodRate = 48000.0f;

}	// namespace BPrivate


#endif	// _LOW_LATENCY_AUDIO_H
