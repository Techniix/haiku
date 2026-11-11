/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Clock-drift correction between two sound cards (roadmap F-31 phase 1):
	a stream routed to the second card goes through a mixer sink, which
	compares that card's clock with the mixer's (the default output's) and
	resamples the mix so that the card gets as many frames as its own clock
	plays. QEMU's two virtual cards do not keep the same pace: each is
	paced by its own audio back end on the host, and the second one
	measured 4800 to 8000 ppm slow against the first, wandering by about
	1000 ppm between 16 s windows. The test therefore makes the second
	card's clock run fast by a known amount with multi_audio's test hook
	(the hidden "renga:clock_skew_ppm" parameter skews the time the node
	publishes) and checks the change, and reads the sink's estimate and
	what it produced from its hidden "renga:clock_drift" parameter
	("<measured ppm> <applied ppm> <frames in> <frames out> <samples>
	<restarts> <rejected>").

	Checks: without skew the estimate settles (a full window, no restarts);
	+6000 ppm of skew moves it by 6000 +- 1500 ppm; the sink produces as
	many more (or fewer) frames than the mixer hands it as the estimate
	says, within 400 ppm; the skew back at 0 brings the estimate back to
	where it was, within 1500 ppm. The card's sound itself cannot show the
	correction in QEMU; that needs two cards with their own crystals.
*/


#include <Application.h>
#include <MediaRoster.h>
#include <OS.h>
#include <ParameterWeb.h>
#include <String.h>

#include <AudioStreamControl.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define SUITE "media_drift"

using BPrivate::AudioStreamControl;

static const char* kPlayer = "/boot/system/non-packaged/bin/ce-tests-media/"
	"media_test";
static const char* kSignature = "application/x-vnd.Renga-drift-player";
static const int32 kSkew = 6000;
static const double kWander = 1500;
	// QEMU's cards against each other, see above

static int sFailures = 0;


static void
info(const char* format, ...)
{
	char text[500];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	printf("CE-INFO " SUITE ": %s\n", text);
	fflush(stdout);
}


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


//!	Finds a parameter of \a kind of \a node; calls \a access on it.
template<typename Access>
static status_t
with_parameter(const media_node& node, const char* kind, Access access)
{
	BParameterWeb* web;
	status_t status = BMediaRoster::Roster()->GetParameterWebFor(node, &web);
	if (status != B_OK)
		return status;
	status = B_ENTRY_NOT_FOUND;
	for (int32 i = 0; i < web->CountParameters(); i++) {
		BParameter* parameter = web->ParameterAt(i);
		if (strcmp(parameter->Kind(), kind) == 0) {
			status = access(parameter);
			break;
		}
	}
	delete web;
	return status;
}


static status_t
node_named(const char* name, media_node* node, uint64 kinds)
{
	live_node_info infos[16];
	int32 count = 16;
	status_t status = BMediaRoster::Roster()->GetLiveNodes(infos, &count,
		NULL, NULL, NULL, kinds);
	if (status != B_OK)
		return status;
	for (int32 i = 0; i < count; i++) {
		if (strcmp(infos[i].name, name) == 0) {
			*node = infos[i].node;
			return B_OK;
		}
	}
	return B_ENTRY_NOT_FOUND;
}


struct Drift {
	double	measured;
	double	applied;
	int64	in;
	int64	out;
	int32	samples;
	int32	restarts;
	int32	rejected;
};


static status_t
read_drift(const media_node& sink, Drift* drift)
{
	return with_parameter(sink, "renga:clock_drift",
		[drift](BParameter* parameter) -> status_t {
			char text[128];
			size_t size = sizeof(text);
			bigtime_t when;
			status_t status = parameter->GetValue(text, &size, &when);
			if (status != B_OK)
				return status;
			text[min_c(size, sizeof(text) - 1)] = '\0';
			drift->samples = drift->restarts = drift->rejected = -1;
			return sscanf(text, "%lf %lf %" B_SCNd64 " %" B_SCNd64
					" %" B_SCNd32 " %" B_SCNd32 " %" B_SCNd32,
				&drift->measured, &drift->applied, &drift->in, &drift->out,
				&drift->samples, &drift->restarts, &drift->rejected)
					>= 4 ? B_OK : B_BAD_DATA;
		});
}



//!	Waits \a seconds, logging the sink's estimate every 3 s.
static void
watch(const media_node& sink, int seconds, const char* phase)
{
	for (int elapsed = 0; elapsed < seconds; elapsed += 3) {
		snooze(3000000);
		Drift drift;
		if (read_drift(sink, &drift) == B_OK) {
			info("%s +%d s: measured %.0f ppm, applied %.0f ppm, %" B_PRId32
				" samples, %" B_PRId32 " restarts, %" B_PRId32 " rejected",
				phase, elapsed + 3, drift.measured, drift.applied,
				drift.samples, drift.restarts, drift.rejected);
		}
	}
}

static status_t
set_skew(const media_node& card, int32 ppm)
{
	return with_parameter(card, "renga:clock_skew_ppm",
		[ppm](BParameter* parameter) -> status_t {
			int32 value = ppm;
			return parameter->SetValue(&value, sizeof(value), system_time());
		});
}


int
main()
{
	BApplication app("application/x-vnd.Renga-media-drift-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	// the media services were just started: wait for the mixer
	for (int i = 0; i < 120; i++) {
		media_node mixer;
		BMediaRoster* roster = BMediaRoster::Roster();
		if (roster != NULL && roster->GetAudioMixer(&mixer) == B_OK) {
			roster->ReleaseNode(mixer);
			break;
		}
		snooze(500000);
	}
	snooze(3000000);

	// a player routed to the second card (a copy of media_test's player)
	BString command;
	command << "cp " << kPlayer << " /tmp/DriftPlayer && /tmp/DriftPlayer "
		"player " << kSignature;
	FILE* player = popen(command.String(), "w");
	if (player != NULL) {
		fprintf(player, "tone 900 900\n");
		fflush(player);
	}
	AudioStreamControl control;
	const BPrivate::AudioStream* stream = NULL;
	for (int i = 0; i < 100 && stream == NULL; i++) {
		snooze(200000);
		if (control.Update() == B_OK)
			stream = control.FindStream(kSignature);
	}
	BString second;
	for (int32 i = 1; i < control.Outputs().CountStrings(); i++) {
		if (control.Outputs().StringAt(i).EndsWith(" 2"))
			second = control.Outputs().StringAt(i);
	}
	status_t status = stream != NULL && second.Length() > 0
		? control.SetOutputByName(stream->id, second.String())
		: B_ENTRY_NOT_FOUND;

	BString sinkName("Mixer sink: ");
	sinkName << second;
	media_node sink;
	media_node card;
	status_t found = B_ENTRY_NOT_FOUND;
	for (int i = 0; i < 50 && found != B_OK; i++) {
		snooze(200000);
		found = node_named(sinkName.String(), &sink, B_BUFFER_PRODUCER);
	}
	status_t cardFound = node_named(second.String(), &card,
		B_BUFFER_CONSUMER);
	report("sink", status == B_OK && found == B_OK && cardFound == B_OK,
		"routed to \"%s\": %s; sink node %s, card node %s", second.String(),
		strerror(status), strerror(found), strerror(cardFound));
	if (found != B_OK || cardFound != B_OK) {
		if (player != NULL) {
			fprintf(player, "quit\n");
			pclose(player);
		}
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	// the shared host clock: no drift (the estimate fits a line through
	// the last 16 s, one sample a second)
	watch(sink, 18, "no skew");
	Drift before;
	status = read_drift(sink, &before);
	info("the second card against the first without skew: %.0f ppm",
		before.measured);
	report("estimate", status == B_OK && before.samples == 16
			&& before.restarts == 0 && fabs(before.applied - before.measured)
				< 1,
		"%s; measured %.0f ppm, applied %.0f ppm, %" B_PRId32 " samples, %"
		B_PRId32 " restarts, %" B_PRId32 " rejected (want a full window of "
		"16, no restarts, the estimate applied)", strerror(status),
		before.measured, before.applied, before.samples, before.restarts,
		before.rejected);

	// the second card's clock kSkew ppm faster
	status = set_skew(card, kSkew);
	watch(sink, 18, "skew");
	Drift start;
	read_drift(sink, &start);
	watch(sink, 9, "skew, counting");
	Drift fast;
	status_t read = read_drift(sink, &fast);
	double change = fast.measured - before.measured;
	report("skew_measured", status == B_OK && read == B_OK
			&& fabs(change - kSkew) < kWander,
		"skew set: %s; measured %.0f ppm, %+.0f ppm from before (want %+"
		B_PRId32 " +- %.0f)", strerror(status), fast.measured, change, kSkew,
		kWander);
	double ratio = fast.in > start.in
		? (double)(fast.out - start.out) / (fast.in - start.in) : 0;
	double average = (start.applied + fast.applied) / 2;
	report("skew_corrected", read == B_OK && fast.out > start.out
			&& fabs((ratio - 1) * 1e6 - average) < 400,
		"over 9 s the sink made %" B_PRId64 " frames of %" B_PRId64
		" (%+.0f ppm; want the applied correction, %+.0f ppm on average, "
		"+- 400; same rates on both sides)", fast.out - start.out,
		fast.in - start.in, (ratio - 1) * 1e6, average);

	set_skew(card, 0);
	watch(sink, 18, "skew removed");
	Drift after;
	read = read_drift(sink, &after);
	report("skew_removed", read == B_OK
			&& fabs(after.measured - before.measured) < kWander,
		"skew back at 0: measured %.0f ppm, %+.0f ppm from before the skew "
		"(want 0 +- %.0f)", after.measured, after.measured - before.measured,
		kWander);

	if (control.Update() == B_OK && (stream = control.FindStream(kSignature))
			!= NULL) {
		control.SetOutput(stream->id, 0);
	}
	if (player != NULL) {
		fprintf(player, "quit\n");
		fflush(player);
		pclose(player);
	}

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
