/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _VIRTUAL_CAMERA_NODE_H
#define _VIRTUAL_CAMERA_NODE_H


#include <BufferProducer.h>
#include <Controllable.h>
#include <Locker.h>
#include <MediaEventLooper.h>


class BBufferGroup;


/*!	The virtual camera (F-58, OBS Studio's O-12; VirtualCamera.h): a raw
	video producer whose frames come from the program that feeds it
	through the socket VIRTUAL_CAMERA_SOCKET and a memfd, scaled to the
	connection's size, at the connection's field_rate.
*/
class VirtualCameraNode : public BBufferProducer, public BMediaEventLooper,
	public BControllable {
public:
								VirtualCameraNode(BMediaAddOn* addOn,
									const char* name, int32 internalID);
	virtual						~VirtualCameraNode();

	// BMediaNode
	virtual	BMediaAddOn*		AddOn(int32* _internalID) const;
	virtual	status_t			HandleMessage(int32 message, const void* data,
									size_t size);
	virtual	void				NodeRegistered();

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
									media_format* format,
									media_source* _source, char* _name);
	virtual	void				Connect(status_t error,
									const media_source& source,
									const media_destination& destination,
									const media_format& format,
									char* ioName);
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

private:
	enum {
		kStatusParameter = 1,
		kModesParameter = 2
	};

	static	status_t			_FrameThreadEntry(void* data);
			void				_FrameThread();
	static	void				_AcquireFeed();
	static	void				_ReleaseFeed();
	static	status_t			_ServerThreadEntry(void* data);
	static	void				_ServerThread();
	static	void				_ServeClient(int client);
	static	void				_DropFeed();
			void				_Fill(uint32* destination, uint32 width,
									uint32 height, bigtime_t* _time);
			void				_Specialize(media_format* format) const;

			BMediaAddOn*		fAddOn;
			int32				fInternalID;

			BLocker				fLock;
			media_output		fOutput;
			bool				fConnected;
			bool				fEnabled;
			bool				fRunning;
			BBufferGroup*		fBufferGroup;
			media_raw_video_format fFormat;	// the connection's

			thread_id			fFrameThread;
			sem_id				fFrameSync;
			volatile bool		fQuitting;
			bigtime_t			fStartTime;		// performance time
			int64				fFrame;

			int64				fFramesSent;
};


#endif	// _VIRTUAL_CAMERA_NODE_H
