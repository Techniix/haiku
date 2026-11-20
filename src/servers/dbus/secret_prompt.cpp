/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	session_bus_prompt: the Secret Service's approval question, as a program
	of its own. session_bus is a background application, whose windows do
	not come to the front and get no keyboard focus; a program started for
	the question is a normal application that does (as ScreenRecording's
	prompt is). session_bus starts it and reads the answer from its exit
	status, so no other program can answer for the user through it.

	session_bus_prompt <program path>
	Exit status: 0 Deny (also Escape and closing), 1 Allow this time,
	2 Always allow. Return means Deny.
*/


#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Invoker.h>
#include <Messenger.h>
#include <String.h>

#include <stdio.h>


class PromptApplication : public BApplication {
public:
	PromptApplication()
		:
		BApplication("application/x-vnd.Renga-SessionBusPrompt"),
		fAnswer(0)
	{
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		if (argc >= 2)
			fProgram = argv[1];
	}

	virtual void ReadyToRun()
	{
		BString name = fProgram;
		int32 slash = name.FindLast('/');
		if (slash >= 0)
			name.Remove(0, slash + 1);
		BString text;
		text << "\"" << name << "\" wants to use your saved passwords "
			"(collection \"Login\").\n\n" << fProgram << "\n\n"
			"Allow it only if you started this program and trust it.";

		BAlert* alert = new BAlert("Saved passwords", text.String(), "Deny",
			"Allow this time", "Always allow", B_WIDTH_AS_USUAL,
			B_OFFSET_SPACING, B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		alert->SetShortcut(1, 't');
		alert->SetShortcut(2, 'a');
		alert->SetDefaultButton(alert->ButtonAt(0));
		BMessenger alertMessenger(alert);
		alert->Go(new BInvoker(new BMessage('answ'), this));
		BMessage activate(B_SET_PROPERTY);
		activate.AddSpecifier("Active");
		activate.AddBool("data", true);
		alertMessenger.SendMessage(&activate);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != 'answ') {
			BApplication::MessageReceived(message);
			return;
		}
		fAnswer = message->GetInt32("which", 0);
		if (fAnswer < 0 || fAnswer > 2)
			fAnswer = 0;
		PostMessage(B_QUIT_REQUESTED);
	}

	int32 Answer() const { return fAnswer; }

private:
	BString	fProgram;
	int32	fAnswer;
};


int
main()
{
	PromptApplication application;
	application.Run();
	return application.Answer();
}
