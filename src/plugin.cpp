/*
 * TeamSpeak 3 Overwatch Player Statistics Plugin
 *
 * Copyright (c) 2016 DryWolf
 */

#ifdef _WIN32
#pragma warning (disable : 4100)  /* Disable Unreferenced parameter warning */
#include <Windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "teamspeak/public_errors.h"
#include "teamspeak/public_errors_rare.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_rare_definitions.h"
#include "teamspeak/clientlib_publicdefinitions.h"
#include "ts3_functions.h"
#include "plugin.h"

// http
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/curlbuild.h>

// std
#include <algorithm>
#include <string>
#include <vector>

typedef struct OWPlayerData
{
	char* ts3name;

	char* battleTag;
	char* statsTag;

	int level;
	int prestige;

	int losses;
	int wins;

	int games;

	int comp_rank;

} OWPlayerData;

typedef std::vector<OWPlayerData> OWPlayerDataList;

static struct TS3Functions ts3Functions;

#ifdef _WIN32
#define _strcpy(dest, destSize, src) strcpy_s(dest, destSize, src)
#define snprintf sprintf_s
#else
#define _strcpy(dest, destSize, src) { strncpy(dest, src, destSize-1); (dest)[destSize-1] = '\0'; }
#endif

#define PLUGIN_API_VERSION 20

#define PATH_BUFSIZE 512
#define COMMAND_BUFSIZE 128
#define INFODATA_BUFSIZE 512
#define SERVERINFO_BUFSIZE 256
#define CHANNELINFO_BUFSIZE 512
#define RETURNCODE_BUFSIZE 128

static char* pluginID = NULL;

extern "C" {

#ifdef _WIN32
/* Helper function to convert wchar_T to Utf-8 encoded strings on Windows */
static int wcharToUtf8(const wchar_t* str, char** result) {
	int outlen = WideCharToMultiByte(CP_UTF8, 0, str, -1, 0, 0, 0, 0);
	*result = (char*)malloc(outlen * sizeof(char));
	if(WideCharToMultiByte(CP_UTF8, 0, str, -1, *result, outlen, 0, 0) == 0) {
		*result = NULL;
		return -1;
	}
	return 0;
}

/* Helper function to convert Utf-8 to wchar_T encoded strings on Windows */
static int utf8ToWchar(const char* str, wchar_t** result) {
	int outlen = MultiByteToWideChar(CP_ACP, 0, str, -1, 0, 0);
	*result = (wchar_t*)malloc(outlen * sizeof(wchar_t));
	if (MultiByteToWideChar(CP_ACP, 0, str, -1, *result, outlen) == 0) {
		*result = NULL;
		return -1;
	}
	return 0;
}
#endif

char* player_separator;
const char* PlayerInfoFormatStr = "lvl: %d%s | w/l: [color=%s]%4.2f%%[/color] | rank: [color=%s]%d[/color] | #games: %d %s %s [url=%s]...[/url]";

const int num_raised_digits = 10;

static char* digits_table_src[] =
{
	"0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};
static wchar_t* raised_digits_table_src[] =
{
	L"⁰", L"¹", L"²", L"³", L"⁴", L"⁵", L"⁶", L"⁷", L"⁸", L"⁹",
};
static char* raised_digits_table[num_raised_digits];

static wchar_t* star_digit_src = L"☆";
static char* star_digit;

void init_utf8_digits()
{
	for (int i = 0; i < num_raised_digits; ++i)
	{
		const wchar_t* src_d = raised_digits_table_src[i];
		char* raised_d;

		if (wcharToUtf8(src_d, &raised_d) == -1) {  /* Convert name into UTF-8 encoded result */
			raised_d = digits_table_src[i];  /* Conversion failed, fallback here */
		}

		raised_digits_table[i] = raised_d;
	}

	if (wcharToUtf8(star_digit_src, &star_digit) == -1) {  /* Convert name into UTF-8 encoded result */
		star_digit = "*";  /* Conversion failed, fallback here */
	}
}

/* Converts a hex character to its integer value */
char from_hex(char ch) {
	return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/* Converts an integer value to its hex character*/
char to_hex(char code) {
	static char hex[] = "0123456789abcdef";
	return hex[code & 15];
}

/* Returns a url-encoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
char *url_encode(char *str) {
	char *pstr = str, *buf = (char*)malloc(strlen(str) * 3 + 1), *pbuf = buf;
	while (*pstr) {
		if (isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' || *pstr == '~')
			*pbuf++ = *pstr;
		else if (*pstr == ' ')
			*pbuf++ = '+';
		else
			*pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ = to_hex(*pstr & 15);
		pstr++;
	}
	*pbuf = '\0';
	return buf;
}

/* Returns a url-decoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
char *url_decode(char *str) {
	char *pstr = str, *buf = (char*)malloc(strlen(str) + 1), *pbuf = buf;
	while (*pstr) {
		if (*pstr == '%') {
			if (pstr[1] && pstr[2]) {
				*pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
				pstr += 2;
			}
		}
		else if (*pstr == '+') {
			*pbuf++ = ' ';
		}
		else {
			*pbuf++ = *pstr;
		}
		pstr++;
	}
	*pbuf = '\0';
	return buf;
}


/*********************************** Required functions ************************************/
/*
 * If any of these required functions is not implemented, TS3 will refuse to load the plugin
 */

/* Unique name identifying this plugin */
const char* ts3plugin_name() {
#ifdef _WIN32
	/* TeamSpeak expects UTF-8 encoded characters. Following demonstrates a possibility how to convert UTF-16 wchar_t into UTF-8. */
	static char* result = NULL;  /* Static variable so it's allocated only once */
	if(!result) {
		const wchar_t* name = L"Overwatch Player Stats";
		if(wcharToUtf8(name, &result) == -1) {  /* Convert name into UTF-8 encoded result */
			result = "Overwatch Player Stats";  /* Conversion failed, fallback here */
		}
	}
	return result;
#else
	return "Overwatch Player Stats";
#endif
}

/* Plugin version */
const char* ts3plugin_version() {
    return "0.1-alpha";
}

/* Plugin API version. Must be the same as the clients API major version, else the plugin fails to load. */
int ts3plugin_apiVersion() {
	return PLUGIN_API_VERSION;
}

/* Plugin author */
const char* ts3plugin_author() {
#ifdef _WIN32
	/* TeamSpeak expects UTF-8 encoded characters. Following demonstrates a possibility how to convert UTF-16 wchar_t into UTF-8. */
	static char* result = NULL;  /* Static variable so it's allocated only once */
	if (!result) {
		const wchar_t* name = L"DryWolf © 2016";
		if (wcharToUtf8(name, &result) == -1) {  /* Convert name into UTF-8 encoded result */
			result = "DryWolf © 2016";  /* Conversion failed, fallback here */
		}
	}
	return result;
#else
	return "DryWolf © 2016";
#endif
}

/* Plugin description */
const char* ts3plugin_description() {
	/* If you want to use wchar_t, see ts3plugin_name() on how to use */
    return "This plugin allows to view Overwatch player statistics.";
}

/* Set TeamSpeak 3 callback functions */
void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    ts3Functions = funcs;
}

void* curl;

/*
 * Custom code called right after loading the plugin. Returns 0 on success, 1 on failure.
 * If the function returns 1 on failure, the plugin will be unloaded again.
 */
int ts3plugin_init() {
    char appPath[PATH_BUFSIZE];
    char resourcesPath[PATH_BUFSIZE];
    char configPath[PATH_BUFSIZE];
	char pluginPath[PATH_BUFSIZE];

    /* Your plugin init code here */
    printf("PLUGIN: init\n");

    /* Example on how to query application, resources and configuration paths from client */
    /* Note: Console client returns empty string for app and resources path */
    ts3Functions.getAppPath(appPath, PATH_BUFSIZE);
    ts3Functions.getResourcesPath(resourcesPath, PATH_BUFSIZE);
    ts3Functions.getConfigPath(configPath, PATH_BUFSIZE);
	ts3Functions.getPluginPath(pluginPath, PATH_BUFSIZE);

	printf("PLUGIN: App path: %s\nResources path: %s\nConfig path: %s\nPlugin path: %s\n", appPath, resourcesPath, configPath, pluginPath);

	curl = curl_easy_init();

	init_utf8_digits();

	wcharToUtf8(L"        ", &player_separator);

    return 0;  /* 0 = success, 1 = failure, -2 = failure but client will not show a "failed to load" warning */
	/* -2 is a very special case and should only be used if a plugin displays a dialog (e.g. overlay) asking the user to disable
	 * the plugin again, avoiding the show another dialog by the client telling the user the plugin failed to load.
	 * For normal case, if a plugin really failed to load because of an error, the correct return value is 1. */
}

/* Custom code called right before the plugin is unloaded */
void ts3plugin_shutdown() {
    /* Your plugin cleanup code here */
    printf("PLUGIN: shutdown\n");

	curl_easy_cleanup(curl);

	/*
	 * Note:
	 * If your plugin implements a settings dialog, it must be closed and deleted here, else the
	 * TeamSpeak client will most likely crash (DLL removed but dialog from DLL code still open).
	 */

	/* Free pluginID if we registered it */
	if(pluginID) {
		free(pluginID);
		pluginID = NULL;
	}
}

/****************************** Optional functions ********************************/
/*
 * Following functions are optional, if not needed you don't need to implement them.
 */

/* Tell client if plugin offers a configuration window. If this function is not implemented, it's an assumed "does not offer" (PLUGIN_OFFERS_NO_CONFIGURE). */
int ts3plugin_offersConfigure() {
	printf("PLUGIN: offersConfigure\n");
	/*
	 * Return values:
	 * PLUGIN_OFFERS_NO_CONFIGURE         - Plugin does not implement ts3plugin_configure
	 * PLUGIN_OFFERS_CONFIGURE_NEW_THREAD - Plugin does implement ts3plugin_configure and requests to run this function in an own thread
	 * PLUGIN_OFFERS_CONFIGURE_QT_THREAD  - Plugin does implement ts3plugin_configure and requests to run this function in the Qt GUI thread
	 */
	return PLUGIN_OFFERS_NO_CONFIGURE;  /* In this case ts3plugin_configure does not need to be implemented */
}

/* Plugin might offer a configuration window. If ts3plugin_offersConfigure returns 0, this function does not need to be implemented. */
void ts3plugin_configure(void* handle, void* qParentWidget) {
    printf("PLUGIN: configure\n");
}

/*
 * If the plugin wants to use error return codes, plugin commands, hotkeys or menu items, it needs to register a command ID. This function will be
 * automatically called after the plugin was initialized. This function is optional. If you don't use these features, this function can be omitted.
 * Note the passed pluginID parameter is no longer valid after calling this function, so you must copy it and store it in the plugin.
 */
void ts3plugin_registerPluginID(const char* id) {
	const size_t sz = strlen(id) + 1;
	pluginID = (char*)malloc(sz * sizeof(char));
	_strcpy(pluginID, sz, id);  /* The id buffer will invalidate after exiting this function */
	printf("PLUGIN: registerPluginID: %s\n", pluginID);
}

/* Plugin command keyword. Return NULL or "" if not used. */
const char* ts3plugin_commandKeyword() {
	return "ow";
}

struct string {
	char *ptr;
	size_t len;
};

void init_string(struct string *s) {
	s->len = 0;
	s->ptr = (char*)malloc(s->len + 1);
	if (s->ptr == NULL) {
		fprintf(stderr, "malloc() failed\n");
		exit(EXIT_FAILURE);
	}
	s->ptr[0] = '\0';
}

size_t writefunc(void *ptr, size_t size, size_t nmemb, struct string *s)
{
	size_t new_len = s->len + size*nmemb;
	s->ptr = (char*)realloc(s->ptr, new_len + 1);
	if (s->ptr == NULL) {
		fprintf(stderr, "realloc() failed\n");
		exit(EXIT_FAILURE);
	}
	memcpy(s->ptr + s->len, ptr, size*nmemb);
	s->ptr[new_len] = '\0';
	s->len = new_len;

	return size*nmemb;
}

char* download(const char* url)
{
	struct string s;
	init_string(&s);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	/* example.com is redirected, so we tell libcurl to follow redirection */
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1); //Prevent "longjmp causes uninitialized stack frame" bug
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "deflate");

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);

	/* Perform the request, res will get the return code */
	CURLcode res = curl_easy_perform(curl);
	/* Check for errors */
	if (res != CURLE_OK) {
		char msg[1024];
		sprintf(msg, "curl_easy_perform() failed: %s", curl_easy_strerror(res));
		ts3Functions.printMessageToCurrentTab(msg);

		fprintf(stderr, "curl_easy_perform() failed: %s\n",
			curl_easy_strerror(res));

		return 0;
	}

	return s.ptr;
}

int findProperty(char* html, const char* prop_prefix, const int default)
{
	char* prop_str = strstr(html, prop_prefix);
	const int prop_prefix_len = strlen(prop_prefix);

	if (prop_str != NULL)
	{
		int i = 0;
		while (1)
		{
			char c = prop_str[i];
			if (c == ',' || c == '\0')
				break;

			++i;
		}

		prop_str = strdup(prop_str + prop_prefix_len);
		prop_str[i - prop_prefix_len] = '\0';
	}

	if (!prop_str)
		return default;

	int result = atoi(prop_str);
	free(prop_str);
	return result;
}

int getPlayerData(uint64 serverConnectionHandlerID, anyID clientID, OWPlayerData* result)
{
	result->ts3name = NULL;

	result->battleTag = NULL;
	result->statsTag = NULL;

	result->level = 0;
	result->prestige = 0;

	result->losses = 0;
	result->wins = 0;

	result->games = 0;
	result->comp_rank = 0;

	char clientName[256];
	ts3Functions.getClientDisplayName(serverConnectionHandlerID, clientID, clientName, 256);

	result->ts3name = (char*)malloc(strlen(clientName) + 1);
	strcpy(result->ts3name, clientName);

	int clientNameLen = strlen(clientName);	

	int hashIdx = -1;
	for (int i = 0; i < clientNameLen; ++i)
		if (clientName[i] == '#')
		{
			hashIdx = i;
			break;
		}

	if (hashIdx < 0)
	{
		return 0;
	}

	int nameStart = -1;
	for (int i = hashIdx - 1; i >= 0; --i)
	{
		wchar_t c = *(clientName + i);

		if (c & 0x80)
			continue;

		int is_alpha = iswalnum(c);
		int is_space = isspace(c);

		if (!is_alpha || is_space)
		{
			nameStart = i + 1;
			break;
		}

		if (i == 0)
			nameStart = i;
	}

	if (nameStart < 0)
	{
		return 0;
	}

	int nameEnd = -1;
	for (int i = hashIdx + 1; i < clientNameLen; ++i)
	{
		wchar_t c = *(clientName + i);

		if (c & 0x80)
			continue;

		if (!iswdigit(c))
		{
			nameEnd = i - 1;
			break;
		}

		if (i == clientNameLen - 1)
			nameEnd = i;
	}

	if (nameEnd < 0)
	{
		return 0;
	}

	char battleTag[256];

	// format battleTag
	strcpy(battleTag, clientName + nameStart);
	battleTag[nameEnd - nameStart + 1] = '\0';	
	result->battleTag = strdup(battleTag);

	// format statsTag
	battleTag[hashIdx - nameStart] = '-';
	result->statsTag = strdup(battleTag);

	char battleUrl[256];
	sprintf(battleUrl, "https://owapi.net/api/v2/u/%s/stats/general", url_encode(battleTag));

	char* battleHtml = download(battleUrl);

	if (battleHtml == 0)
		return 0;

	// lookup stats
	int prestige = findProperty(battleHtml, "\"prestige\": ", 0);
	int level = findProperty(battleHtml, "\"level\": ", 0);

	int losses = findProperty(battleHtml, "\"losses\": ", 0);
	int wins = findProperty(battleHtml, "\"wins\": ", 0);

	int games = findProperty(battleHtml, "\"games\": ", 0);
	int comp_rank = findProperty(battleHtml, "\"comprank\": ", 0);

	if (battleHtml)
		free(battleHtml);

	result->level = level;
	result->prestige = prestige;

	result->losses = losses;
	result->wins = wins;

	result->games = games;
	result->comp_rank = comp_rank;

	return 0;
}

void freePlayerData(OWPlayerData* player_data)
{
	if (player_data->ts3name)
	{
		free(player_data->ts3name);
		player_data->ts3name = NULL;
	}

	if (player_data->battleTag)
	{
		free(player_data->battleTag);
		player_data->battleTag = NULL;
	}

	if (player_data->statsTag)
	{
		free(player_data->statsTag);
		player_data->statsTag = NULL;
	}
}

char* getPrestigeString(const OWPlayerData* player_data, const bool& table_format)
{
	char str[256];

	sprintf(str, "%d", player_data->level);
	int num_lvl_digits = strlen(str);

	sprintf(str, "%d", player_data->prestige);
	int num_digits = strlen(str);
	
	if (player_data->prestige == 0)
	{
		if (!table_format)
			return strdup(" ");

		char* placeholder;
		wcharToUtf8(L" ", &placeholder);

		int num_places = max(0, 2 - num_lvl_digits) * 2 /*lvl-fill*/ + 1 + /*separator*/ 3 /*star*/ + num_digits * 2 /*prestige-nr*/;
		char* no_prestige = (char*)malloc((num_places + 1) * 2);
		no_prestige[0] = '\0';

		for (int i = 0; i < num_places; ++i)
			strcat(no_prestige, placeholder);

		return no_prestige;
	}

	char* prestige_stars = (char*)malloc((num_digits + 3) * 2);
	prestige_stars[0] = '\0';

	strcat(prestige_stars, " ");
	strcat(prestige_stars, star_digit);

	for (int i = 0; i < num_digits; ++i)
	{
		char d_char = str[i];
		int d = (int)d_char - 48;
		char* raised_d = raised_digits_table[d];

		strcat(prestige_stars, raised_d);
	}

	return prestige_stars;
}

float getWinRatio(const OWPlayerData* player_data)
{
	if (player_data->losses == 0)
		return 0;

	float win_ratio = (float)player_data->wins / (float)player_data->losses;
	return win_ratio;
}

float getWinPercent(const OWPlayerData* player_data)
{
	if (player_data->losses == 0)
		return 0;

	int total_games = player_data->wins + player_data->losses;
	float win_percent = 100.0f * (float)player_data->wins / (float)total_games;
	return win_percent;
}

char* getStatsURL(const OWPlayerData* player_data)
{
	char statsUrl[256];
	sprintf(statsUrl, "http://masteroverwatch.com/profile/pc/eu/%s", player_data->statsTag);
	return strdup(statsUrl);
}

bool sortPlayerList(const OWPlayerData& a, const OWPlayerData& b)
{
	if (a.prestige > b.prestige)
		return true;

	if (a.prestige == b.prestige)
		return a.level > b.level;

	return false;
}

void printChannelPlayerStats(uint64 serverConnectionHandlerID, anyID channelID)
{
	anyID* channelClientIDs;

	if (ts3Functions.getChannelClientList(serverConnectionHandlerID, channelID, &channelClientIDs) != ERROR_ok) {
		ts3Functions.printMessageToCurrentTab("Error querying channel client list");
		return;
	}	

	char* channelName;
	if (ts3Functions.getChannelVariableAsString(serverConnectionHandlerID, channelID, CHANNEL_NAME, &channelName) != ERROR_ok) {
		channelName = strdup("???");
	}
	
	anyID clientID = -1;
	int cliendIdx = 0;

	OWPlayerDataList channel_player_stats;

	while ((clientID = channelClientIDs[cliendIdx++]) != NULL)
	{
		OWPlayerData temp;
		if (!getPlayerData(serverConnectionHandlerID, clientID, &temp))
		{
			channel_player_stats.push_back(temp);
		}
	}

	std::sort(channel_player_stats.begin(), channel_player_stats.end(), sortPlayerList);
	
	ts3Functions.printMessageToCurrentTab("------------------------------------------------------------------------------------------------------------------------------------------------------");
	ts3Functions.printMessageToCurrentTab(channelName);
	ts3Functions.printMessageToCurrentTab("------------------------------------------------------------------------------------------------------------------------------------------------------");

	free(channelName);

	for (OWPlayerDataList::iterator it = channel_player_stats.begin();
		it != channel_player_stats.end();
		++it)
	{
		OWPlayerData& player_data = *it;

		char msg[512];
		char* prestige = getPrestigeString(&player_data, true);
		float win_percent = getWinPercent(&player_data);
		const char* win_percent_color = win_percent < 50.0 ? "#b85813" : "#588c01";
		const char* comp_rank_color = player_data.comp_rank != 0 && player_data.comp_rank < 50 ? "#b85813" : "#588c01";
		char* stats_url = getStatsURL(&player_data);

		sprintf(msg, PlayerInfoFormatStr,
			player_data.level,
			prestige,
			win_percent_color,
			win_percent,
			comp_rank_color,
			player_data.comp_rank,
			player_data.games,
			player_separator,
			player_data.ts3name,
			stats_url
		);

		free(prestige);
		free(stats_url);

		freePlayerData(&player_data);

		ts3Functions.printMessageToCurrentTab(msg);
	}

	ts3Functions.printMessageToCurrentTab("");

	ts3plugin_freeMemory(channelClientIDs);
}

/* Plugin processes console command. Return 0 if plugin handled the command, 1 if not handled. */
int ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char* command) {
	char buf[COMMAND_BUFSIZE];
	char *s;
	int i = 0;
	enum { CMD_NONE = 0, CMD_LIST_CHANNEL } cmd = CMD_NONE;
#ifdef _WIN32
	char* context = NULL;
#endif

	printf("PLUGIN: process command: '%s'\n", command);

	_strcpy(buf, COMMAND_BUFSIZE, command);
#ifdef _WIN32
	s = strtok_s(buf, " ", &context);
#else
	s = strtok(buf, " ");
#endif
	while(s != NULL) {
		if(i == 0) {
			if(!strcmp(s, "ls")) {
				cmd = CMD_LIST_CHANNEL;
			}
		}
#ifdef _WIN32
		s = strtok_s(NULL, " ", &context);
#else
		s = strtok(NULL, " ");
#endif
		i++;
	}

	switch(cmd) {
		case CMD_NONE:
			return 1;  /* Command not handled by plugin */

		case CMD_LIST_CHANNEL:
			{
				anyID myID;
				uint64 myChannelID;

				if (ts3Functions.getClientID(serverConnectionHandlerID, &myID) != ERROR_ok) {
					ts3Functions.printMessageToCurrentTab("Error querying client ID");
					break;
				}

				if (ts3Functions.getChannelOfClient(serverConnectionHandlerID, myID, &myChannelID) != ERROR_ok) {
					ts3Functions.printMessageToCurrentTab("Error querying channel ID");
					break;
				}

				printChannelPlayerStats(serverConnectionHandlerID, myChannelID);
			}
			break;
	}

	return 0;  /* Plugin handled command */
}

/* Client changed current server connection handler */
void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) {
    printf("PLUGIN: currentServerConnectionChanged %llu (%llu)\n", (long long unsigned int)serverConnectionHandlerID, (long long unsigned int)ts3Functions.getCurrentServerConnectionHandlerID());
}

/*
 * Implement the following three functions when the plugin should display a line in the server/channel/client info.
 * If any of ts3plugin_infoTitle, ts3plugin_infoData or ts3plugin_freeMemory is missing, the info text will not be displayed.
 */

/* Static title shown in the left column in the info frame */
const char* ts3plugin_infoTitle() {
	return "Overwatch Rank";
}

/*
 * Dynamic content shown in the right column in the info frame. Memory for the data string needs to be allocated in this
 * function. The client will call ts3plugin_freeMemory once done with the string to release the allocated memory again.
 * Check the parameter "type" if you want to implement this feature only for specific item types. Set the parameter
 * "data" to NULL to have the client ignore the info data.
 */
void ts3plugin_infoData(uint64 serverConnectionHandlerID, uint64 id, enum PluginItemType type, char** data) {

	OWPlayerData player_data;

	/* For demonstration purpose, display the name of the currently selected server, channel or client. */
	switch(type) {
		//case PLUGIN_SERVER:
		//	if(ts3Functions.getServerVariableAsString(serverConnectionHandlerID, VIRTUALSERVER_NAME, &name) != ERROR_ok) {
		//		printf("Error getting virtual server name\n");
		//		return;
		//	}
		//	break;
		//case PLUGIN_CHANNEL:
		//	if(ts3Functions.getChannelVariableAsString(serverConnectionHandlerID, id, CHANNEL_NAME, &name) != ERROR_ok) {
		//		printf("Error getting channel name\n");
		//		return;
		//	}
		//	break;
		case PLUGIN_CLIENT:
			{
				if (getPlayerData(serverConnectionHandlerID, id, &player_data))
				{
					char* clientName;
					if(ts3Functions.getClientVariableAsString(serverConnectionHandlerID, (anyID)id, CLIENT_NICKNAME, &clientName) != ERROR_ok) {
						clientName = strdup("?");
					}

					char msg[512];
					sprintf(msg, "Unable to get Overwatch player-stats for client: %s", clientName);
					ts3Functions.printMessageToCurrentTab(msg);
					return;
				}
			}
			break;
		default:
			printf("Invalid item type: %d\n", type);
			data = NULL;  /* Ignore */
			return;
	}

	*data = (char*)malloc(INFODATA_BUFSIZE * sizeof(char));  /* Must be allocated in the plugin! */
	char* prestige = getPrestigeString(&player_data, false);
	float win_percent = getWinPercent(&player_data);
	const char* win_percent_color = win_percent < 50.0 ? "#b85813" : "#588c01";
	const char* comp_rank_color = player_data.comp_rank != 0 && player_data.comp_rank < 50 ? "#b85813" : "#588c01";
	char* stats_url = getStatsURL(&player_data);

	snprintf(*data, INFODATA_BUFSIZE, PlayerInfoFormatStr, 
		player_data.level,
		prestige,
		win_percent_color,
		win_percent,
		comp_rank_color,
		player_data.comp_rank,
		player_data.games,
		"",
		"",
		stats_url
	);  /* bbCode is supported. HTML is not supported */

	free(prestige);
	free(stats_url);

	freePlayerData(&player_data);
}

/* Required to release the memory for parameter "data" allocated in ts3plugin_infoData and ts3plugin_initMenus */
void ts3plugin_freeMemory(void* data) {
	free(data);
}

/*
 * Plugin requests to be always automatically loaded by the TeamSpeak 3 client unless
 * the user manually disabled it in the plugin dialog.
 * This function is optional. If missing, no autoload is assumed.
 */
int ts3plugin_requestAutoload() {
	return 0;  /* 1 = request autoloaded, 0 = do not request autoload */
}

/* Helper function to create a menu item */
static struct PluginMenuItem* createMenuItem(enum PluginMenuType type, int id, const char* text, const char* icon) {
	struct PluginMenuItem* menuItem = (struct PluginMenuItem*)malloc(sizeof(struct PluginMenuItem));
	menuItem->type = type;
	menuItem->id = id;
	_strcpy(menuItem->text, PLUGIN_MENU_BUFSZ, text);
	_strcpy(menuItem->icon, PLUGIN_MENU_BUFSZ, icon);
	return menuItem;
}

/* Some makros to make the code to create menu items a bit more readable */
#define BEGIN_CREATE_MENUS(x) const size_t sz = x + 1; size_t n = 0; *menuItems = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * sz);
#define CREATE_MENU_ITEM(a, b, c, d) (*menuItems)[n++] = createMenuItem(a, b, c, d);
#define END_CREATE_MENUS (*menuItems)[n++] = NULL; assert(n == sz);

/*
 * Menu IDs for this plugin. Pass these IDs when creating a menuitem to the TS3 client. When the menu item is triggered,
 * ts3plugin_onMenuItemEvent will be called passing the menu ID of the triggered menu item.
 * These IDs are freely choosable by the plugin author. It's not really needed to use an enum, it just looks prettier.
 */
enum {
	MENU_ID_CHANNEL_1 = 1,
};

/*
 * Initialize plugin menus.
 * This function is called after ts3plugin_init and ts3plugin_registerPluginID. A pluginID is required for plugin menus to work.
 * Both ts3plugin_registerPluginID and ts3plugin_freeMemory must be implemented to use menus.
 * If plugin menus are not used by a plugin, do not implement this function or return NULL.
 */
void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon) {
	/*
	 * Create the menus
	 * There are three types of menu items:
	 * - PLUGIN_MENU_TYPE_CLIENT:  Client context menu
	 * - PLUGIN_MENU_TYPE_CHANNEL: Channel context menu
	 * - PLUGIN_MENU_TYPE_GLOBAL:  "Plugins" menu in menu bar of main window
	 *
	 * Menu IDs are used to identify the menu item when ts3plugin_onMenuItemEvent is called
	 *
	 * The menu text is required, max length is 128 characters
	 *
	 * The icon is optional, max length is 128 characters. When not using icons, just pass an empty string.
	 * Icons are loaded from a subdirectory in the TeamSpeak client plugins folder. The subdirectory must be named like the
	 * plugin filename, without dll/so/dylib suffix
	 * e.g. for "test_plugin.dll", icon "1.png" is loaded from <TeamSpeak 3 Client install dir>\plugins\test_plugin\1.png
	 */

	BEGIN_CREATE_MENUS(1);  /* IMPORTANT: Number of menu items must be correct! */
	CREATE_MENU_ITEM(PLUGIN_MENU_TYPE_CHANNEL, MENU_ID_CHANNEL_1, "Show player stats", "stats.png");
	END_CREATE_MENUS;  /* Includes an assert checking if the number of menu items matched */

	/*
	 * Specify an optional icon for the plugin. This icon is used for the plugins submenu within context and main menus
	 * If unused, set menuIcon to NULL
	 */
	*menuIcon = (char*)malloc(PLUGIN_MENU_BUFSZ * sizeof(char));
	_strcpy(*menuIcon, PLUGIN_MENU_BUFSZ, "ow_logo.png");

	/*
	 * Menus can be enabled or disabled with: ts3Functions.setPluginMenuEnabled(pluginID, menuID, 0|1);
	 * Test it with plugin command: /test enablemenu <menuID> <0|1>
	 * Menus are enabled by default. Please note that shown menus will not automatically enable or disable when calling this function to
	 * ensure Qt menus are not modified by any thread other the UI thread. The enabled or disable state will change the next time a
	 * menu is displayed.
	 */
	/* For example, this would disable MENU_ID_GLOBAL_2: */
	/* ts3Functions.setPluginMenuEnabled(pluginID, MENU_ID_GLOBAL_2, 0); */

	/* All memory allocated in this function will be automatically released by the TeamSpeak client later by calling ts3plugin_freeMemory */
}

/************************** TeamSpeak callbacks ***************************/
/*
 * Following functions are optional, feel free to remove unused callbacks.
 * See the clientlib documentation for details on each function.
 */

/* Clientlib */

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) {
    /* Some example code following to show how to use the information query functions. */

    if(newStatus == STATUS_CONNECTION_ESTABLISHED) {  /* connection established and we have client and channels available */
        char* s;
        char msg[1024];
        anyID myID;
        uint64* ids;
        size_t i;
		unsigned int error;

        /* Print clientlib version */
        if(ts3Functions.getClientLibVersion(&s) == ERROR_ok) {
            printf("PLUGIN: Client lib version: %s\n", s);
            ts3Functions.freeMemory(s);  /* Release string */
        } else {
            ts3Functions.logMessage("Error querying client lib version", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
            return;
        }

		/* Write plugin name and version to log */
        snprintf(msg, sizeof(msg), "Plugin %s, Version %s, Author: %s", ts3plugin_name(), ts3plugin_version(), ts3plugin_author());
        ts3Functions.logMessage(msg, LogLevel_INFO, "Plugin", serverConnectionHandlerID);

        /* Print virtual server name */
        if((error = ts3Functions.getServerVariableAsString(serverConnectionHandlerID, VIRTUALSERVER_NAME, &s)) != ERROR_ok) {
			if(error != ERROR_not_connected) {  /* Don't spam error in this case (failed to connect) */
				ts3Functions.logMessage("Error querying server name", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
			}
            return;
        }
        printf("PLUGIN: Server name: %s\n", s);
        ts3Functions.freeMemory(s);

        /* Print virtual server welcome message */
        if(ts3Functions.getServerVariableAsString(serverConnectionHandlerID, VIRTUALSERVER_WELCOMEMESSAGE, &s) != ERROR_ok) {
            ts3Functions.logMessage("Error querying server welcome message", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
            return;
        }
        printf("PLUGIN: Server welcome message: %s\n", s);
        ts3Functions.freeMemory(s);  /* Release string */

        /* Print own client ID and nickname on this server */
        if(ts3Functions.getClientID(serverConnectionHandlerID, &myID) != ERROR_ok) {
            ts3Functions.logMessage("Error querying client ID", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
            return;
        }
        if(ts3Functions.getClientSelfVariableAsString(serverConnectionHandlerID, CLIENT_NICKNAME, &s) != ERROR_ok) {
            ts3Functions.logMessage("Error querying client nickname", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
            return;
        }
        printf("PLUGIN: My client ID = %d, nickname = %s\n", myID, s);
        ts3Functions.freeMemory(s);

        /* Print list of all channels on this server */
        if(ts3Functions.getChannelList(serverConnectionHandlerID, &ids) != ERROR_ok) {
            ts3Functions.logMessage("Error getting channel list", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
            return;
        }
        printf("PLUGIN: Available channels:\n");
        for(i=0; ids[i]; i++) {
            /* Query channel name */
            if(ts3Functions.getChannelVariableAsString(serverConnectionHandlerID, ids[i], CHANNEL_NAME, &s) != ERROR_ok) {
                ts3Functions.logMessage("Error querying channel name", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
                return;
            }
            printf("PLUGIN: Channel ID = %llu, name = %s\n", (long long unsigned int)ids[i], s);
            ts3Functions.freeMemory(s);
        }
        ts3Functions.freeMemory(ids);  /* Release array */

        /* Print list of existing server connection handlers */
        printf("PLUGIN: Existing server connection handlers:\n");
        if(ts3Functions.getServerConnectionHandlerList(&ids) != ERROR_ok) {
            ts3Functions.logMessage("Error getting server list", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
            return;
        }
        for(i=0; ids[i]; i++) {
            if((error = ts3Functions.getServerVariableAsString(ids[i], VIRTUALSERVER_NAME, &s)) != ERROR_ok) {
				if(error != ERROR_not_connected) {  /* Don't spam error in this case (failed to connect) */
					ts3Functions.logMessage("Error querying server name", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
				}
                continue;
            }
            printf("- %llu - %s\n", (long long unsigned int)ids[i], s);
            ts3Functions.freeMemory(s);
        }
        ts3Functions.freeMemory(ids);
    }
}

void ts3plugin_onNewChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 channelParentID) {
}

void ts3plugin_onNewChannelCreatedEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 channelParentID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
}

void ts3plugin_onDelChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
}

void ts3plugin_onChannelMoveEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 newChannelParentID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
}

void ts3plugin_onUpdateChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
}

void ts3plugin_onUpdateChannelEditedEvent(uint64 serverConnectionHandlerID, uint64 channelID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
}

void ts3plugin_onUpdateClientEvent(uint64 serverConnectionHandlerID, anyID clientID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
}

void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* moveMessage) {
}

void ts3plugin_onClientMoveSubscriptionEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility) {
}

void ts3plugin_onClientMoveTimeoutEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* timeoutMessage) {
}

void ts3plugin_onClientMoveMovedEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID moverID, const char* moverName, const char* moverUniqueIdentifier, const char* moveMessage) {
}

void ts3plugin_onClientKickFromChannelEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage) {
}

void ts3plugin_onClientKickFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage) {
}

void ts3plugin_onClientIDsEvent(uint64 serverConnectionHandlerID, const char* uniqueClientIdentifier, anyID clientID, const char* clientName) {
}

void ts3plugin_onClientIDsFinishedEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onServerEditedEvent(uint64 serverConnectionHandlerID, anyID editerID, const char* editerName, const char* editerUniqueIdentifier) {
}

void ts3plugin_onServerUpdatedEvent(uint64 serverConnectionHandlerID) {
}

int ts3plugin_onServerErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, const char* extraMessage) {
	printf("PLUGIN: onServerErrorEvent %llu %s %d %s\n", (long long unsigned int)serverConnectionHandlerID, errorMessage, error, (returnCode ? returnCode : ""));
	if(returnCode) {
		/* A plugin could now check the returnCode with previously (when calling a function) remembered returnCodes and react accordingly */
		/* In case of using a a plugin return code, the plugin can return:
		 * 0: Client will continue handling this error (print to chat tab)
		 * 1: Client will ignore this error, the plugin announces it has handled it */
		return 1;
	}
	return 0;  /* If no plugin return code was used, the return value of this function is ignored */
}

void ts3plugin_onServerStopEvent(uint64 serverConnectionHandlerID, const char* shutdownMessage) {
}

int ts3plugin_onTextMessageEvent(uint64 serverConnectionHandlerID, anyID targetMode, anyID toID, anyID fromID, const char* fromName, const char* fromUniqueIdentifier, const char* message, int ffIgnored) {
    printf("PLUGIN: onTextMessageEvent %llu %d %d %s %s %d\n", (long long unsigned int)serverConnectionHandlerID, targetMode, fromID, fromName, message, ffIgnored);

	/* Friend/Foe manager has ignored the message, so ignore here as well. */
	if(ffIgnored) {
		return 0; /* Client will ignore the message anyways, so return value here doesn't matter */
	}

#if 0
	{
		/* Example code: Autoreply to sender */
		/* Disabled because quite annoying, but should give you some ideas what is possible here */
		/* Careful, when two clients use this, they will get banned quickly... */
		anyID myID;
		if(ts3Functions.getClientID(serverConnectionHandlerID, &myID) != ERROR_ok) {
			ts3Functions.logMessage("Error querying own client id", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
			return 0;
		}
		if(fromID != myID) {  /* Don't reply when source is own client */
			if(ts3Functions.requestSendPrivateTextMsg(serverConnectionHandlerID, "Text message back!", fromID, NULL) != ERROR_ok) {
				ts3Functions.logMessage("Error requesting send text message", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
			}
		}
	}
#endif

    return 0;  /* 0 = handle normally, 1 = client will ignore the text message */
}

void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status, int isReceivedWhisper, anyID clientID) {
	/* Demonstrate usage of getClientDisplayName */
	char name[512];
	if(ts3Functions.getClientDisplayName(serverConnectionHandlerID, clientID, name, 512) == ERROR_ok) {
		if(status == STATUS_TALKING) {
			printf("--> %s starts talking\n", name);
		} else {
			printf("--> %s stops talking\n", name);
		}
	}
}

void ts3plugin_onConnectionInfoEvent(uint64 serverConnectionHandlerID, anyID clientID) {
}

void ts3plugin_onServerConnectionInfoEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onChannelSubscribeEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
}

void ts3plugin_onChannelSubscribeFinishedEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onChannelUnsubscribeEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
}

void ts3plugin_onChannelUnsubscribeFinishedEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onChannelDescriptionUpdateEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
}

void ts3plugin_onChannelPasswordChangedEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
}

void ts3plugin_onPlaybackShutdownCompleteEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onSoundDeviceListChangedEvent(const char* modeID, int playOrCap) {
}

void ts3plugin_onEditPlaybackVoiceDataEvent(uint64 serverConnectionHandlerID, anyID clientID, short* samples, int sampleCount, int channels) {
}

void ts3plugin_onEditPostProcessVoiceDataEvent(uint64 serverConnectionHandlerID, anyID clientID, short* samples, int sampleCount, int channels, const unsigned int* channelSpeakerArray, unsigned int* channelFillMask) {
}

void ts3plugin_onEditMixedPlaybackVoiceDataEvent(uint64 serverConnectionHandlerID, short* samples, int sampleCount, int channels, const unsigned int* channelSpeakerArray, unsigned int* channelFillMask) {
}

void ts3plugin_onEditCapturedVoiceDataEvent(uint64 serverConnectionHandlerID, short* samples, int sampleCount, int channels, int* edited) {
}

void ts3plugin_onCustom3dRolloffCalculationClientEvent(uint64 serverConnectionHandlerID, anyID clientID, float distance, float* volume) {
}

void ts3plugin_onCustom3dRolloffCalculationWaveEvent(uint64 serverConnectionHandlerID, uint64 waveHandle, float distance, float* volume) {
}

void ts3plugin_onUserLoggingMessageEvent(const char* logMessage, int logLevel, const char* logChannel, uint64 logID, const char* logTime, const char* completeLogString) {
}

/* Clientlib rare */

void ts3plugin_onClientBanFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, uint64 time, const char* kickMessage) {
}

int ts3plugin_onClientPokeEvent(uint64 serverConnectionHandlerID, anyID fromClientID, const char* pokerName, const char* pokerUniqueIdentity, const char* message, int ffIgnored) {
    anyID myID;

    printf("PLUGIN onClientPokeEvent: %llu %d %s %s %d\n", (long long unsigned int)serverConnectionHandlerID, fromClientID, pokerName, message, ffIgnored);

	/* Check if the Friend/Foe manager has already blocked this poke */
	if(ffIgnored) {
		return 0;  /* Client will block anyways, doesn't matter what we return */
	}

    /* Example code: Send text message back to poking client */
    if(ts3Functions.getClientID(serverConnectionHandlerID, &myID) != ERROR_ok) {  /* Get own client ID */
        ts3Functions.logMessage("Error querying own client id", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
        return 0;
    }
    if(fromClientID != myID) {  /* Don't reply when source is own client */
        if(ts3Functions.requestSendPrivateTextMsg(serverConnectionHandlerID, "Received your poke!", fromClientID, NULL) != ERROR_ok) {
            ts3Functions.logMessage("Error requesting send text message", LogLevel_ERROR, "Plugin", serverConnectionHandlerID);
        }
    }

    return 0;  /* 0 = handle normally, 1 = client will ignore the poke */
}

void ts3plugin_onClientSelfVariableUpdateEvent(uint64 serverConnectionHandlerID, int flag, const char* oldValue, const char* newValue) {
}

void ts3plugin_onFileListEvent(uint64 serverConnectionHandlerID, uint64 channelID, const char* path, const char* name, uint64 size, uint64 datetime, int type, uint64 incompletesize, const char* returnCode) {
}

void ts3plugin_onFileListFinishedEvent(uint64 serverConnectionHandlerID, uint64 channelID, const char* path) {
}

void ts3plugin_onFileInfoEvent(uint64 serverConnectionHandlerID, uint64 channelID, const char* name, uint64 size, uint64 datetime) {
}

void ts3plugin_onServerGroupListEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID, const char* name, int type, int iconID, int saveDB) {
}

void ts3plugin_onServerGroupListFinishedEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onServerGroupByClientIDEvent(uint64 serverConnectionHandlerID, const char* name, uint64 serverGroupList, uint64 clientDatabaseID) {
}

void ts3plugin_onServerGroupPermListEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
}

void ts3plugin_onServerGroupPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID) {
}

void ts3plugin_onServerGroupClientListEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID, uint64 clientDatabaseID, const char* clientNameIdentifier, const char* clientUniqueID) {
}

void ts3plugin_onChannelGroupListEvent(uint64 serverConnectionHandlerID, uint64 channelGroupID, const char* name, int type, int iconID, int saveDB) {
}

void ts3plugin_onChannelGroupListFinishedEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onChannelGroupPermListEvent(uint64 serverConnectionHandlerID, uint64 channelGroupID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
}

void ts3plugin_onChannelGroupPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 channelGroupID) {
}

void ts3plugin_onChannelPermListEvent(uint64 serverConnectionHandlerID, uint64 channelID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
}

void ts3plugin_onChannelPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
}

void ts3plugin_onClientPermListEvent(uint64 serverConnectionHandlerID, uint64 clientDatabaseID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
}

void ts3plugin_onClientPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 clientDatabaseID) {
}

void ts3plugin_onChannelClientPermListEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 clientDatabaseID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
}

void ts3plugin_onChannelClientPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 clientDatabaseID) {
}

void ts3plugin_onClientChannelGroupChangedEvent(uint64 serverConnectionHandlerID, uint64 channelGroupID, uint64 channelID, anyID clientID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
}

int ts3plugin_onServerPermissionErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, unsigned int failedPermissionID) {
	return 0;  /* See onServerErrorEvent for return code description */
}

void ts3plugin_onPermissionListGroupEndIDEvent(uint64 serverConnectionHandlerID, unsigned int groupEndID) {
}

void ts3plugin_onPermissionListEvent(uint64 serverConnectionHandlerID, unsigned int permissionID, const char* permissionName, const char* permissionDescription) {
}

void ts3plugin_onPermissionListFinishedEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onPermissionOverviewEvent(uint64 serverConnectionHandlerID, uint64 clientDatabaseID, uint64 channelID, int overviewType, uint64 overviewID1, uint64 overviewID2, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
}

void ts3plugin_onPermissionOverviewFinishedEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onServerGroupClientAddedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientName, const char* clientUniqueIdentity, uint64 serverGroupID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
}

void ts3plugin_onServerGroupClientDeletedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientName, const char* clientUniqueIdentity, uint64 serverGroupID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
}

void ts3plugin_onClientNeededPermissionsEvent(uint64 serverConnectionHandlerID, unsigned int permissionID, int permissionValue) {
}

void ts3plugin_onClientNeededPermissionsFinishedEvent(uint64 serverConnectionHandlerID) {
}

void ts3plugin_onFileTransferStatusEvent(anyID transferID, unsigned int status, const char* statusMessage, uint64 remotefileSize, uint64 serverConnectionHandlerID) {
}

void ts3plugin_onClientChatClosedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientUniqueIdentity) {
}

void ts3plugin_onClientChatComposingEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientUniqueIdentity) {
}

void ts3plugin_onServerLogEvent(uint64 serverConnectionHandlerID, const char* logMsg) {
}

void ts3plugin_onServerLogFinishedEvent(uint64 serverConnectionHandlerID, uint64 lastPos, uint64 fileSize) {
}

void ts3plugin_onMessageListEvent(uint64 serverConnectionHandlerID, uint64 messageID, const char* fromClientUniqueIdentity, const char* subject, uint64 timestamp, int flagRead) {
}

void ts3plugin_onMessageGetEvent(uint64 serverConnectionHandlerID, uint64 messageID, const char* fromClientUniqueIdentity, const char* subject, const char* message, uint64 timestamp) {
}

void ts3plugin_onClientDBIDfromUIDEvent(uint64 serverConnectionHandlerID, const char* uniqueClientIdentifier, uint64 clientDatabaseID) {
}

void ts3plugin_onClientNamefromUIDEvent(uint64 serverConnectionHandlerID, const char* uniqueClientIdentifier, uint64 clientDatabaseID, const char* clientNickName) {
}

void ts3plugin_onClientNamefromDBIDEvent(uint64 serverConnectionHandlerID, const char* uniqueClientIdentifier, uint64 clientDatabaseID, const char* clientNickName) {
}

void ts3plugin_onComplainListEvent(uint64 serverConnectionHandlerID, uint64 targetClientDatabaseID, const char* targetClientNickName, uint64 fromClientDatabaseID, const char* fromClientNickName, const char* complainReason, uint64 timestamp) {
}

void ts3plugin_onBanListEvent(uint64 serverConnectionHandlerID, uint64 banid, const char* ip, const char* name, const char* uid, uint64 creationTime, uint64 durationTime, const char* invokerName,
							  uint64 invokercldbid, const char* invokeruid, const char* reason, int numberOfEnforcements, const char* lastNickName) {
}

void ts3plugin_onClientServerQueryLoginPasswordEvent(uint64 serverConnectionHandlerID, const char* loginPassword) {
}

void ts3plugin_onPluginCommandEvent(uint64 serverConnectionHandlerID, const char* pluginName, const char* pluginCommand) {
	printf("ON PLUGIN COMMAND: %s %s\n", pluginName, pluginCommand);
}

void ts3plugin_onIncomingClientQueryEvent(uint64 serverConnectionHandlerID, const char* commandText) {
}

void ts3plugin_onServerTemporaryPasswordListEvent(uint64 serverConnectionHandlerID, const char* clientNickname, const char* uniqueClientIdentifier, const char* description, const char* password, uint64 timestampStart, uint64 timestampEnd, uint64 targetChannelID, const char* targetChannelPW) {
}

/* Client UI callbacks */

/*
 * Called from client when an avatar image has been downloaded to or deleted from cache.
 * This callback can be called spontaneously or in response to ts3Functions.getAvatar()
 */
void ts3plugin_onAvatarUpdated(uint64 serverConnectionHandlerID, anyID clientID, const char* avatarPath) {
	/* If avatarPath is NULL, the avatar got deleted */
	/* If not NULL, avatarPath contains the path to the avatar file in the TS3Client cache */
	if(avatarPath != NULL) {
		printf("onAvatarUpdated: %llu %d %s\n", (long long unsigned int)serverConnectionHandlerID, clientID, avatarPath);
	} else {
		printf("onAvatarUpdated: %llu %d - deleted\n", (long long unsigned int)serverConnectionHandlerID, clientID);
	}
}

/*
 * Called when a plugin menu item (see ts3plugin_initMenus) is triggered. Optional function, when not using plugin menus, do not implement this.
 * 
 * Parameters:
 * - serverConnectionHandlerID: ID of the current server tab
 * - type: Type of the menu (PLUGIN_MENU_TYPE_CHANNEL, PLUGIN_MENU_TYPE_CLIENT or PLUGIN_MENU_TYPE_GLOBAL)
 * - menuItemID: Id used when creating the menu item
 * - selectedItemID: Channel or Client ID in the case of PLUGIN_MENU_TYPE_CHANNEL and PLUGIN_MENU_TYPE_CLIENT. 0 for PLUGIN_MENU_TYPE_GLOBAL.
 */
void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID) {
	printf("PLUGIN: onMenuItemEvent: serverConnectionHandlerID=%llu, type=%d, menuItemID=%d, selectedItemID=%llu\n", (long long unsigned int)serverConnectionHandlerID, type, menuItemID, (long long unsigned int)selectedItemID);
	switch(type) {
		case PLUGIN_MENU_TYPE_GLOBAL:
			/* Global menu item was triggered. selectedItemID is unused and set to zero. */
			switch(menuItemID) {
				//case MENU_ID_GLOBAL_1:
				//	/* Menu global 1 was triggered */
				//	break;
				//case MENU_ID_GLOBAL_2:
				//	/* Menu global 2 was triggered */
				//	break;
				default:
					break;
			}
			break;
		case PLUGIN_MENU_TYPE_CHANNEL:
			/* Channel contextmenu item was triggered. selectedItemID is the channelID of the selected channel */
			switch(menuItemID) {
				case MENU_ID_CHANNEL_1:
					/* Menu channel 1 was triggered */
					printChannelPlayerStats(serverConnectionHandlerID, selectedItemID);
					break;
				//case MENU_ID_CHANNEL_2:
				//	/* Menu channel 2 was triggered */
				//	break;
				//case MENU_ID_CHANNEL_3:
				//	/* Menu channel 3 was triggered */
				//	break;
				default:
					break;
			}
			break;
		case PLUGIN_MENU_TYPE_CLIENT:
			/* Client contextmenu item was triggered. selectedItemID is the clientID of the selected client */
			switch(menuItemID) {
				//case MENU_ID_CLIENT_1:
				//	/* Menu client 1 was triggered */
				//	break;
				//case MENU_ID_CLIENT_2:
				//	/* Menu client 2 was triggered */
				//	break;
				default:
					break;
			}
			break;
		default:
			break;
	}
}

/* Called when client custom nickname changed */
void ts3plugin_onClientDisplayNameChanged(uint64 serverConnectionHandlerID, anyID clientID, const char* displayName, const char* uniqueClientIdentifier) {
}

}