/*
    Copyright (C) 2010  ABRT team
    Copyright (C) 2010  RedHat Inc

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "abrtlib.h"
#include "Settings.h"

#define SECTION_COMMON      "Common"
#define SECTION_CRON        "Cron"

/* Conf file has this format:
 * [ section_name1 ]
 * name1 = value1
 * name2 = value2
 * [ section_name2 ]
 * name = value
 */

/* Static data */
/* Filled by LoadSettings() */

/* map["name"] = "value" strings from [ Common ] section.
 * If the same name found on more than one line,
 * the values are appended, separated by comma: map["name"] = "value1,value2" */
static map_string_t s_mapSectionCommon;
/* ... from [ Cron ] */
static map_string_t s_mapSectionCron;

/* Public data */
/* Written out exactly in this order by SaveSettings() */

/* [ Common ] */
/* one line: "OpenGPGCheck = value" */
bool          g_settings_bOpenGPGCheck = false;
/* one line: "OpenGPGPublicKeys = value1,value2" */
GList *g_settings_setOpenGPGPublicKeys = NULL;
GList *g_settings_setBlackListedPkgs = NULL;
GList *g_settings_setBlackListedPaths = NULL;
char *g_settings_sWatchCrashdumpArchiveDir = NULL;
unsigned int  g_settings_nMaxCrashReportsSize = 1000;
bool          g_settings_bProcessUnpackaged = false;

/* [ Cron ] */
/* many lines, one per key: "map_key = aa_first,bb_first(bb_second),cc_first" */
map_cron_t    g_settings_mapCron;


/*
 * Loading
 */

static GList *parse_list(const char* list)
{
    struct strbuf *item = strbuf_new();
    GList *l = NULL;

    for (unsigned ii = 0; list[ii]; ii++)
    {
        if (list[ii] == ',')
        {
            l = g_list_append(l, xstrdup(item->buf));
            strbuf_clear(item);
        }
        else
            strbuf_append_char(item, list[ii]);
    }

    if (item->len > 0)
            l = g_list_append(l, xstrdup(item->buf));

    strbuf_free(item);
    return l;
}

/* Format: name, name(param),name("param with spaces \"and quotes\"") */
static vector_pair_string_string_t ParseListWithArgs(const char *pValue, int *err)
{
    VERB3 log(" ParseListWithArgs(%s)", pValue);

    vector_pair_string_string_t pluginsWithArgs;
    std::string item;
    std::string action;
    bool is_quote = false;
    bool is_arg = false;
    for (int ii = 0; pValue[ii]; ii++)
    {
        if (is_quote && pValue[ii] == '\\' && pValue[ii + 1])
        {
            ii++;
            item += pValue[ii];
            continue;
        }
        if (pValue[ii] == '"')
        {
            is_quote = !is_quote;
            /*item += pValue[ii]; - wrong! name("param") must be == name(param) */
            continue;
        }
        if (is_quote)
        {
            item += pValue[ii];
            continue;
        }
        if (pValue[ii] == '(')
        {
            if (!is_arg)
            {
                action = item;
                item = "";
                is_arg = true;
            }
            else
            {
                *err = 1;
                error_msg("Parser error: Invalid syntax on column %d in \"%s\"", ii, pValue);
            }

            continue;
        }
        if (pValue[ii] == ')')
        {
            if (is_arg)
            {
                VERB3 log(" adding (%s,%s)", action.c_str(), item.c_str());
                pluginsWithArgs.push_back(make_pair(action, item));
                item = "";
                is_arg = false;
                action = "";
            }
            else
            {
                *err = 1;
                error_msg("Parser error: Invalid syntax on column %d in \"%s\"", ii, pValue);
            }

            continue;
        }
        if (pValue[ii] == ',' && !is_arg)
        {
            if (item != "")
            {
                VERB3 log(" adding (%s,%s)", item.c_str(), "");
                pluginsWithArgs.push_back(make_pair(item, ""));
                item = "";
            }
            continue;
        }
        item += pValue[ii];
    }

    if (is_quote)
    {
        *err = 1;
        error_msg("Parser error: Unclosed quote in \"%s\"", pValue);
    }

    if (is_arg)
    {
        *err = 1;
        error_msg("Parser error: Unclosed argument in \"%s\"", pValue);
    }
    else if (item != "")
    {
        VERB3 log(" adding (%s,%s)", item.c_str(), "");
        pluginsWithArgs.push_back(make_pair(item, ""));
    }
    return pluginsWithArgs;
}

static int ParseCommon()
{
    map_string_t::const_iterator end = s_mapSectionCommon.end();
    map_string_t::const_iterator it = s_mapSectionCommon.find("OpenGPGCheck");
    if (it != end)
    {
        g_settings_bOpenGPGCheck = string_to_bool(it->second.c_str());
    }
    it = s_mapSectionCommon.find("BlackList");
    if (it != end)
    {
        g_settings_setBlackListedPkgs = parse_list(it->second.c_str());
    }
    it = s_mapSectionCommon.find("BlackListedPaths");
    if (it != end)
    {
        g_settings_setBlackListedPaths = parse_list(it->second.c_str());
    }
    it = s_mapSectionCommon.find("WatchCrashdumpArchiveDir");
    if (it != end)
    {
        g_settings_sWatchCrashdumpArchiveDir = xstrdup(it->second.c_str());
    }
    it = s_mapSectionCommon.find("MaxCrashReportsSize");
    if (it != end)
    {
        g_settings_nMaxCrashReportsSize = xatoi_positive(it->second.c_str());
    }
    it = s_mapSectionCommon.find("ProcessUnpackaged");
    if (it != end)
    {
        g_settings_bProcessUnpackaged = string_to_bool(it->second.c_str());
    }
    return 0; /* no error */
}

static int ParseCron()
{
    map_string_t::iterator it = s_mapSectionCron.begin();
    for (; it != s_mapSectionCron.end(); it++)
    {
        int err = 0;
        vector_pair_string_string_t actionsAndReporters = ParseListWithArgs(it->second.c_str(), &err);
        if (err)
            return err;
        g_settings_mapCron[it->first] = actionsAndReporters;
    }
    return 0; /* no error */
}

static void LoadGPGKeys()
{
    FILE *fp = fopen(CONF_DIR"/gpg_keys", "r");
    if (fp)
    {
        /* every line is one key
         * FIXME: make it more robust, it doesn't handle comments
         */
        char *line;
        while ((line = xmalloc_fgetline(fp)) != NULL)
        {
            if (line[0] == '/') // probably the begining of path, so let's handle it as a key
                g_settings_setOpenGPGPublicKeys = g_list_append(g_settings_setOpenGPGPublicKeys, line);
            else
                free(line);
        }
        fclose(fp);
    }
}

/**
 * Reads configuration from file to s_mapSection* static variables.
 * The file must be opened for reading.
 */
static int ReadConfigurationFromFile(FILE *fp)
{
    char *line;
    std::string section;
    int lineno = 0;
    while ((line = xmalloc_fgetline(fp)) != NULL)
    {
        ++lineno;
        bool is_key = true;
        bool is_section = false;
        bool is_quote = false;
        unsigned ii;
        std::string key, value;
        for (ii = 0; line[ii] != '\0'; ii++)
        {
            if (is_quote && line[ii] == '\\' && line[ii + 1])
            {
                value += line[ii];
                ii++;
                value += line[ii];
                continue;
            }
            if (isspace(line[ii]) && !is_quote)
            {
                continue;
            }
            if (line[ii] == '#' && !is_quote && key == "")
            {
                break;
            }
            if (line[ii] == '[' && !is_quote)
            {
                is_section = true;
                section = "";
                continue;
            }
            if (line[ii] == '"')
            {
                is_quote = !is_quote;
                value += line[ii];
                continue;
            }
            if (is_section)
            {
                if (line[ii] == ']')
                    break;
                section += line[ii];
                continue;
            }
            if (line[ii] == '=' && !is_quote)
            {
                is_key = false;
                key = value;
                value = "";
                continue;
            }
            value += line[ii];
        } /* for */

        if (is_quote)
        {
            error_msg("abrt.conf: Invalid syntax on line %d", lineno);
            goto return_error;
        }

        if (is_section)
        {
            if (line[ii] != ']') /* section not closed */
            {
                error_msg("abrt.conf: Section not closed on line %d", lineno);
                goto return_error;
            }
            goto free_line;
        }

        if (is_key)
        {
            if (!value.empty()) /* the key is stored in value */
            {
                error_msg("abrt.conf: Invalid syntax on line %d", lineno);
                goto return_error;
            }
            goto free_line;
        }
        if (key.empty()) /* A line without key: " = something" */
        {
            error_msg("abrt.conf: Invalid syntax on line %d", lineno);
            goto return_error;
        }

        if (section == SECTION_COMMON)
        {
            if (s_mapSectionCommon[key] != "")
                s_mapSectionCommon[key] += ",";
            s_mapSectionCommon[key] += value;
        }
        else if (section == SECTION_CRON)
        {
            if (s_mapSectionCron[key] != "")
                s_mapSectionCron[key] += ",";
            s_mapSectionCron[key] += value;
        }
        else
        {
            error_msg("abrt.conf: Ignoring entry in invalid section [%s]", section.c_str());
 return_error:
            free(line);
            return 1; /* error */
        }
 free_line:
        free(line);
    } /* while */

    return 0; /* success */
}

/* abrt daemon loads .conf file */
int LoadSettings()
{
    int err = 0;

    FILE *fp = fopen(CONF_DIR"/abrt.conf", "r");
    if (fp)
    {
        err = ReadConfigurationFromFile(fp);
        fclose(fp);
    }
    else
        error_msg("Unable to read configuration file %s", CONF_DIR"/abrt.conf");

    if (err == 0)
        err = ParseCommon();
    if (err == 0)
        err = ParseCron();

    if (err == 0)
    {
        /*
         * loading gpg keys will invoke rpm_load_gpgkey() from rpm.cpp
         * pgpReadPkts which makes nss to re-init and thus makes
         * bugzilla plugin work :-/
         */
        //FIXME FIXME FIXME FIXME FIXME FIXME!!!
        //if (g_settings_bOpenGPGCheck)
        LoadGPGKeys();
    }

    return err;
}

/* dbus call to retrieve .conf file data from daemon */
map_abrt_settings_t GetSettings()
{
    map_abrt_settings_t ABRTSettings;

    ABRTSettings[SECTION_COMMON] = s_mapSectionCommon;
    ABRTSettings[SECTION_CRON] = s_mapSectionCron;

    return ABRTSettings;
}

/* dbus call to change some .conf file data */
void SetSettings(const map_abrt_settings_t& pSettings, const char *dbus_sender)
{
    map_abrt_settings_t::const_iterator it = pSettings.find(SECTION_COMMON);
    map_abrt_settings_t::const_iterator end = pSettings.end();
    if (it != end)
    {
        s_mapSectionCommon = it->second;
        ParseCommon();
    }
    it = pSettings.find(SECTION_CRON);
    if (it != end)
    {
        s_mapSectionCron = it->second;
        ParseCron();
    }
}

void settings_free()
{
    for (GList *li = g_settings_setOpenGPGPublicKeys; li != NULL; li = g_list_next(li))
        free((char*)li->data);

    g_list_free(g_settings_setOpenGPGPublicKeys);
    g_settings_setOpenGPGPublicKeys = NULL;

    for (GList *li = g_settings_setBlackListedPkgs; li != NULL; li = g_list_next(li))
        free((char*)li->data);

    g_list_free(g_settings_setBlackListedPkgs);
    g_settings_setBlackListedPkgs = NULL;

    for (GList *li = g_settings_setBlackListedPaths; li != NULL; li = g_list_next(li))
        free((char*)li->data);

    g_list_free(g_settings_setBlackListedPaths);
    g_settings_setBlackListedPaths = NULL;

    free(g_settings_sWatchCrashdumpArchiveDir);
    g_settings_sWatchCrashdumpArchiveDir = NULL;
}
