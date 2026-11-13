/*
 * Copyright 2002, Marcus Overhagen. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_SERVER_H
#define MEDIA_SERVER_H


class AppManager;
class NodeManager;
class BufferManager;
class NotificationManager;
class MediaFilesManager;
class AddOnManager;
class FormatManager;

extern AppManager* gAppManager;
extern NodeManager* gNodeManager;
extern BufferManager* gBufferManager;
extern NotificationManager* gNotificationManager;
extern MediaFilesManager* gMediaFilesManager;
extern AddOnManager* gAddOnManager;
extern FormatManager* gFormatManager;

// Renga (F-59): the audio devices changed; the audio device agent
// (system/servers/audio_device_agent) is started once they settle
#define MEDIA_SERVER_AUDIO_DEVICES_CHANGED	'AdCh'
#define MEDIA_SERVER_AUDIO_DEVICES_SETTLED	'AdSe'

#endif	// MEDIA_SERVER_H
