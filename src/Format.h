/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef FORMAT_H
#define FORMAT_H


#include <String.h>


// "1.2M views"
BString		views_text(int64 views);
// "23.0M subscribers"
BString		followers_text(int64 followers);
// how long ago a date (YYYY-MM-DD) was: "today", "3 days ago", "2 years
// ago"; empty when it is not one, or still to come
BString		age_text(const BString& date);
// "1.2M views · 3 days ago", or what of it is known
BString		views_and_age(int64 views, const BString& date);


#endif	// FORMAT_H
