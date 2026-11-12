/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _MIXER_TAP_H
#define _MIXER_TAP_H


#include <BufferProducer.h>
#include <Controllable.h>
#include <Locker.h>
#include <MediaEventLooper.h>
#include <Mime.h>
#include <String.h>
#include <StringList.h>


class BTimeSource;
class MixerCore;


/*!	What a loopback records, as the mix thread sees it: a copy taken
	without waiting (MixerTap::GetMixSource()), so nothing here allocates.
	Renga (F-58, OBS Studio's O-1): \a exclude leaves applications out of
	the loopback, for programs that play their own monitoring and record
	"What you hear" at the same time.
*/
struct loopback_selection {
	enum { kMaxExclude = 4096 };

	char		signature[B_MIME_TYPE_LENGTH];
					// "" for every application
	char		exclude[kMaxExclude];
					// signatures left out, each followed by '\n'; "" none

	// Whether the stream of \a signature is left out (case-insensitive,
	// as MIME types compare).
	bool		Excludes(const char* signature) const;
	bool		IsPlain() const
					{ return signature[0] == '\0' && exclude[0] == '\0'; }
};


/*!	A producer node fed by the system mixer's mix thread. Two kinds, in the
	terms PulseAudio and PipeWire use:

	- a sink: the mixer's way to another output device. Streams routed to
	  that device are mixed into a buffer of their own and handed to the
	  sink, which the mixer connects to the device's multi_audio node. The
	  mixer creates and registers sinks itself; they are not add-on flavors.
	- a loopback ("monitor") source: what the mixer plays, as a Media Kit
	  source recording applications connect to like to a sound card's input.
	  It carries either all streams ("what you hear") or only the streams of
	  one application, selected by its signature. Loopbacks are a flavor of
	  the mixer add-on (B_PHYSICAL_INPUT, so recorders list them).

	Push() is called by the mix thread with the mixer core locked. It
	converts the mix (float, interleaved, the mix rate) into the connection's
	format with a stateful linear-interpolation resampler, collects it in a
	FIFO and sends buffers of the negotiated size, so the mix and the
	connection may differ in rate and buffer length, and a change of the mix
	format (the default output changed) needs no renegotiation.
*/
class MixerTap : public BBufferProducer, public BMediaEventLooper,
	public BControllable {
public:
	enum kind_t {
		kSink,
		kLoopback
	};

								MixerTap(BMediaAddOn* addOn, kind_t kind,
									const char* name, int32 flavorID);
	virtual						~MixerTap();

			kind_t				Kind() const { return fKind; }

	// sinks: the device they feed (its flavor name)
			const char*			DeviceName() const
									{ return fDeviceName.String(); }
			void				SetDeviceName(const char* name);
			media_node_id		DeviceNode() const { return fDeviceNode; }
	// Also opens the device's clock (its published time), for the drift
	// correction; not from the mix thread.
			void				SetDeviceNode(media_node_id node);

	// loopbacks: whose streams; empty for all
			BString				SourceSignature();
			void				SetSourceSignature(const char* signature);
	// Loopbacks: applications left out (F-58, O-1), as a newline-separated
	// list of signatures; empty for none. Normalized: one signature per
	// line, each followed by '\n', blanks and empty lines dropped.
			BString				Exclusions();
			void				SetExclusions(const char* list);
	// For the mix thread: copies the selection without waiting; false if
	// the tap is busy.
			bool				GetMixSource(loopback_selection& selection);
	// The applications the mixer has streams of: signature, display name
			void				SetApplications(const BStringList& signatures,
									const BStringList& names);

	// Called by the mix thread, with the core locked. \a data holds
	// \a frames frames of \a channels floats at \a rate, starting at
	// \a startTime (the mixer's performance time).
			void				Push(const float* data, int32 channels,
									int32 frames, float rate,
									bigtime_t startTime,
									media_node_id timeSource, float gain,
									BTimeSource* mixerClock = NULL);
	// Whether Push() can deliver anything; false once sending failed for
	// good (the consumer is gone).
			bool				IsReady();
			bool				IsDead() const { return fDead; }
	// Sinks: not ready before \a when (their device needs a moment after
	// it was started, see AudioMixer::_CreateSink()), and since when the
	// router has found no stream routed to them (0: some are).
			void				SetActiveAt(bigtime_t when)
									{ fActiveAt = when; }
			bigtime_t			UnwantedSince() const
									{ return fUnwantedSince; }
			void				SetUnwantedSince(bigtime_t when)
									{ fUnwantedSince = when; }
	// Blocking versions for the router (not for the mix thread).
			bool				IsConnected();
			bool				GetConnection(media_output& output);

	// BMediaNode
	virtual	BMediaAddOn*		AddOn(int32* _internalID) const;
	virtual	void				NodeRegistered();
	virtual	status_t			HandleMessage(int32 message,
									const void* data, size_t size);
	virtual	void				Start(bigtime_t performanceTime);
	virtual	void				Stop(bigtime_t performanceTime,
									bool immediate);

	// BMediaEventLooper
	virtual	void				HandleEvent(const media_timed_event* event,
									bigtime_t lateness,
									bool realTimeEvent = false);

	// BBufferProducer
	virtual	status_t			FormatSuggestionRequested(media_type type,
									int32 quality, media_format* format);
	virtual	status_t			FormatProposal(const media_source& output,
									media_format* format);
	virtual	status_t			FormatChangeRequested(
									const media_source& source,
									const media_destination& destination,
									media_format* format, int32* _deprecated);
	virtual	status_t			GetNextOutput(int32* cookie,
									media_output* _output);
	virtual	status_t			DisposeOutputCookie(int32 cookie);
	virtual	status_t			SetBufferGroup(const media_source& source,
									BBufferGroup* group);
	virtual	status_t			GetLatency(bigtime_t* _latency);
	virtual	status_t			PrepareToConnect(const media_source& what,
									const media_destination& where,
									media_format* format, media_source* _source,
									char* _name);
	virtual	void				Connect(status_t error,
									const media_source& source,
									const media_destination& destination,
									const media_format& format, char* ioName);
	virtual	void				Disconnect(const media_source& what,
									const media_destination& where);
	virtual	void				LateNoticeReceived(const media_source& what,
									bigtime_t howMuch,
									bigtime_t performanceTime);
	virtual	void				EnableOutput(const media_source& what,
									bool enabled, int32* _deprecated);

	// BControllable
	virtual	status_t			GetParameterValue(int32 id,
									bigtime_t* _lastChange, void* value,
									size_t* _size);
	virtual	void				SetParameterValue(int32 id, bigtime_t when,
									const void* value, size_t size);

	// The parameter ids of a loopback's web (see PORTING.md).
	enum {
		kSourceParameter = 1,		// discrete: 0 all, n: application n
		kSignatureParameter = 2,	// text: signature, "" for all
		kDriftParameter = 3,		// sinks, text (hidden, kind
									// "renga:clock_drift"): "<measured ppm>
									// <applied ppm> <frames in> <frames out>"
		kExcludeParameter = 4		// loopbacks, text (hidden, kind
									// "renga:loopback_exclude"): signatures
									// left out, one per line (F-58, O-1)
	};

private:
			void				_Specialize(media_format* format) const;
			void				_ResetConversion();
			void				_UpdateWeb();
			void				_SendBuffers();
			void				_UpdateDrift(BTimeSource* mixerClock);

			BMediaAddOn*		fAddOn;
			kind_t				fKind;
			int32				fFlavorID;

			BLocker				fLock;
			media_output		fOutput;
			bool				fConnected;
			bool				fEnabled;
			bool				fRunning;
			bool				fDead;
			BBufferGroup*		fBufferGroup;
			bool				fOwnBufferGroup;

			BString				fDeviceName;
			media_node_id		fDeviceNode;
			BString				fSourceSignature;
			BString				fExclusions;	// normalized, see above
			BStringList			fAppSignatures;
			BStringList			fAppNames;
			bigtime_t			fLastChange;

			// conversion state (fLock)
			int32				fChannels;		// of the connection
			float				fRate;
			uint32				fBufferFrames;
			float*				fFifo;			// interleaved, fChannels
			uint32				fFifoFrames;
			uint32				fFifoCapacity;
			bigtime_t			fFifoTime;		// of the first FIFO frame
			double				fPosition;		// resampler position
			float				fLast[2];		// last input frame
			float				fLastRate;
			int32				fSendErrors;
			media_node_id		fPushTimeSource;
			volatile bigtime_t	fActiveAt;
			bigtime_t			fUnwantedSince;

			// clock drift correction (sinks): the device's clock against
			// the mixer's, the latter's frames resampled to the former's
			BTimeSource*		fDeviceClock;
			enum { kDriftSamples = 16 };	// one a second
			bigtime_t			fDriftMixer[kDriftSamples];
			bigtime_t			fDriftDevice[kDriftSamples];
			int32				fDriftCount;
			int32				fDriftNext;
			int32				fDriftRestarts;	// for the test hook
			int32				fDriftRejected;
			double				fDrift;			// device / mixer - 1
			double				fMeasuredDrift;
			bool				fDriftValid;
			int64				fFramesIn;
			int64				fFramesOut;
};


#endif	// _MIXER_TAP_H
