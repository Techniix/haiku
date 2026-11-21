/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "MenuSearch.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <Catalog.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>
#include <Query.h>
#include <Roster.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include <ExpressionParser.h>
#include <control_panel_index.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "MenuSearch"


static const int32 kMaxApplications = 12;
static const int32 kMaxSettings = 8;
static const int32 kMaxRecent = 6;
static const int32 kMaxMenuEntries = 2000;
	// a runaway user menu folder cannot make every search slow


// #pragma mark - units


enum unit_category {
	kLength,
	kMass,
	kVolume,
	kTime,
	kSpeed,
	kData,
	kArea,
	kTemperature
};


struct Unit {
	const char*		names;
		// space separated, the first is shown; matched without case
		// except where kCaseSensitive is set
	unit_category	category;
	double			factor;
		// to the category's base unit; temperatures: see convert_value()
	bool			caseSensitive;
};


static const Unit kUnits[] = {
	{ "m meter meters metre metres", kLength, 1, false },
	{ "mm millimeter millimeters", kLength, 0.001, false },
	{ "cm centimeter centimeters", kLength, 0.01, false },
	{ "km kilometer kilometers kilometre kilometres", kLength, 1000, false },
	{ "in inch inches", kLength, 0.0254, false },
	{ "ft foot feet", kLength, 0.3048, false },
	{ "yd yard yards", kLength, 0.9144, false },
	{ "mi mile miles", kLength, 1609.344, false },
	{ "nmi", kLength, 1852, false },

	{ "kg kilogram kilograms kilo kilos", kMass, 1, false },
	{ "g gram grams", kMass, 0.001, false },
	{ "mg milligram milligrams", kMass, 0.000001, false },
	{ "t tonne tonnes", kMass, 1000, false },
	{ "oz ounce ounces", kMass, 0.028349523125, false },
	{ "lb lbs pound pounds", kMass, 0.45359237, false },
	{ "st stone stones", kMass, 6.35029318, false },

	{ "l liter liters litre litres", kVolume, 1, false },
	{ "ml milliliter milliliters millilitre millilitres", kVolume, 0.001,
		false },
	{ "gal gallon gallons", kVolume, 3.785411784, false },
	{ "qt quart quarts", kVolume, 0.946352946, false },
	{ "pt pint pints", kVolume, 0.473176473, false },
	{ "cup cups", kVolume, 0.2365882365, false },
	{ "floz", kVolume, 0.0295735295625, false },
	{ "m3", kVolume, 1000, false },

	{ "s sec second seconds", kTime, 1, false },
	{ "ms millisecond milliseconds", kTime, 0.001, false },
	{ "min minute minutes", kTime, 60, false },
	{ "h hr hour hours", kTime, 3600, false },
	{ "d day days", kTime, 86400, false },
	{ "week weeks", kTime, 604800, false },

	{ "m/s", kSpeed, 1, false },
	{ "km/h kmh kph", kSpeed, 1 / 3.6, false },
	{ "mph", kSpeed, 0.44704, false },
	{ "kn knot knots", kSpeed, 0.514444444, false },

	{ "B byte bytes", kData, 1, true },
	{ "b bit bits", kData, 0.125, true },
	{ "KB kB kb", kData, 1000, true },
	{ "MB mb", kData, 1e6, true },
	{ "GB gb", kData, 1e9, true },
	{ "TB tb", kData, 1e12, true },
	{ "KiB kib", kData, 1024, true },
	{ "MiB mib", kData, 1048576, true },
	{ "GiB gib", kData, 1073741824.0, true },
	{ "TiB tib", kData, 1099511627776.0, true },

	{ "m2 sqm", kArea, 1, false },
	{ "km2", kArea, 1e6, false },
	{ "ha hectare hectares", kArea, 1e4, false },
	{ "acre acres", kArea, 4046.8564224, false },
	{ "ft2 sqft", kArea, 0.09290304, false },

	{ "C celsius °C", kTemperature, 0, false },
	{ "F fahrenheit °F", kTemperature, 1, false },
	{ "K kelvin", kTemperature, 2, false },
};


static bool
word_in_list(const char* list, const char* word, bool caseSensitive)
{
	size_t length = strlen(word);
	const char* p = list;
	while (*p != '\0') {
		const char* end = strchr(p, ' ');
		size_t itemLength = end != NULL ? (size_t)(end - p) : strlen(p);
		if (itemLength == length && (caseSensitive
				? strncmp(p, word, length) == 0
				: strncasecmp(p, word, length) == 0)) {
			return true;
		}
		if (end == NULL)
			break;
		p = end + 1;
	}
	return false;
}


static const Unit*
find_unit(const char* word, const Unit* sameCategoryAs = NULL)
{
	// Case-sensitive names first ("b" is a bit, "B" a byte), then the rest
	// without case.
	for (int pass = 0; pass < 2; pass++) {
		for (size_t i = 0; i < sizeof(kUnits) / sizeof(kUnits[0]); i++) {
			const Unit& unit = kUnits[i];
			if (sameCategoryAs != NULL
				&& unit.category != sameCategoryAs->category) {
				continue;
			}
			bool caseSensitive = pass == 0;
			if (caseSensitive != unit.caseSensitive)
				continue;
			if (word_in_list(unit.names, word, caseSensitive))
				return &unit;
		}
	}
	return NULL;
}


static double
convert_value(double value, const Unit* from, const Unit* to)
{
	if (from->category != kTemperature)
		return value * from->factor / to->factor;

	// through Celsius
	double celsius = value;
	if (from->factor == 1)
		celsius = (value - 32) * 5 / 9;
	else if (from->factor == 2)
		celsius = value - 273.15;
	if (to->factor == 1)
		return celsius * 9 / 5 + 32;
	if (to->factor == 2)
		return celsius + 273.15;
	return celsius;
}


static BString
unit_label(const Unit* unit)
{
	const char* end = strchr(unit->names, ' ');
	BString label;
	label.SetTo(unit->names, end != NULL ? end - unit->names
		: (int32)strlen(unit->names));
	if (unit->category == kTemperature)
		label.Prepend("°");
	return label;
}


static BString
format_number(double value)
{
	BString text;
	if (value != 0 && (fabs(value) >= 1e15 || fabs(value) < 1e-6))
		text.SetToFormat("%.6g", value);
	else {
		text.SetToFormat("%.6f", value);
		// remove surplus zeros
		if (text.FindFirst('.') >= 0) {
			int32 length = text.Length();
			while (length > 0 && text[length - 1] == '0')
				length--;
			if (length > 0 && text[length - 1] == '.')
				length--;
			text.Truncate(length);
		}
	}
	if (text == "-0")
		text = "0";
	return text;
}


// #pragma mark - MenuSearch


MenuSearch::MenuSearch()
	:
	fCalculator(true),
	fApplicationsRead(false),
	fApplications(20)
{
}


MenuSearch::~MenuSearch()
{
}


/*static*/ const char*
MenuSearch::KindLabel(search_result_kind kind)
{
	switch (kind) {
		case kResultCalculator:
			return B_TRANSLATE("Calculator");
		case kResultConversion:
			return B_TRANSLATE("Conversion");
		case kResultApplication:
			return B_TRANSLATE("Application");
		case kResultSetting:
			return B_TRANSLATE("Setting");
		case kResultRecentDocument:
			return B_TRANSLATE("Recent document");
		case kResultRecentFolder:
			return B_TRANSLATE("Recent folder");
		case kResultFile:
			return B_TRANSLATE("File");
	}
	return "";
}


/*!	An expression is evaluated only when it has an operator or a function
	and starts like one (a digit, a sign, a parenthesis, a dot, "pi",
	"e" or a function name followed by a parenthesis): a plain number or a
	word ("terminal") is no expression.
*/
/*static*/ bool
MenuSearch::Calculate(const char* text, BString& value)
{
	BString expression(text);
	expression.Trim();
	if (expression.StartsWith("="))
		expression.Remove(0, 1);
	expression.Trim();
	if (expression.IsEmpty())
		return false;

	bool hasDigit = false;
	bool hasOperator = false;
	bool hasFunction = false;
	for (int32 i = 0; i < expression.Length(); i++) {
		char c = expression[i];
		if (isdigit(c))
			hasDigit = true;
		else if (strchr("+-*/^%!()", c) != NULL && !(i == 0 && c == '-'))
			hasOperator = true;
		else if (isalpha(c) && expression.FindFirst('(') > i)
			hasFunction = true;
	}
	char first = expression[0];
	bool startsLikeExpression = isdigit(first) || first == '-' || first == '('
		|| first == '.' || first == '+' || hasFunction
		|| expression.IStartsWith("pi");
	if (!startsLikeExpression || !(hasOperator || hasFunction)
		|| !(hasDigit || expression.IFindFirst("pi") >= 0)) {
		return false;
	}

	try {
		ExpressionParser parser;
		double result = parser.EvaluateToDouble(expression.String());
		if (isnan(result) || isinf(result))
			return false;
		value = format_number(result);
		return true;
	} catch (...) {
		return false;
	}
}


/*!	"<number> <unit> to|in|= <unit>", also without the space after the
	number ("5km to mi"). Both units must be of the same kind.
*/
/*static*/ bool
MenuSearch::Convert(const char* text, BString& value, BString& title)
{
	const char* p = text;
	while (isspace(*p))
		p++;
	char* end;
	double number = strtod(p, &end);
	if (end == p)
		return false;
	p = end;

	// the words that follow
	BString words[4];
	int32 count = 0;
	while (*p != '\0' && count < 4) {
		while (isspace(*p))
			p++;
		const char* start = p;
		while (*p != '\0' && !isspace(*p))
			p++;
		if (p > start)
			words[count++].SetTo(start, p - start);
	}
	while (isspace(*p))
		p++;
	if (*p != '\0' || count != 3)
		return false;
	if (words[1].ICompare("to") != 0 && words[1].ICompare("in") != 0
		&& words[1] != "=" && words[1] != "->") {
		return false;
	}

	const Unit* from = find_unit(words[0]);
	if (from == NULL)
		return false;
	const Unit* to = find_unit(words[2], from);
	if (to == NULL)
		return false;

	double result = convert_value(number, from, to);
	value.SetToFormat("%s %s", format_number(result).String(),
		unit_label(to).String());
	title.SetToFormat("%s %s = %s", format_number(number).String(),
		unit_label(from).String(), value.String());
	return true;
}


void
MenuSearch::_ReadDirectory(const char* path, int32 depth)
{
	BDirectory directory(path);
	if (directory.InitCheck() != B_OK)
		return;
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK
		&& fApplications.CountItems() < kMaxMenuEntries) {
		BPath entryPath;
		if (entry.GetPath(&entryPath) != B_OK)
			continue;
		// Folders are categories ("Applications", "Preferences"); their
		// entries are what can be opened. Symbolic links to folders are
		// entries too (they open the folder).
		if (entry.IsDirectory()) {
			if (depth > 0)
				_ReadDirectory(entryPath.Path(), depth - 1);
			continue;
		}
		Application* application = new Application;
		application->name = entryPath.Leaf();
		application->lowerName = application->name;
		application->lowerName.ToLower();
		entry.GetRef(&application->ref);
		// The same entry from two folders (packaged and the user's) is
		// listed once.
		bool known = false;
		for (int32 i = 0; i < fApplications.CountItems(); i++) {
			if (fApplications.ItemAt(i)->name == application->name) {
				known = true;
				break;
			}
		}
		if (known)
			delete application;
		else
			fApplications.AddItem(application);
	}
}


/*!	The Deskbar menu is the user's folder ~/config/settings/deskbar/menu
	merged with the packages' data/deskbar/menu (BDeskbar menu, see
	TDeskbarMenu): the same folders are read here.
*/
void
MenuSearch::_ReadApplications()
{
	fApplications.MakeEmpty();
	BPath path;
	if (find_directory(B_USER_DESKBAR_DIRECTORY, &path) == B_OK)
		_ReadDirectory(path.Path(), 3);
	static const directory_which kDataDirectories[] = {
		B_USER_NONPACKAGED_DATA_DIRECTORY,
		B_USER_DATA_DIRECTORY,
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY
	};
	for (size_t i = 0;
			i < sizeof(kDataDirectories) / sizeof(kDataDirectories[0]); i++) {
		if (find_directory(kDataDirectories[i], &path) == B_OK
			&& path.Append("deskbar/menu") == B_OK) {
			_ReadDirectory(path.Path(), 3);
		}
	}
	fApplicationsRead = true;
}


static int32
match_rank(const char* name, const char* lowerText)
{
	BString lower(name);
	lower.ToLower();
	if (lower.StartsWith(lowerText))
		return 0;
	// a word in the name starting with the text ("Media" in "Media
	// player"): as good as the start
	int32 at = lower.FindFirst(lowerText);
	if (at < 0)
		return -1;
	if (at > 0 && !isalnum(lower[at - 1]))
		return 0;
	return 1;
}


static void
add_sorted(SearchResultList& results, SearchResult* result, int32 from)
{
	// stable by rank within the results from \a from on
	int32 index = results.CountItems();
	while (index > from && results.ItemAt(index - 1)->rank > result->rank)
		index--;
	results.AddItem(result, index);
}


void
MenuSearch::SearchQuick(const char* text, SearchResultList& results)
{
	BString trimmed(text);
	trimmed.Trim();
	if (trimmed.IsEmpty())
		return;
	BString lowerText(trimmed);
	lowerText.ToLower();

	if (fCalculator) {
		BString value;
		BString title;
		if (Convert(trimmed, value, title)) {
			SearchResult* result = new SearchResult;
			result->kind = kResultConversion;
			result->title = title;
			result->detail = KindLabel(kResultConversion);
			result->value = value;
			result->rank = 0;
			results.AddItem(result);
		} else if (Calculate(trimmed, value)) {
			SearchResult* result = new SearchResult;
			result->kind = kResultCalculator;
			result->title.SetToFormat("%s = %s", trimmed.String(),
				value.String());
			result->detail = KindLabel(kResultCalculator);
			result->value = value;
			result->rank = 0;
			results.AddItem(result);
		}
	}

	if (!fApplicationsRead)
		_ReadApplications();
	int32 first = results.CountItems();
	for (int32 i = 0; i < fApplications.CountItems(); i++) {
		Application* application = fApplications.ItemAt(i);
		int32 rank = match_rank(application->name, lowerText);
		if (rank < 0)
			continue;
		SearchResult* result = new SearchResult;
		result->kind = kResultApplication;
		result->title = application->name;
		result->detail = KindLabel(kResultApplication);
		result->ref = application->ref;
		result->rank = rank;
		add_sorted(results, result, first);
		if (results.CountItems() - first > kMaxApplications)
			delete results.RemoveItemAt(results.CountItems() - 1);
	}

	std::vector<std::pair<const ControlPanelIndex::PageInfo*, std::string> >
		settings = ControlPanelIndex::search(trimmed);
	for (size_t i = 0; i < settings.size() && (int32)i < kMaxSettings; i++) {
		SearchResult* result = new SearchResult;
		result->kind = kResultSetting;
		result->title = settings[i].second.c_str();
		result->detail.SetToFormat("%s: %s", B_TRANSLATE("Control Panel"),
			settings[i].first->title);
		result->page = settings[i].first->id;
		result->rank = 0;
		results.AddItem(result);
	}

	for (int kind = 0; kind < 2; kind++) {
		BMessage recent;
		if (kind == 0)
			be_roster->GetRecentDocuments(&recent, 40);
		else
			be_roster->GetRecentFolders(&recent, 40);
		entry_ref ref;
		int32 added = 0;
		first = results.CountItems();
		for (int32 i = 0; recent.FindRef("refs", i, &ref) == B_OK
				&& added < kMaxRecent; i++) {
			if (ref.name == NULL)
				continue;
			int32 rank = match_rank(ref.name, lowerText);
			if (rank < 0 || !BEntry(&ref).Exists())
				continue;
			SearchResult* result = new SearchResult;
			result->kind = kind == 0 ? kResultRecentDocument
				: kResultRecentFolder;
			result->title = ref.name;
			BPath path;
			if (BEntry(&ref).GetPath(&path) == B_OK
				&& path.GetParent(&path) == B_OK) {
				result->detail = path.Path();
			} else
				result->detail = KindLabel(result->kind);
			result->ref = ref;
			result->rank = rank;
			add_sorted(results, result, first);
			added++;
		}
	}
}


/*!	A name query on the boot volume: "name" is indexed on BFS, so this is
	fast even on large volumes. Every letter becomes "[xX]" because BFS
	queries compare with case.
*/
/*static*/ void
MenuSearch::SearchFiles(const char* text, int32 max,
	SearchResultList& results, volatile bool* cancel)
{
	BString trimmed(text);
	trimmed.Trim();
	if (trimmed.Length() < 2)
		return;

	BString pattern;
	for (int32 i = 0; i < trimmed.Length(); i++) {
		char c = trimmed[i];
		if (isalpha(c)) {
			pattern << '[' << (char)tolower(c) << (char)toupper(c) << ']';
		} else if (strchr("*?[]\\\"", c) != NULL) {
			pattern << '\\' << c;
		} else
			pattern << c;
	}
	BString predicate;
	predicate.SetToFormat("name==\"*%s*\"", pattern.String());

	BVolume volume;
	if (BVolumeRoster().GetBootVolume(&volume) != B_OK)
		return;
	BQuery query;
	query.SetVolume(&volume);
	if (query.SetPredicate(predicate) != B_OK || query.Fetch() != B_OK)
		return;

	BString lowerText(trimmed);
	lowerText.ToLower();
	int32 first = results.CountItems();
	entry_ref ref;
	int32 added = 0;
	while (added < max && (cancel == NULL || !*cancel)
		&& query.GetNextRef(&ref) == B_OK) {
		BPath path;
		if (BEntry(&ref).GetPath(&path) != B_OK)
			continue;
		// Packaged system files are found by the applications search
		// already; their copies in the package mounts are noise here.
		if (strncmp(path.Path(), "/boot/system/", 13) == 0
			|| strncmp(path.Path(), "/boot/home/config/", 18) == 0
			|| strstr(path.Path(), "/.") != NULL) {
			continue;
		}
		SearchResult* result = new SearchResult;
		result->kind = kResultFile;
		result->title = ref.name;
		if (path.GetParent(&path) == B_OK)
			result->detail = path.Path();
		result->ref = ref;
		result->rank = match_rank(ref.name, lowerText);
		add_sorted(results, result, first);
		added++;
	}
}
