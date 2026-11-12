/*
 * Copyright 2011, Gabriel Hartmann, gabriel.hartmann@gmail.com.
 * Copyright 2011, Jérôme Duval, korli@users.berlios.de.
 * Copyright 2009, Ithamar Adema, <ithamar.adema@team-embedded.nl>.
 * Distributed under the terms of the MIT License.
 */
#ifndef _UVC_CAM_DEVICE_H
#define _UVC_CAM_DEVICE_H


#include "CamDevice.h"
#include "USB_video.h"
#include <usb/USB_video.h>


class UVCCamDevice : public CamDevice {
public:
								UVCCamDevice(CamDeviceAddon &_addon,
									BUSBDevice* _device);
	virtual						~UVCCamDevice();

	virtual bool				SupportsIsochronous();
	virtual status_t			StartTransfer();
	virtual status_t			StopTransfer();
	virtual status_t			SuggestVideoFrame(uint32 &width,
									uint32 &height);
	virtual status_t			AcceptVideoFrame(uint32 &width,
									uint32 &height);
	virtual status_t			SetFrameRate(float fps);
	virtual float				FrameRate();
	virtual BString				VideoModes();
	virtual void				AddParameters(BParameterGroup *group,
									int32 &index);
	virtual status_t			GetParameterValue(int32 id,
									bigtime_t *last_change, void *value,
									size_t *size);
	virtual status_t			SetParameterValue(int32 id, bigtime_t when,
									const void *value, size_t size);
	virtual status_t			FillFrameBuffer(BBuffer *buffer,
									bigtime_t *stamp = NULL);

private:
			void				_ParseVideoControl(
									const usbvc_class_descriptor* descriptor,
									size_t len);
			void				_ParseVideoStreaming(
									const usbvc_class_descriptor* descriptor,
									size_t len);
			status_t			_ProbeCommitFormat();
			status_t			_SelectBestAlternate();
			status_t			_SelectIdleAlternate();
			status_t			_DecodeMJPEG(const uint8* frame, size_t size,
									uint8* destination, uint32 width,
									uint32 height);
			void 				_DecodeColor(unsigned char *dst,
									unsigned char *src, int32 width,
									int32 height);

			void				_AddProcessingParameter(BParameterGroup* group,
									int32 index,
									const usb_video_processing_unit_descriptor*
										descriptor);
			float				_AddParameter(BParameterGroup* group,
									BParameterGroup** subgroup, int32 index,
									uint16 wValue, const char* name);
			uint8 				_AddAutoParameter(BParameterGroup* subgroup,
									int32 index, uint16 wValue);
			status_t			_SetParameterValue(uint16 wValue,
									int16 setValue);
			status_t			_SetParameterValue(uint16 wValue,
									int8 setValue);


			usbvc_interface_header_descriptor *fHeaderDescriptor;

			const BUSBEndpoint*	fInterruptIn;
			uint32				fControlIndex;
			uint16				fControlRequestIndex;
			uint32				fStreamingIndex;
			uint32				fUncompressedFormatIndex;
			uint32				fUncompressedFrameIndex;
			uint32				fMJPEGFormatIndex;
			uint32				fMJPEGFrameIndex;
			// Renga: the format in use; MJPEG when the size is not
			// offered uncompressed (YUY2), or the camera has no YUY2
			bool				fUseMJPEG;
			bool				fUncompressedIsYUY2;
			uint32				fMaxVideoFrameSize;
			uint32				fMaxPayloadTransferSize;

			BList				fUncompressedFrames;
			BList				fMJPEGFrames;
				// whole VS_FRAME descriptors (malloc()ed, with their
				// frame intervals)
			// Renga (F-58, O-5): the frame rate asked for (0 the default)
			// and the interval of the current frame descriptor chosen for
			// it, sent in the probe and commit
			float				fRequestedFps;
			uint32				fFrameInterval;

			const usb_video_frame_descriptor* _CurrentFrame() const;
			void				_ChooseInterval();

			float				fBrightness;
			float				fContrast;
			float				fHue;
			float				fSaturation;
			float				fSharpness;
			float				fGamma;
			float				fWBTemp;
			float				fWBComponent;
			float				fBacklightCompensation;
			float				fGain;

			bool				fBinaryBacklightCompensation;

			int					fWBTempAuto;
			int					fWBCompAuto;
			int					fHueAuto;
			int					fBacklightCompensationBinary;
			int					fPowerlineFrequency;


};


class UVCCamDeviceAddon : public CamDeviceAddon {
public:
								UVCCamDeviceAddon(WebCamMediaAddOn* webcam);
	virtual 					~UVCCamDeviceAddon();

	virtual const char*			BrandName();
	virtual UVCCamDevice*		Instantiate(CamRoster &roster,
									BUSBDevice *from);
};

#endif /* _UVC_CAM_DEVICE_H */

