/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _CAPTURE_SESSION_H
#define _CAPTURE_SESSION_H


/*!	Screen and audio capture for recording and streaming applications
	(Renga, F-31 phase 3). See 3rdparty/ce/PORTING.md, "Screen capture".

	CaptureSession combines
	- video: the screen, one window or a region, through app_server
	  (BPrivate::BScreenCapture), sampled at a steady frame rate;
	- system audio: everything the system mixer plays ("What you hear") or
	  one application's streams, through the mixer's loopback (phase 1);
	- the microphone: the default audio input;
	all on one clock, system_time(). It either hands frames and audio to a
	Listener (streaming, a port's own encoder) or writes a file through the
	Media Kit's encoders (BMediaFile; today the FFmpeg plug-in: Matroska
	with MPEG-4 video and 16-bit PCM audio, or what CaptureOptions names).

	ScreenCaptureNode is the same video as a Media Kit node: a
	B_BUFFER_PRODUCER of B_RGB32 raw video whose buffers carry the
	performance time each frame was taken at, for applications that build
	Media Kit graphs (CodyCam-like previews, a consumer that encodes).
*/


#include <BufferProducer.h>
#include <Entry.h>
#include <Locker.h>
#include <MediaEventLooper.h>
#include <Region.h>
#include <String.h>

#include <ScreenCaptureDefs.h>


class BBitmap;
class BBufferGroup;
class BMediaFile;
class BMediaRecorder;
class BMediaTrack;


namespace BPrivate {


class BScreenCapture;


struct CaptureOptions {
	int32		source;			// SCREEN_CAPTURE_SCREEN, _WINDOW, _REGION
	int32		windowToken;	// for SCREEN_CAPTURE_WINDOW (server token)
	BRect		region;			// for SCREEN_CAPTURE_REGION
	uint32		flags;			// SCREEN_CAPTURE_INCLUDE_CURSOR, ...
	float		frameRate;		// frames per second, 1-60
	bool		systemAudio;	// record what the system plays
	BString		audioApplication;
								// with systemAudio: only this signature's
								// streams (empty: all)
	bool		microphone;		// record the default audio input
	BString		fileFormat;		// short name, e.g. "mkv" (default), "avi"
	BString		videoCodec;		// short name, e.g. "mpeg4" (default)
	BString		audioCodec;		// short name, e.g. "pcm" (default)
	bigtime_t	permissionTimeout;

				CaptureOptions();
};


class CaptureSession {
public:
	class Listener {
	public:
		virtual					~Listener();
		// Called from the session's video thread, frameRate times a
		// second; the bitmap stays valid until the call returns. damage
		// is empty if nothing changed since the previous frame.
		virtual	void			VideoFrame(const BBitmap* frame,
									const BRegion& damage,
									bigtime_t time);
		// Called from the session's video thread with the audio due by
		// now: interleaved stereo, 16-bit, 48000 frames a second, system
		// audio and microphone mixed; time is when the first frame was
		// recorded.
		virtual	void			AudioData(const int16* samples,
									size_t frames, bigtime_t time);
	};

								CaptureSession();
								~CaptureSession();

			// Without a path: frames and audio go to the listener only.
			status_t			Start(const CaptureOptions& options,
									const entry_ref* file,
									Listener* listener = NULL);
			status_t			Stop();
				// writes the rest and closes the file

			bool				IsRunning() const { return fRunning; }
			BRect				Frame() const { return fFrame; }
			bigtime_t			StartTime() const { return fStartTime; }
			bigtime_t			Duration() const;
			int64				FramesWritten() const
									{ return fFramesWritten; }
			int64				AudioFramesWritten() const
									{ return fAudioFramesWritten; }
			status_t			Error() const { return fError; }
				// the first error while running (B_CANCELED: the user
				// stopped the capture)

	// What the Media Kit can write today: Matroska + MPEG-4 + PCM, as the
	// file format and codecs CaptureOptions defaults to.
	static	status_t			CheckEncoders(const CaptureOptions& options,
									BString* description = NULL);

private:
			struct AudioSource;

			status_t			_OpenFile(const entry_ref* file);
			status_t			_StartAudio(AudioSource* source,
									bool microphone);
			void				_StopAudio(AudioSource* source);
	static	void				_RecordHook(void* cookie, bigtime_t time,
									void* data, size_t size,
									const media_format& format);
	static	status_t			_VideoThreadEntry(void* data);
			void				_VideoThread();
			void				_WriteAudio(bool final);

			CaptureOptions		fOptions;
			BScreenCapture*		fCapture;
			BBitmap*			fBitmap;
			BRect				fFrame;
			Listener*			fListener;
			BMediaFile*			fFile;
			BMediaTrack*		fVideoTrack;
			BMediaTrack*		fAudioTrack;
			AudioSource*		fSystem;
			AudioSource*		fMicrophone;
			thread_id			fThread;
	volatile bool				fRunning;
	volatile bool				fQuit;
			bigtime_t			fStartTime;
			bigtime_t			fStopTime;
			int64				fFramesWritten;
			int64				fAudioFramesWritten;
			status_t			fError;
};


class ScreenCaptureNode : public virtual BMediaEventLooper,
	public virtual BBufferProducer {
public:
								ScreenCaptureNode(const char* name,
									const CaptureOptions& options);
	virtual						~ScreenCaptureNode();

			status_t			InitCheck() const { return fInitStatus; }
				// B_NOT_ALLOWED if the user said no

	// BMediaNode
	virtual	BMediaAddOn*		AddOn(int32* _internalID) const;
	virtual	status_t			HandleMessage(int32 message, const void* data,
									size_t size);

protected:
	virtual	void				NodeRegistered();
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

private:
	static	status_t			_ThreadEntry(void* data);
			void				_Thread();

			status_t			fInitStatus;
			CaptureOptions		fOptions;
			BScreenCapture*		fCapture;
			BBitmap*			fBitmap;
			media_output		fOutput;
			BBufferGroup*		fBufferGroup;
			BLocker				fLock;
			thread_id			fThread;
	volatile bool				fQuit;
			bool				fConnected;
			bool				fEnabled;
			bigtime_t			fStartPerformanceTime;
			int64				fFrame;
};


}	// namespace BPrivate


#endif	// _CAPTURE_SESSION_H
