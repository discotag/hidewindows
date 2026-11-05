/*
	hidewindows.cpp:
	toggle minimize all windows/put back
	g++ hidewindows.cpp -o hidewindows -Os -lbe
	© discotag 2026
*/
#include <Application.h>
#include <Roster.h>
#include <Message.h>
#include <Messenger.h>
#include <List.h>
#include <OS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>

static const char* kStatePath = "/tmp/hidewindows.state";
static const char* kTrackerSig = "application/x-vnd.Be-TRAK";
static const char* kDeskbarSig = "application/x-vnd.Be-TSKB";
static const bigtime_t kTimeout = 300000;

struct HiddenWindow {
	team_id team;
	int32 index;
	std::string title;
};

static status_t QueryWindow(const BMessenger& app, const char* prop, int32 index,
		BMessage* reply) {
	BMessage msg(B_GET_PROPERTY);
	msg.AddSpecifier(prop);
	msg.AddSpecifier("Window", index);
	status_t st = app.SendMessage(&msg, reply, kTimeout, kTimeout);
	if (st != B_OK)	return st;
	int32 error;
	if (reply->FindInt32("error", &error) == B_OK && error != B_OK)	return error;
	return B_OK;
}

static int32 CountWindows(const BMessenger& app) {
	BMessage msg(B_COUNT_PROPERTIES);
	msg.AddSpecifier("Window");
	BMessage reply;
	if (app.SendMessage(&msg, &reply, kTimeout, kTimeout) != B_OK)	return 0;
	int32 error;
	if (reply.FindInt32("error", &error) == B_OK && error != B_OK)	return 0;
	int32 count = 0;
	if (reply.FindInt32("result", &count) != B_OK)	return 0;
	return count;
}

static bool QueryBool(const BMessenger& app, const char* prop, int32 index,
		bool* out) {
	BMessage reply;
	return QueryWindow(app, prop, index, &reply) == B_OK
		&& reply.FindBool("result", out) == B_OK;
}

static bool QueryInt(const BMessenger& app, const char* prop, int32 index,
		int32* out) {
	BMessage reply;
	return QueryWindow(app, prop, index, &reply) == B_OK
		&& reply.FindInt32("result", out) == B_OK;
}

// A Title without tabs or line breaks: stored in a text file.
static bool QueryTitle(const BMessenger& app, int32 index, std::string* out) {
	BMessage reply;
	const char* title;
	if (QueryWindow(app, "Title", index, &reply) != B_OK
		|| reply.FindString("result", &title) != B_OK)
		return false;
	*out = title;
	for (size_t i= 0; i < out->size(); i++) {
		char c= (*out)[i];
		if (c == '\t' || c == '\n' || c == '\r')
			(*out)[i] = ' ';
	}
	return true;
}

static void SetMinimize(const BMessenger& app, int32 index, bool minimize) {
	BMessage msg(B_SET_PROPERTY);
	msg.AddSpecifier("Minimize");
	msg.AddSpecifier("Window", index);
	msg.AddBool("data", minimize);
	BMessage reply;
	app.SendMessage(&msg, &reply, kTimeout, kTimeout);
}

static void HideAll(std::vector<HiddenWindow>* hidden) {
	BList teams;
	be_roster->GetAppList(&teams);

	for (int32 t = 0; t < teams.CountItems(); t++) {
		team_id team = (team_id)(addr_t)teams.ItemAt(t);
		if (team == getpid())	continue;
		app_info info;
		if (be_roster->GetRunningAppInfo(team, &info) != B_OK)
			continue;
		if (strcmp(info.signature, kDeskbarSig) == 0)
			continue;
		bool isTracker = strcmp(info.signature, kTrackerSig) == 0;

		BMessenger app(NULL, team);
		int32 count = CountWindows(app);
		for (int32 i = 0; i < count; i++) {
			bool flag = false;
			if (QueryBool(app, "Minimize", i, &flag) && flag)	continue;
			if (QueryBool(app, "Hidden", i, &flag) && flag)	continue;

			// In Tracker, only folder windows are minimized (Look 11):
			// Desktop (Look 4) and Tracker status window remain in place.
			int32 value;
			if (isTracker) {
				if (!QueryInt(app, "Look", i, &value) || value != 11)
					continue;
			} else if (!QueryInt(app, "Feel", i, &value) || value != 0)
				continue;	// only usual windows

			HiddenWindow w;
			w.team = team;
			w.index = i;
			if (!QueryTitle(app, i, &w.title))
				continue;
			SetMinimize(app, i, true);
			hidden->push_back(w);
		}
	}
}

static void Restore(const std::vector<HiddenWindow>& saved) {
	for (size_t n = 0; n < saved.size(); n++) {
		const HiddenWindow& w = saved[n];
		BMessenger app(NULL, w.team);
		int32 count = CountWindows(app);
		int32 found = -1;
		std::string title;
		bool minimized;

		// The same position of list first, then search by title:
		// numbers shift if windows have been opened or closed.
		if (w.index < count && QueryTitle(app, w.index, &title)
			&& title == w.title
			&& QueryBool(app, "Minimize", w.index, &minimized) && minimized)
			found = w.index;
		else {
			for (int32 i = 0; i < count && found < 0; i++) {
				if (QueryTitle(app, i, &title) && title == w.title
					&& QueryBool(app, "Minimize", i, &minimized) && minimized)
					found = i;
			}
		}
		if (found >= 0)	SetMinimize(app, found, false);
	}
}

static void SaveState(const std::vector<HiddenWindow>& hidden) {
	FILE* f = fopen(kStatePath, "w");
	if (f == NULL)	return;
	for (size_t i = 0; i < hidden.size(); i++) {
		fprintf(f, "%d\t%d\t%s\n", (int)hidden[i].team, (int)hidden[i].index,
			hidden[i].title.c_str());
	}
	fclose(f);
}

// Returns false, if no state file
static bool LoadState(std::vector<HiddenWindow>* saved) {
	FILE* f = fopen(kStatePath, "r");
	if (f == NULL)
		return false;
	char line[1024];
	while (fgets(line, sizeof(line), f) != NULL) {
		int team, index, offset = 0;
		if (sscanf(line, "%d\t%d\t%n", &team, &index, &offset) < 2 || offset == 0)
			continue;
		HiddenWindow w;
		w.team = team;
		w.index = index;
		w.title = line + offset;
		while (!w.title.empty() && (w.title[w.title.size() - 1] == '\n'
				|| w.title[w.title.size() - 1] == '\r'))
			w.title.erase(w.title.size() - 1);
		saved->push_back(w);
	}
	fclose(f);
	return true;
}

int main() {
	BApplication app("application/x-vnd.discotag-hidewindows");

	std::vector<HiddenWindow> saved;
	if (LoadState(&saved)) {
		Restore(saved);
		unlink(kStatePath);
	} else {
		std::vector<HiddenWindow> hidden;
		HideAll(&hidden);
		if (!hidden.empty())	SaveState(hidden);
	}
	return 0;
}
