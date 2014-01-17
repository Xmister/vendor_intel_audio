/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "usb_audio_hw"
//#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <tinyalsa/asoundlib.h>

#include <dirent.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <unistd.h>

#define PCM_DEV_STR "pcm"
#define USB_AUDIO_STR "USB Audio"
#define MAX_PATH_LEN 30

#define NBR_RETRIES 5
#define RETRY_WAIT_USEC 20000

struct pcm_config pcm_config = {
    .channels = 2,
    .rate = 44100,
    .period_size = 1024,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    int card;
    int device;
    bool standby;
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    bool standby;

    struct audio_device *dev;
};

/**
 * NOTE: when multiple mutexes have to be acquired, always respect the
 * following order: hw device > out stream
 */

/* Helper functions */

/* must be called with hw device and output stream mutexes locked */

/*
 * Find highest hw sample-rate
 * or 0 if no match is possible.
 */
static int find_rate(int card_nbr, int device_nbr)
{
    int rate = 0;
    struct pcm_params *params;

    ALOGV("%s enter",__func__);

    params = pcm_params_get(card_nbr, device_nbr, PCM_OUT);
    if (params == NULL) {
        ALOGE("%s - could not get any params for card=%d, device=%d.",__func__, card_nbr, device_nbr);
        ALOGV("%s exit",__func__);
        return rate;
    }

    rate = pcm_params_get_max(params, PCM_PARAM_RATE);

    ALOGV("%s exit",__func__);
    return rate;
}

static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int i;
    int sample_rate;

    ALOGV("%s enter",__func__);

    if ((adev->card < 0) || (adev->device < 0))
        return -EINVAL;

    sample_rate = find_rate(adev->card, adev->device);

    if (sample_rate)
        pcm_config.rate = sample_rate;

    out->pcm = pcm_open(adev->card, adev->device, PCM_OUT, &pcm_config);

    if (out->pcm && !pcm_is_ready(out->pcm)) {
        ALOGE("pcm_open() failed: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        return -ENOMEM;
    }

    ALOGV("%s exit",__func__);
    return 0;
}

/* API functions */

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    return pcm_config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    return pcm_config.period_size *
           pcm_config.period_count *
           audio_stream_frame_size((struct audio_stream *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    ALOGV("%s enter standby = %d",__func__,out->standby);
    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        out->standby = true;
        ALOGV("%s PCM device closed",__func__);
    }

    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    ALOGV("%s exit",__func__);
    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    int routing = 0;

    ALOGV("%s enter",__func__);

    parms = str_parms_create_str(kvpairs);
    pthread_mutex_lock(&adev->lock);

    ret = str_parms_get_str(parms, "card", value, sizeof(value));
    if (ret >= 0)
        adev->card = atoi(value);

    ret = str_parms_get_str(parms, "device", value, sizeof(value));
    if (ret >= 0)
        adev->device = atoi(value);

    pthread_mutex_unlock(&adev->lock);
    str_parms_destroy(parms);

    ALOGV("%s exit",__func__);
    return 0;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    return (pcm_config.period_size * pcm_config.period_count * 1000) /
            out_get_sample_rate(&stream->common);
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret;
    struct stream_out *out = (struct stream_out *)stream;

    ALOGV("%s enter",__func__);

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            goto err;
        }
        out->standby = false;
    }

    if(!out->pcm){
       ALOGD("%s: null handle to write - device already closed",__func__);
       goto err;
    }
    ret = pcm_write(out->pcm, (void *)buffer, bytes);

    ALOGV("%s: pcm_write returned = %d",__func__,ret);

    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    ALOGV("%s exit",__func__);

    return bytes;

err:
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    ALOGV("%s Silence write",__func__);
    if (ret != 0) {
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               out_get_sample_rate(&stream->common));
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

/*
 * Examines a pcm-device file to see if its a USB Audio device and
 * returns its card-number. If no match, returns -1.
 */
static int first_valid_usb_card(char *pcm_name)
{
    int fd;
    char pcm_dev_path[MAX_PATH_LEN];
    struct snd_pcm_info info;

    ALOGV("%s enter",__func__);

    /* If pcm out then filename must end with 0p */
    if (!strstr(pcm_name, "0p")) {
        ALOGV("%s exit",__func__);
        return -1;
    }

    snprintf(pcm_dev_path, sizeof(pcm_dev_path), "/dev/snd/%s", pcm_name);
    fd = open(pcm_dev_path, O_RDONLY);

    if (fd != -1) {
        if (!(ioctl(fd, SNDRV_PCM_IOCTL_INFO, &info))) {
            if (strstr(info.id, USB_AUDIO_STR)) {
                close(fd);
                ALOGV("%s exit",__func__);
                return info.card;
            }
        } else {
            ALOGE("ioctl failed for file: %s", pcm_dev_path);
        }

        close(fd);
    }

    ALOGV("%s exit",__func__);
    return -1;
}

/*
 * Returns the number of the first valid USB Audio card
 * If none is found, returns -1.
 */
static int get_first_usb_card()
{
    DIR *dir;
    struct dirent *de = NULL;
    int card_nr;

    ALOGV("%s enter",__func__);

    dir = opendir("/dev/snd");
    if (dir == NULL) {
        ALOGE("Could not open directory /dev/snd");
        ALOGV("%s exit",__func__);
        return -1;
    }

    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, PCM_DEV_STR, sizeof(PCM_DEV_STR) - 1) == 0) {
            if ((card_nr = first_valid_usb_card(de->d_name)) != -1) {
                closedir(dir);
                ALOGV("%s exit",__func__);
                return card_nr;
            }
        }
    }

    closedir(dir);
    ALOGW("No usb-card found in /dev/snd");
    ALOGV("%s exit",__func__);
    return -1;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int ret;
    int try_time;

    ALOGV("%s enter",__func__);

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->dev = adev;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    out->standby = true;

    /*
     * Expecting an USB-Audio card to be present, but the dev filesystem
     * might not have presented it yet. In that case do some retries.
     */
    for (try_time = 0; try_time < NBR_RETRIES; try_time++) {
        adev->card = get_first_usb_card();
        if (adev->card == -1)
            usleep(RETRY_WAIT_USEC);
        else
            break;
    }
    adev->device = 0;

    *stream_out = &out->stream;
    ALOGV("%s exit",__func__);
    return 0;

err_open:
    ALOGE("%s exit with error",__func__);
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    ALOGV("%s enter",__func__);

    out_standby(&stream->common);
    free(stream);
    ALOGV("%s exit",__func__);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    return 0;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    return -ENOSYS;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    return -ENOSYS;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    return 0;
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    return -ENOSYS;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int ret;

    ALOGV("%s enter",__func__);

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    *device = &adev->hw_device.common;

    ALOGV("%s exit",__func__);

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Intel USB-audio HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
