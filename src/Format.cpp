/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Format.h"

#include <stdio.h>
#include <time.h>

#include <Catalog.h>
#include <StringFormat.h>

#include <algorithm>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Detail"


static BString
short_number(int64 count)
{
	BString number;
	if (count >= 1000000000)
		number.SetToFormat("%.1fB", count / 1000000000.0);
	else if (count >= 1000000)
		number.SetToFormat("%.1fM", count / 1000000.0);
	else if (count >= 1000)
		number.SetToFormat("%.1fK", count / 1000.0);
	else
		number.SetToFormat("%lld", (long long)count);
	return number;
}


BString
followers_text(int64 followers)
{
	BString text(B_TRANSLATE("%count% subscribers"));
	text.ReplaceFirst("%count%", short_number(followers).String());
	return text;
}


BString
views_text(int64 views)
{
	BString number = short_number(views);
	BString text(B_TRANSLATE("%views% views"));
	text.ReplaceFirst("%views%", number.String());
	return text;
}


static BString
plural(const char* pattern, int32 count)
{
	BString text;
	BStringFormat format(pattern);
	if (format.InitCheck() != B_OK || format.Format(text, count) != B_OK)
		text.SetToFormat("%" B_PRId32, count);
	return text;
}


BString
age_text(const BString& date)
{
	int year = 0, month = 0, day = 0;
	if (sscanf(date.String(), "%d-%d-%d", &year, &month, &day) != 3
		|| year < 1970 || month < 1 || month > 12 || day < 1 || day > 31)
		return BString();
	// both at noon of their day, in local time: whole days apart
	struct tm then = {};
	then.tm_year = year - 1900;
	then.tm_mon = month - 1;
	then.tm_mday = day;
	then.tm_hour = 12;
	time_t now = time(NULL);
	struct tm today;
	localtime_r(&now, &today);
	today.tm_hour = 12;
	today.tm_min = today.tm_sec = 0;
	double seconds = difftime(mktime(&today), mktime(&then));
	if (seconds < -43200)
		return BString();
	int32 days = (int32)(seconds / 86400 + 0.5);
	if (days <= 0)
		return B_TRANSLATE("today");
	if (days == 1)
		return B_TRANSLATE("yesterday");
	if (days < 7) {
		return plural(B_TRANSLATE("{0, plural, one{# day ago} "
			"other{# days ago}}"), days);
	}
	if (days < 30) {
		return plural(B_TRANSLATE("{0, plural, one{# week ago} "
			"other{# weeks ago}}"), days / 7);
	}
	if (days < 365) {
		return plural(B_TRANSLATE("{0, plural, one{# month ago} "
			"other{# months ago}}"), std::max((int32)1, days / 30));
	}
	return plural(B_TRANSLATE("{0, plural, one{# year ago} "
		"other{# years ago}}"), days / 365);
}


BString
views_and_age(int64 views, const BString& date)
{
	BString text;
	if (views > 0)
		text = views_text(views);
	BString age = age_text(date);
	if (!age.IsEmpty()) {
		if (!text.IsEmpty())
			text << " · ";
		text << age;
	}
	return text;
}
