/*
 * Copyright (C) 2012 The Android Open Source Project
 * Inspired by TinyHW, written by Mark Brown at Wolfson Micro
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_primary"
#define LOG_NDEBUG 0

#include <errno.h>
#include <expat.h>
#include <stdbool.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>

#include <cutils/log.h>

#include <tinyalsa/asoundlib.h>

#define BUF_SIZE 1024
#define MIXER_XML_PATH "/system/etc/mixer_paths_%s.xml"
#define CODEC_CHIP_NAME_PATH "/sys/class/sound/hwC%uD0/chip_name"
#define CODEC_CHIP_NAME_UNKNOWN "unknown"
#define INITIAL_MIXER_PATH_SIZE 8


struct mixer_state {
    struct mixer_ctl *ctl;
    int old_value;
    int new_value;
    int reset_value;
};

struct mixer_setting {
    struct mixer_ctl *ctl;
    int value;
};

struct mixer_path {
    char *name;
    unsigned int size;
    unsigned int length;
    struct mixer_setting *setting;
};

struct audio_route {
    struct mixer *mixer;
    unsigned int num_mixer_ctls;
    struct mixer_state *mixer_state;

    unsigned int mixer_path_size;
    unsigned int num_mixer_paths;
    struct mixer_path *mixer_path;
};

struct config_parse_state {
    struct audio_route *ar;
    struct mixer_path *path;
    int level;
};

/* path functions */

static void path_free(struct audio_route *ar)
{
    unsigned int i;

    for (i = 0; i < ar->num_mixer_paths; i++) {
        if (ar->mixer_path[i].name)
            free(ar->mixer_path[i].name);
        if (ar->mixer_path[i].setting)
            free(ar->mixer_path[i].setting);
    }
    free(ar->mixer_path);
    ar->mixer_path = NULL;
    ar->mixer_path_size = 0;
    ar->num_mixer_paths = 0;
}

static struct mixer_path *path_get_by_name(struct audio_route *ar,
                                           const char *name)
{
    unsigned int i;

    for (i = 0; i < ar->num_mixer_paths; i++)
        if (strcmp(ar->mixer_path[i].name, name) == 0)
            return &ar->mixer_path[i];

    return NULL;
}

static struct mixer_path *path_create(struct audio_route *ar, const char *name)
{
    struct mixer_path *new_mixer_path = NULL;

    if (!ar) {
        ALOGE("invalid audio_route");
        return NULL;
    }

    if (path_get_by_name(ar, name)) {
        ALOGE("Path name '%s' already exists", name);
        return NULL;
    }

    /* check if we need to allocate more space for mixer paths */
    if (ar->mixer_path_size <= ar->num_mixer_paths) {
        if (ar->mixer_path_size == 0)
            ar->mixer_path_size = INITIAL_MIXER_PATH_SIZE;
        else
            ar->mixer_path_size *= 2;

        new_mixer_path = realloc(ar->mixer_path, ar->mixer_path_size *
                                 sizeof(struct mixer_path));
        if (new_mixer_path == NULL) {
            ALOGE("Unable to allocate more paths");
            return NULL;
        } else {
            ar->mixer_path = new_mixer_path;
        }
    }

    /* initialise the new mixer path */
    ar->mixer_path[ar->num_mixer_paths].name = strdup(name);
    if (ar->mixer_path[ar->num_mixer_paths].name == NULL) {
        ALOGE("Unable to allocate more paths");
        return NULL;
    }
    ar->mixer_path[ar->num_mixer_paths].size = 0;
    ar->mixer_path[ar->num_mixer_paths].length = 0;
    ar->mixer_path[ar->num_mixer_paths].setting = NULL;

    /* return the mixer path just added, then increment number of them */
    return &ar->mixer_path[ar->num_mixer_paths++];
}

static bool path_setting_exists(struct mixer_path *path,
                                struct mixer_setting *setting)
{
    unsigned int i;

    for (i = 0; i < path->length; i++)
        if (path->setting[i].ctl == setting->ctl)
            return true;

    return false;
}

static int path_add_setting(struct mixer_path *path,
                            struct mixer_setting *setting)
{
    struct mixer_setting *new_path_setting;

    if (path_setting_exists(path, setting)) {
        ALOGE("Duplicate path setting '%s'",
              mixer_ctl_get_name(setting->ctl));
        return -1;
    }

    /* check if we need to allocate more space for path settings */
    if (path->size <= path->length) {
        if (path->size == 0)
            path->size = INITIAL_MIXER_PATH_SIZE;
        else
            path->size *= 2;

        new_path_setting = realloc(path->setting,
                                   path->size * sizeof(struct mixer_setting));
        if (new_path_setting == NULL) {
            ALOGE("Unable to allocate more path settings");
            return -1;
        } else {
            path->setting = new_path_setting;
        }
    }

    /* initialise the new path setting */
    path->setting[path->length].ctl = setting->ctl;
    path->setting[path->length].value = setting->value;
    path->length++;

    return 0;
}

static int path_add_path(struct mixer_path *path, struct mixer_path *sub_path)
{
    unsigned int i;

    for (i = 0; i < sub_path->length; i++)
        if (path_add_setting(path, &sub_path->setting[i]) < 0)
            return -1;

    return 0;
}

static void path_print(struct mixer_path *path)
{
    unsigned int i;

    ALOGV("Path: %s, length: %d", path->name, path->length);
    for (i = 0; i < path->length; i++)
        ALOGV("  %d: %s -> %d", i, mixer_ctl_get_name(path->setting[i].ctl),
              path->setting[i].value);
}

static int path_apply(struct audio_route *ar, struct mixer_path *path)
{
    unsigned int i;
    unsigned int j;

    for (i = 0; i < path->length; i++) {
        struct mixer_ctl *ctl = path->setting[i].ctl;

        /* locate the mixer ctl in the list */
        for (j = 0; j < ar->num_mixer_ctls; j++) {
            if (ar->mixer_state[j].ctl == ctl)
                break;
        }

        /* apply the new value */
        ar->mixer_state[j].new_value = path->setting[i].value;
    }

    return 0;
}

/* mixer helper function */
static int mixer_enum_string_to_value(struct mixer_ctl *ctl, const char *string)
{
    unsigned int i;

    /* Search the enum strings for a particular one */
    for (i = 0; i < mixer_ctl_get_num_enums(ctl); i++) {
        if (strcmp(mixer_ctl_get_enum_string(ctl, i), string) == 0)
            break;
    }

    return i;
}

static void start_tag(void *data, const XML_Char *tag_name,
                      const XML_Char **attr)
{
    const XML_Char *attr_name = NULL;
    const XML_Char *attr_value = NULL;
    struct config_parse_state *state = data;
    struct audio_route *ar = state->ar;
    unsigned int i;
    struct mixer_ctl *ctl;
    int value;
    struct mixer_setting mixer_setting;
    struct mixer_path *new_mixer_path = NULL;

    /* Get name, type and value attributes (these may be empty) */
    for (i = 0; attr[i]; i += 2) {
        if (strcmp(attr[i], "name") == 0)
            attr_name = attr[i + 1];
        else if (strcmp(attr[i], "value") == 0)
            attr_value = attr[i + 1];
    }

    /* Look at tags */
    if (strcmp(tag_name, "path") == 0) {
        if (attr_name == NULL) {
            ALOGE("Unnamed path!");
        } else {
            if (state->level == 1) {
                /* top level path: create and stash the path */
                new_mixer_path = path_create(ar, (char *)attr_name);
                if (new_mixer_path != NULL)
                    state->path = new_mixer_path;
            } else {
                /* nested path */
                struct mixer_path *sub_path = path_get_by_name(ar, attr_name);
                path_add_path(state->path, sub_path);
            }
        }
    }

    else if (strcmp(tag_name, "ctl") == 0) {
        /* Obtain the mixer ctl and value */
        ctl = mixer_get_ctl_by_name(ar->mixer, attr_name);
        switch (mixer_ctl_get_type(ctl)) {
        case MIXER_CTL_TYPE_BOOL:
        case MIXER_CTL_TYPE_INT:
            value = atoi((char *)attr_value);
            break;
        case MIXER_CTL_TYPE_ENUM:
            value = mixer_enum_string_to_value(ctl, (char *)attr_value);
            break;
        default:
            value = 0;
            break;
        }

        if (state->level == 1) {
            /* top level ctl (initial setting) */

            /* locate the mixer ctl in the list */
            for (i = 0; i < ar->num_mixer_ctls; i++) {
                if (ar->mixer_state[i].ctl == ctl)
                    break;
            }

            if (i < ar->num_mixer_ctls) {
                /* apply the new value */
                ar->mixer_state[i].new_value = value;
            }
        } else {
            /* nested ctl (within a path) */
            mixer_setting.ctl = ctl;
            mixer_setting.value = value;
            path_add_setting(state->path, &mixer_setting);
        }
    }

    state->level++;
}

static void end_tag(void *data, const XML_Char *tag_name)
{
    struct config_parse_state *state = data;

    state->level--;
}

static int alloc_mixer_state(struct audio_route *ar)
{
    unsigned int i;

    ar->num_mixer_ctls = mixer_get_num_ctls(ar->mixer);
    ar->mixer_state = malloc(ar->num_mixer_ctls * sizeof(struct mixer_state));
    if (!ar->mixer_state)
        return -1;

    for (i = 0; i < ar->num_mixer_ctls; i++) {
        ar->mixer_state[i].ctl = mixer_get_ctl(ar->mixer, i);
        /* only get value 0, assume multiple ctl values are the same */
        ar->mixer_state[i].old_value = mixer_ctl_get_value(ar->mixer_state[i].ctl, 0);
        ar->mixer_state[i].new_value = ar->mixer_state[i].old_value;
    }

    return 0;
}

static void free_mixer_state(struct audio_route *ar)
{
    free(ar->mixer_state);
    ar->mixer_state = NULL;
}

void update_mixer_state(struct audio_route *ar)
{
    unsigned int i;
    unsigned int j;

    for (i = 0; i < ar->num_mixer_ctls; i++) {
        /* if the value has changed, update the mixer */
        if (ar->mixer_state[i].old_value != ar->mixer_state[i].new_value) {
            /* set all ctl values the same */
            for (j = 0; j < mixer_ctl_get_num_values(ar->mixer_state[i].ctl); j++)
                mixer_ctl_set_value(ar->mixer_state[i].ctl, j,
                                    ar->mixer_state[i].new_value);
            ar->mixer_state[i].old_value = ar->mixer_state[i].new_value;
        }
    }
}

/* saves the current state of the mixer, for resetting all controls */
static void save_mixer_state(struct audio_route *ar)
{
    unsigned int i;

    for (i = 0; i < ar->num_mixer_ctls; i++) {
        /* only get value 0, assume multiple ctl values are the same */
        ar->mixer_state[i].reset_value = mixer_ctl_get_value(ar->mixer_state[i].ctl, 0);
    }
}

/* this resets all mixer settings to the saved values */
void reset_mixer_state(struct audio_route *ar)
{
    unsigned int i;

    /* load all of the saved values */
    for (i = 0; i < ar->num_mixer_ctls; i++)
        ar->mixer_state[i].new_value = ar->mixer_state[i].reset_value;
}

void audio_route_apply_path(struct audio_route *ar, const char *name)
{
    struct mixer_path *path;

    if (!ar) {
        ALOGE("invalid audio_route");
        return;
    }

    path = path_get_by_name(ar, name);
    if (!path) {
        ALOGE("unable to find path '%s'", name);
        return;
    }

    path_apply(ar, path);
}

int audio_route_control_set_number(unsigned int card_slot, char *control_name,
                                   char *string)
{
    struct mixer *control_mixer;
    struct mixer_ctl *ctl;
    const char *name;
    unsigned int num_ctls, num_values;
    unsigned int i, j;
    enum mixer_ctl_type type;
    int value;
    int ret, mixer_ret;

    control_mixer = mixer_open(card_slot);
    if (!control_mixer) {
        ALOGE("Unable to open the control mixer, aborting.");
        return -1;
    }
    ALOGV("Control mixer open successful.");

    num_ctls = mixer_get_num_ctls(control_mixer);

    ret = 0;
    for (i = 0; i < num_ctls; i++) {
        ctl = mixer_get_ctl(control_mixer, i);
        name = mixer_ctl_get_name(ctl);
        if (name && strcmp(name, control_name) == 0) {
            /* Found the control, update and exit */
            value = atoi(string);
            num_values = mixer_ctl_get_num_values(ctl);
            for (j = 0; j < num_values; j++) {
                mixer_ret = mixer_ctl_set_value(ctl, j, value);
                if (mixer_ret) {
                    ALOGE("Error: invalid value (%s to %d)", name, value);
                    mixer_close(control_mixer);
                    /* Add up the number of failed controller values */
                    ret += -1;
                }
            }
            if (ret == 0)
                ALOGV("Setting %s to int %d", name, value);
            break;
        }
    }

    return ret;
}

int audio_route_control_set_enum(unsigned int card_slot, char *control_name,
                                 char *string)
{
    struct mixer *control_mixer;
    struct mixer_ctl *ctl;
    const char *name;
    unsigned int num_ctls, num_values;
    unsigned int i, j;
    enum mixer_ctl_type type;
    int value;
    int ret, mixer_ret;

    control_mixer = mixer_open(card_slot);
    if (!control_mixer) {
        ALOGE("Unable to open the control mixer, aborting.");
        return -1;
    }
    ALOGV("Control mixer open successful.");

    num_ctls = mixer_get_num_ctls(control_mixer);

    ret = 0;
    for (i = 0; i < num_ctls; i++) {
        ctl = mixer_get_ctl(control_mixer, i);
        name = mixer_ctl_get_name(ctl);
        if (name && strcmp(name, control_name) == 0) {
            /* Found the control, update and exit */
            type = mixer_ctl_get_type(ctl);
            if (type == MIXER_CTL_TYPE_ENUM) {
                if (mixer_ctl_set_enum_by_string(ctl, string)) {
                    ALOGE("Error: invalid enum value");
                    ret = -1;
                } else {
                    ALOGV("Setting %s to string %s", name, string);
                }
            } else {
                ALOGV("Error: only enum types can be set with strings");
                ret = -1;
            }
            break;
        }
    }

    mixer_close(control_mixer);
    return ret;
}

struct audio_route *audio_route_init(unsigned int card_slot)
{
    struct config_parse_state state;
    XML_Parser parser;
    FILE *file;
    int bytes_read;
    void *buf;
    int fd, cnt;
    struct mixer_path *path;
    struct audio_route *ar;
    char   vendor_xml_path[PATH_MAX];
    char   codec_vendor_name[PATH_MAX];
    char   vendor_name[255];
    char  *tmpchar;

    ar = calloc(1, sizeof(struct audio_route));
    if (!ar)
        goto err_calloc;

    ar->mixer = mixer_open(card_slot);
    if (!ar->mixer) {
        ALOGE("Unable to open the mixer, aborting.");
        goto err_mixer_open;
    }
    ALOGV("Mixer open successful.");

    ar->mixer_path = NULL;
    ar->mixer_path_size = 0;
    ar->num_mixer_paths = 0;

    /* allocate space for and read current mixer settings */
    if (alloc_mixer_state(ar) < 0)
        goto err_mixer_state;

    snprintf(codec_vendor_name, sizeof(codec_vendor_name), CODEC_CHIP_NAME_PATH, card_slot);
    fd = open(codec_vendor_name, O_RDONLY);
    if (fd == -1) {
        ALOGE("Failed to open %s", codec_vendor_name);
        /* If no codec name file, then use unknown. */
        strcpy(vendor_name, CODEC_CHIP_NAME_UNKNOWN);
    } else {

        cnt = read(fd, vendor_name, 255);
        if (cnt <= 0) {
            ALOGE("Failed to read vendor name:%s.", vendor_name);
            /* If no codec name file, then use unknown. */
            strcpy(vendor_name, CODEC_CHIP_NAME_UNKNOWN);
        } else {
           vendor_name[cnt-1] = '\0';
        }
        close(fd);
    }
    /* Replace spaces with underscore in vendor name */
    tmpchar = vendor_name;
    while (*tmpchar) {
        if (*tmpchar == ' ')
            *tmpchar = '_';
        tmpchar++;
    }

    sprintf(vendor_xml_path, MIXER_XML_PATH, vendor_name);
    ALOGV("Opening up %s.", vendor_xml_path);
    file = fopen(vendor_xml_path, "r");
    if (!file) {
        ALOGE("Failed to open %s", vendor_xml_path);
        goto err_fopen;
    }

    parser = XML_ParserCreate(NULL);
    if (!parser) {
        ALOGE("Failed to create XML parser");
        goto err_parser_create;
    }

    memset(&state, 0, sizeof(state));
    state.ar = ar;
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, start_tag, end_tag);

    for (;;) {
        buf = XML_GetBuffer(parser, BUF_SIZE);
        if (buf == NULL)
            goto err_parse;

        bytes_read = fread(buf, 1, BUF_SIZE, file);
        if (ferror(file))
            goto err_parse;

        if (XML_ParseBuffer(parser, bytes_read,
                            bytes_read == 0) == XML_STATUS_ERROR) {
            ALOGE("Error in mixer xml (%s)", MIXER_XML_PATH);
            goto err_parse;
        }

        if (bytes_read == 0)
            break;
    }

    /* apply the initial mixer values, and save them so we can reset the
       mixer to the original values */
    update_mixer_state(ar);
    save_mixer_state(ar);

    XML_ParserFree(parser);
    fclose(file);
    return ar;

err_parse:
    XML_ParserFree(parser);
err_parser_create:
    fclose(file);
err_fopen:
    free_mixer_state(ar);
err_mixer_state:
    mixer_close(ar->mixer);
err_mixer_open:
    free(ar);
    ar = NULL;
err_calloc:
    return NULL;
}

void audio_route_free(struct audio_route *ar)
{
    free_mixer_state(ar);
    mixer_close(ar->mixer);
    path_free(ar);
    free(ar);
    ar = NULL;
}
