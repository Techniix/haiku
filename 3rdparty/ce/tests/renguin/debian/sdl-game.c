/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (F-27, Steam groundwork): a small 64-bit Linux game (a ball
 * and a paddle) on the Debian root's SDL2 and glibc, started outside any
 * Steam client, as the design note's first Steam milestone asks.
 *
 *   sdl-game headless
 *       SDL's dummy video and audio drivers (no display needed): the game
 *       runs 120 frames with a fixed time step, its paddle steered by
 *       keyboard events it posts to itself, renders each frame with SDL's
 *       software renderer and reads the last one back. Also SDL's timer,
 *       threads, audio callback thread and SDL_GetBasePath() (which reads
 *       /proc/self/exe). One line per check, then "ok".
 *   sdl-game window <seconds>
 *       The same game in a window on Renga's Wayland compositor (SDL's
 *       Wayland driver, the renderer on EGL/GLES through Mesa's llvmpipe):
 *       prints "window ok" after its tenth frame, plays for <seconds>,
 *       then "ok". The background is kBackground, so a test can count its
 *       pixels on the screen.
 */
#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kWidth = 320, kHeight = 200, kBall = 8, kPaddleWidth = 40,
	kPaddleHeight = 6, kPaddleY = kHeight - 12, kFrames = 120 };

// the window's background: wayland_test counts it (0x2a 0x9d 0x8f)
static const Uint8 kBackground[3] = { 0x2a, 0x9d, 0x8f };
static const Uint8 kBallColour[3] = { 0xf0, 0xc0, 0x20 };
static const Uint8 kPaddleColour[3] = { 0xe0, 0x30, 0x40 };

struct game {
	int ballX, ballY, speedX, speedY;
	int paddleX;
	int keys;		// keyboard events handled
	int bounces;
};

static int sFailures = 0;

static void
check(const char* name, int pass, const char* detail)
{
	if (pass)
		printf("%s ok\n", name);
	else {
		printf("%s FAIL: %s\n", name, detail);
		sFailures++;
	}
	fflush(stdout);
}

//! One fixed time step: input, then the ball.
static void
step(struct game* game)
{
	// The "player": a key press towards the ball, through SDL's queue.
	int centre = game->paddleX + kPaddleWidth / 2;
	int target = game->ballX + kBall / 2;
	if (centre != target) {
		SDL_Event event;
		memset(&event, 0, sizeof(event));
		event.type = SDL_KEYDOWN;
		event.key.keysym.sym = target < centre ? SDLK_LEFT : SDLK_RIGHT;
		event.key.keysym.scancode = target < centre
			? SDL_SCANCODE_LEFT : SDL_SCANCODE_RIGHT;
		SDL_PushEvent(&event);
	}
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type != SDL_KEYDOWN)
			continue;
		game->keys++;
		if (event.key.keysym.sym == SDLK_LEFT)
			game->paddleX -= 4;
		else if (event.key.keysym.sym == SDLK_RIGHT)
			game->paddleX += 4;
	}
	if (game->paddleX < 0)
		game->paddleX = 0;
	if (game->paddleX > kWidth - kPaddleWidth)
		game->paddleX = kWidth - kPaddleWidth;

	game->ballX += game->speedX;
	game->ballY += game->speedY;
	if (game->ballX < 0 || game->ballX > kWidth - kBall) {
		game->speedX = -game->speedX;
		game->ballX += 2 * game->speedX;
		game->bounces++;
	}
	if (game->ballY < 0 || game->ballY > kPaddleY - kBall) {
		game->speedY = -game->speedY;
		game->ballY += 2 * game->speedY;
		game->bounces++;
	}
}

static void
draw(SDL_Renderer* renderer, const struct game* game)
{
	SDL_SetRenderDrawColor(renderer, kBackground[0], kBackground[1],
		kBackground[2], 255);
	SDL_RenderClear(renderer);
	SDL_Rect ball = { game->ballX, game->ballY, kBall, kBall };
	SDL_SetRenderDrawColor(renderer, kBallColour[0], kBallColour[1],
		kBallColour[2], 255);
	SDL_RenderFillRect(renderer, &ball);
	SDL_Rect paddle = { game->paddleX, kPaddleY, kPaddleWidth, kPaddleHeight };
	SDL_SetRenderDrawColor(renderer, kPaddleColour[0], kPaddleColour[1],
		kPaddleColour[2], 255);
	SDL_RenderFillRect(renderer, &paddle);
	SDL_RenderPresent(renderer);
}

static int
pixel_is(const Uint32* pixels, int x, int y, const Uint8 colour[3])
{
	Uint32 value = pixels[y * kWidth + x];
	return ((value >> 16) & 0xff) == colour[0]
		&& ((value >> 8) & 0xff) == colour[1] && (value & 0xff) == colour[2];
}

static SDL_atomic_t sTicks;
static SDL_atomic_t sAudioCallbacks;

static Uint32
tick(Uint32 interval, void* cookie)
{
	(void)cookie;
	SDL_AtomicAdd(&sTicks, 1);
	return interval;
}

static int
worker(void* data)
{
	int sum = 0;
	for (int i = 1; i <= 100; i++)
		sum += i;
	*(int*)data = sum;
	return 7;
}

static void
audio(void* cookie, Uint8* stream, int length)
{
	(void)cookie;
	memset(stream, 0, length);
	SDL_AtomicAdd(&sAudioCallbacks, 1);
}

static int
headless(void)
{
	char detail[256];
	SDL_setenv("SDL_VIDEODRIVER", "dummy", 0);
	SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER
			| SDL_INIT_EVENTS) != 0) {
		printf("init FAIL: %s\n", SDL_GetError());
		return 1;
	}
	SDL_version version;
	SDL_GetVersion(&version);
	snprintf(detail, sizeof(detail), "%s, SDL %d.%d.%d", SDL_GetPlatform(),
		version.major, version.minor, version.patch);
	check("platform", strcmp(SDL_GetPlatform(), "Linux") == 0
		&& version.major == 2 && version.minor == 32, detail);

	char* base = SDL_GetBasePath();
	char path[1024];
	snprintf(path, sizeof(path), "%ssdl-game", base != NULL ? base : "?/");
	FILE* self = fopen(path, "rb");
	snprintf(detail, sizeof(detail), "base path %s", base != NULL ? base
		: SDL_GetError());
	check("basepath", self != NULL, detail);
	if (self != NULL)
		fclose(self);
	SDL_free(base);

	SDL_Window* window = SDL_CreateWindow("Renguin SDL game", 0, 0, kWidth,
		kHeight, 0);
	SDL_Renderer* renderer = window != NULL
		? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : NULL;
	snprintf(detail, sizeof(detail), "driver %s: %s",
		SDL_GetCurrentVideoDriver(), renderer == NULL ? SDL_GetError() : "");
	check("video", renderer != NULL
		&& strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0, detail);
	if (renderer == NULL)
		return 1;

	struct game game = { 20, 30, 3, 2, 140, 0, 0 };
	for (int frame = 0; frame < kFrames; frame++) {
		step(&game);
		draw(renderer, &game);
	}
	// read back the last frame (the software renderer keeps it)
	draw(renderer, &game);
	Uint32* pixels = malloc(kWidth * kHeight * 4);
	int read = SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
		pixels, kWidth * 4);
	int ballOk = read == 0 && pixel_is(pixels, game.ballX + kBall / 2,
		game.ballY + kBall / 2, kBallColour);
	int paddleOk = read == 0 && pixel_is(pixels,
		game.paddleX + kPaddleWidth / 2, kPaddleY + kPaddleHeight / 2,
		kPaddleColour);
	int background = 0;
	for (int i = 0; read == 0 && i < kWidth * kHeight; i++) {
		if (pixel_is(pixels, i % kWidth, i / kWidth, kBackground))
			background++;
	}
	int wantBackground = kWidth * kHeight - kBall * kBall
		- kPaddleWidth * kPaddleHeight;
	snprintf(detail, sizeof(detail), "read %d, ball (%d,%d) %s, paddle %d %s, "
		"%d background pixels (want %d), %d bounces", read, game.ballX,
		game.ballY, ballOk ? "drawn" : "missing", game.paddleX,
		paddleOk ? "drawn" : "missing", background, wantBackground,
		game.bounces);
	check("render", ballOk && paddleOk && background == wantBackground
		&& game.bounces > 0, detail);
	free(pixels);
	snprintf(detail, sizeof(detail), "%d key events", game.keys);
	check("events", game.keys > 0 && game.keys <= kFrames, detail);

	SDL_TimerID timer = SDL_AddTimer(10, tick, NULL);
	SDL_Delay(300);
	SDL_RemoveTimer(timer);
	snprintf(detail, sizeof(detail), "%d ticks in 300 ms",
		SDL_AtomicGet(&sTicks));
	check("timer", timer != 0 && SDL_AtomicGet(&sTicks) >= 5, detail);

	int sum = 0, status = 0;
	SDL_Thread* thread = SDL_CreateThread(worker, "renguin worker", &sum);
	SDL_WaitThread(thread, &status);
	snprintf(detail, sizeof(detail), "sum %d status %d", sum, status);
	check("thread", thread != NULL && sum == 5050 && status == 7, detail);

	SDL_AudioSpec want, have;
	memset(&want, 0, sizeof(want));
	want.freq = 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = audio;
	SDL_AudioDeviceID device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (device != 0) {
		SDL_PauseAudioDevice(device, 0);
		for (int i = 0; i < 50 && SDL_AtomicGet(&sAudioCallbacks) < 3; i++)
			SDL_Delay(20);
		SDL_CloseAudioDevice(device);
	}
	snprintf(detail, sizeof(detail), "driver %s, device %u, %d callbacks",
		SDL_GetCurrentAudioDriver(), device, SDL_AtomicGet(&sAudioCallbacks));
	check("audio", device != 0 && SDL_AtomicGet(&sAudioCallbacks) >= 3,
		detail);

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	if (sFailures == 0)
		printf("ok\n");
	return sFailures == 0 ? 0 : 1;
}

static int
windowed(int seconds)
{
	SDL_setenv("SDL_VIDEODRIVER", "wayland", 0);
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
		printf("init FAIL: %s\n", SDL_GetError());
		return 1;
	}
	// An OpenGL (EGL) window from the start: a plain window would be
	// recreated for the GLES renderer, and SDL 2.32's Wayland driver
	// crashes in Wayland_DestroyWindow() when such a recreation fails (a
	// NULL window data), which hides the reason.
	SDL_Window* window = SDL_CreateWindow("Renguin SDL game",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, kWidth, kHeight,
		SDL_WINDOW_OPENGL);
	if (window == NULL) {
		printf("window FAIL: driver %s, no window: %s\n",
			SDL_GetCurrentVideoDriver(), SDL_GetError());
		return 1;
	}
	SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
		SDL_RENDERER_ACCELERATED);
	if (renderer == NULL) {
		printf("window FAIL: driver %s, no renderer: %s\n",
			SDL_GetCurrentVideoDriver(), SDL_GetError());
		return 1;
	}
	SDL_RendererInfo info;
	SDL_GetRendererInfo(renderer, &info);
	fprintf(stderr, "sdl-game: video %s, renderer %s\n",
		SDL_GetCurrentVideoDriver(), info.name);

	struct game game = { 20, 30, 3, 2, 140, 0, 0 };
	Uint32 end = SDL_GetTicks() + seconds * 1000;
	int frames = 0;
	while (!SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
		step(&game);
		draw(renderer, &game);
		if (++frames == 10) {
			printf("window ok\n");
			fflush(stdout);
		}
		SDL_Delay(33);
	}
	fprintf(stderr, "sdl-game: %d frames\n", frames);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	printf("ok\n");
	return 0;
}

int
main(int argc, char** argv)
{
	// line by line into a pipe too: a test reads the lines as they come
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc > 1 && strcmp(argv[1], "window") == 0)
		return windowed(argc > 2 ? atoi(argv[2]) : 8);
	return headless();
}
