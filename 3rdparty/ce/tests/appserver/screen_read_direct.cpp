/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Helper of screen_read_test (roadmap F-59): a BDirectWindow at the frame
	given on the command line ("left top right bottom") whose content is
	drawn straight into the frame buffer, magenta, while it is connected.
	Prints "screen_read_direct: connected <bits per pixel>" once it drew,
	and runs until it is killed. A program of its own because it needs
	libgame, which the minimum images do not have.
*/


#include <Application.h>
#include <Autolock.h>
#include <DirectWindow.h>
#include <Region.h>

#include <stdio.h>
#include <stdlib.h>


static const uint32 kMagenta = 0xffff00ff;	// B G R A in memory


/*!	Fills its content with magenta, straight into the frame buffer, as long
	as it is connected (a thread repeats it every 20 ms, so app_server
	redrawing the area in between does not matter).
*/
class MagentaWindow : public BDirectWindow {
public:
	MagentaWindow(BRect frame)
		:
		BDirectWindow(frame, "Direct", B_TITLED_WINDOW,
			B_NOT_RESIZABLE | B_NOT_ZOOMABLE),
		fLock("magenta"),
		fConnected(false),
		fQuit(false),
		fDrawCount(0),
		fBitsPerPixel(0)
	{
		fThread = spawn_thread(_DrawThread, "magenta", B_NORMAL_PRIORITY,
			this);
		resume_thread(fThread);
	}

	virtual ~MagentaWindow()
	{
		fQuit = true;
		status_t result;
		wait_for_thread(fThread, &result);
	}

	virtual void DirectConnected(direct_buffer_info* info)
	{
		BAutolock _(fLock);
		switch (info->buffer_state & B_DIRECT_MODE_MASK) {
			case B_DIRECT_START:
			case B_DIRECT_MODIFY:
			{
				fConnected = true;
				fBits = (uint8*)info->bits;
				fBytesPerRow = info->bytes_per_row;
				fBitsPerPixel = info->bits_per_pixel;
				fClip.MakeEmpty();
				for (uint32 i = 0; i < info->clip_list_count; i++)
					fClip.Include(info->clip_list[i]);
				break;
			}
			case B_DIRECT_STOP:
				fConnected = false;
				break;
		}
	}

	int32 DrawCount() { return atomic_get(&fDrawCount); }
	uint32 BitsPerPixel() { BAutolock _(fLock); return fBitsPerPixel; }

private:
	static status_t _DrawThread(void* data)
	{
		MagentaWindow* self = (MagentaWindow*)data;
		while (!self->fQuit) {
			self->_Draw();
			snooze(20000);
		}
		return B_OK;
	}

	void _Draw()
	{
		BAutolock _(fLock);
		if (!fConnected || fBitsPerPixel != 32)
			return;
		for (int32 i = 0; i < fClip.CountRects(); i++) {
			clipping_rect rect = fClip.RectAtInt(i);
			for (int32 y = rect.top; y <= rect.bottom; y++) {
				uint32* row = (uint32*)(fBits + y * fBytesPerRow) + rect.left;
				for (int32 x = rect.left; x <= rect.right; x++)
					*row++ = kMagenta;
			}
		}
		atomic_add(&fDrawCount, 1);
	}

	BLocker			fLock;
	bool			fConnected;
	volatile bool	fQuit;
	int32			fDrawCount;
	uint8*			fBits;
	int32			fBytesPerRow;
	uint32			fBitsPerPixel;
	BRegion			fClip;
	thread_id		fThread;
};



int
main(int argc, char** argv)
{
	if (argc != 5)
		return 1;
	BApplication app("application/x-vnd.Renga-screen-read-direct");
	BRect frame(atof(argv[1]), atof(argv[2]), atof(argv[3]), atof(argv[4]));
	MagentaWindow* window = new MagentaWindow(frame);
	window->Show();
	bool told = false;
	for (;;) {
		snooze(100000);
		if (!told && window->DrawCount() > 0) {
			printf("screen_read_direct: connected %u\n",
				(unsigned)window->BitsPerPixel());
			fflush(stdout);
			told = true;
		}
	}
	return 0;
}
