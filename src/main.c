/*
 * Copyright (c) 2016 Daniel Loffgren
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <ctype.h>    /* toupper, isalnum */
#include <err.h>      /* warnx */
#include <stdbool.h>
#include <stdlib.h>   /* exit */
#include <stdio.h>    /* printf, fprintf */
#include <string.h>   /* strlen */
#include <sysexits.h> /* EX_USAGE, EX_OK */
#include <unistd.h>   /* getopt */

#include <curl/curl.h>

#define URL_PREFIX_TAF     "http://tgftp.nws.noaa.gov/data/forecasts/taf/stations/"
#define URL_PREFIX_DECODED "http://tgftp.nws.noaa.gov/data/observations/metar/decoded/"
#define URL_PREFIX_METAR   "http://tgftp.nws.noaa.gov/data/observations/metar/stations/"
#define URL_EXTENSION      ".TXT"

#define STATION_ID_LEN              (4)
#define DEFAULT_STATION_PREFIX      "K" /* TODO: This should be localizable */
#define DEFAULT_STATION_PREFIX_LEN  (sizeof(DEFAULT_STATION_PREFIX) - 1)
#define HTTP_RESPONSE_NOT_FOUND     404
#define REQUEST_TIMEOUT             (3L) /* seconds */

/* The URL_BUFFER_LEN must be large enough to fit the largest producible URL.
 * This is easy to ensure by simply keeping the LONGEST_URL_PREFIX set to the
 * largest prefix string. The prefix and extension null terminators are
 * subtracted from the sizeof's, and the final null terminator is added at the
 * end.
 */
#define LONGEST_URL_PREFIX URL_PREFIX_METAR
#define URL_BUFFER_LEN     ( (sizeof(LONGEST_URL_PREFIX) - 1) + \
                             STATION_ID_LEN + \
                             (sizeof(URL_EXTENSION) - 1) + \
                             1 \
                           )

enum urlType {
	METAR,
	TAF,
	Decoded
};

/*
 * Create a URL from parts, namely the urlType and the station, and store
 * the result in the specified buffer.
 */
static bool
formURL(char *buf, size_t bufLen, enum urlType type, const char *station) {
	/* Ensure the station is a valid length */
	size_t stationLen = strlen(station);
	if(stationLen != STATION_ID_LEN &&
	   stationLen != STATION_ID_LEN - DEFAULT_STATION_PREFIX_LEN) {
		warnx("Station ID must be either three or four characters long.");
		return false;
	}

	/* Copy the first part of the URL */
	const char *prefix;
	switch (type) {
		case METAR: {
			prefix = URL_PREFIX_METAR;
		} break;
		case TAF: {
			prefix = URL_PREFIX_TAF;
		} break;
		case Decoded: {
			prefix = URL_PREFIX_DECODED;
		} break;
		default: {
			return false;
		}
	}
	strncpy(buf, prefix, bufLen);
	size_t written = strlen(prefix);

	/* Transfer the station id from end to beginning, simultaneously capitalizing */
	for (size_t i = 1; i <= stationLen; i++) {
		if (!isalnum((int)station[stationLen - i])) {
			warnx("Station ID must contain only alphanumeric characters.");
			return false;
		}
		buf[(written + STATION_ID_LEN) - i] = (char)toupper((int)station[stationLen - i]);
	}

	/* If it is a character short, prepend the 'K' */
	if (stationLen == STATION_ID_LEN - DEFAULT_STATION_PREFIX_LEN) {
		memcpy(buf + written, DEFAULT_STATION_PREFIX, DEFAULT_STATION_PREFIX_LEN);
	}
	written += STATION_ID_LEN;

	/* Append the extension. If the following assertion fails, then we were
	 * handed a buffer that was too short! */
	assert(bufLen >= written + sizeof(URL_EXTENSION));
	strncpy(buf + written, URL_EXTENSION, sizeof(URL_EXTENSION));

	return true;
}

/*
 * This is a libcurl callback that simply dumps the data received, byte-for-byte.
 */
static size_t
printData(void *contents, size_t size, size_t nmemb, void *userp) {
	(void)userp;

	/* NOAA automated information always has a newline at the end, as is required
	 * to be a valid POSIX text file. */
	printf("%s", (const char *)contents);
	return size * nmemb;
}

static int
usage(void) {
	fprintf(stderr, "usage: metar [-dt] station_id [...]\n"
	                "\t-d Show decoded METAR output\n"
	                "\t-t Show TAFs where available\n"
	);

	return EX_USAGE;
}

int
main(int argc, char * const argv[]) {
	bool decoded = false;
	bool tafs = false;

	int ch;
	while ((ch = getopt(argc, argv, "dt")) != -1) {
		switch (ch) {
			case 'd': {
				decoded = true;
			} break;
			case 't': {
				tafs = true;
			} break;
			default: {
				return usage();
			}
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		warnx("At least one argument is required");
		return usage();
	}

	CURL *curl = curl_easy_init();
	if (!curl) {
		return EX_SOFTWARE;
	}

	/* Only set up the things that will be the same for every request once. */
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, printData);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, true);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	for (int arg = 0; arg < argc; arg++) {
		char url[URL_BUFFER_LEN];
		if (!formURL(url, sizeof(url), decoded ? Decoded : METAR, argv[arg])) {
			continue;
		}

		curl_easy_setopt(curl, CURLOPT_URL, url);
		CURLcode res = curl_easy_perform(curl);

		/* This is resilient to both FTP and HTTP */
		long response;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
		if (res == CURLE_REMOTE_FILE_NOT_FOUND || response == HTTP_RESPONSE_NOT_FOUND) {
			warnx("Station ID \"%s\" not found", argv[arg]);
			continue;
		}

		if (res != CURLE_OK) {
			warnx("%s", curl_easy_strerror(res));
			warnx("Unable to fetch information for station ID \"%s\"", argv[arg]);
			continue;
		}

		/* If -t was specified, attempt to fetch TAF, failing silently */
		if (tafs) {
			if(formURL(url, sizeof(url), TAF, argv[arg])) {
				curl_easy_setopt(curl, CURLOPT_URL, url);
				curl_easy_perform(curl);
			}
		}
	}

	curl_easy_cleanup(curl);
	curl_global_cleanup();
	return EX_OK;
}
