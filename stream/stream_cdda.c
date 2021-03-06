/*
 * This file is part of MPlayer.
 *
 * Original author: Albeu
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <cdio/cdio.h>

#if CDIO_API_VERSION < 6
#define OLD_API
#endif

#ifdef OLD_API
#include <cdio/cdda.h>
#include <cdio/paranoia.h>
#else
#include <cdio/paranoia/cdda.h>
#include <cdio/paranoia/paranoia.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>

#include "talloc.h"

#include "stream.h"
#include "options/m_option.h"
#include "compat/mpbswap.h"

#include "common/msg.h"

typedef struct {
    cdrom_drive_t *cd;
    cdrom_paranoia_t *cdp;
    int sector;
    int start_sector;
    int end_sector;

    // options
    int speed;
    int paranoia_mode;
    char *generic_dev;
    int sector_size;
    int search_overlap;
    int toc_bias;
    int toc_offset;
    int no_skip;
    char *device;
    int span[2];
} cdda_priv;

static cdda_priv cdda_dflts = {
    .search_overlap = -1,
};

#define OPT_BASE_STRUCT cdda_priv
static const m_option_t cdda_params_fields[] = {
    OPT_INTPAIR("span", span, 0),
    OPT_INTRANGE("speed", speed, 0, 1, 100),
    OPT_STRING("device", device, 0),
    {0}
};

/// We keep these options but now they set the defaults
const m_option_t cdda_opts[] = {
    {"speed", &cdda_dflts.speed, CONF_TYPE_INT, M_OPT_RANGE, 1, 100, NULL},
    {"paranoia", &cdda_dflts.paranoia_mode, CONF_TYPE_INT, M_OPT_RANGE, 0, 2,
     NULL},
    {"generic-dev", &cdda_dflts.generic_dev, CONF_TYPE_STRING, 0, 0, 0, NULL},
    {"sector-size", &cdda_dflts.sector_size, CONF_TYPE_INT, M_OPT_RANGE, 1,
     100, NULL},
    {"overlap", &cdda_dflts.search_overlap, CONF_TYPE_INT, M_OPT_RANGE, 0, 75,
     NULL},
    {"toc-bias", &cdda_dflts.toc_bias, CONF_TYPE_INT, 0, 0, 0, NULL},
    {"toc-offset", &cdda_dflts.toc_offset, CONF_TYPE_INT, 0, 0, 0, NULL},
    {"noskip", &cdda_dflts.no_skip, CONF_TYPE_FLAG, 0, 0, 1, NULL},
    {"skip", &cdda_dflts.no_skip, CONF_TYPE_FLAG, 0, 1, 0, NULL},
    {"device", &cdda_dflts.device, CONF_TYPE_STRING, 0, 0, 0, NULL},
    {"span", &cdda_dflts.span, CONF_TYPE_INT_PAIR, 0, 0, 0, NULL},
    {NULL, NULL, 0, 0, 0, 0, NULL}
};

static const char *cdtext_name[] = {
#ifdef OLD_API
    [CDTEXT_ARRANGER] = "Arranger",
    [CDTEXT_COMPOSER] = "Composer",
    [CDTEXT_MESSAGE]  =  "Message",
    [CDTEXT_ISRC] =  "ISRC",
    [CDTEXT_PERFORMER] = "Performer",
    [CDTEXT_SONGWRITER] =  "Songwriter",
    [CDTEXT_TITLE] =  "Title",
    [CDTEXT_UPC_EAN] = "UPC_EAN",
#else
    [CDTEXT_FIELD_ARRANGER] = "Arranger",
    [CDTEXT_FIELD_COMPOSER] = "Composer",
    [CDTEXT_FIELD_MESSAGE]  =  "Message",
    [CDTEXT_FIELD_ISRC] =  "ISRC",
    [CDTEXT_FIELD_PERFORMER] = "Performer",
    [CDTEXT_FIELD_SONGWRITER] =  "Songwriter",
    [CDTEXT_FIELD_TITLE] =  "Title",
    [CDTEXT_FIELD_UPC_EAN] = "UPC_EAN",
#endif
};

static bool print_cdtext(stream_t *s, int track)
{
    cdda_priv* p = (cdda_priv*)s->priv;
#ifdef OLD_API
    cdtext_t *text = cdio_get_cdtext(p->cd->p_cdio, track);
#else
    cdtext_t *text = cdio_get_cdtext(p->cd->p_cdio);
#endif
    int header = 0;
    if (text) {
        for (int i = 0; i < sizeof(cdtext_name) / sizeof(cdtext_name[0]); i++) {
            const char *name = cdtext_name[i];
#ifdef OLD_API
            const char *value = cdtext_get_const(i, text);
#else
            const char *value = cdtext_get_const(text, i, track);
#endif
            if (name && value) {
                if (!header)
                    MP_INFO(s, "CD-Text (%s):\n", track ? "track" : "CD");
                header = 1;
                MP_INFO(s, "  %s: '%s'\n", name, value);
            }
        }
        return true;
    }
    return false;
}

static void print_track_info(stream_t *s, int track)
{
    MP_INFO(s, "Switched to track %d\n", track);
    print_cdtext(s, track);
}

static void cdparanoia_callback(long int inpos, paranoia_cb_mode_t function)
{
}

static int fill_buffer(stream_t *s, char *buffer, int max_len)
{
    cdda_priv *p = (cdda_priv *)s->priv;
    int16_t *buf;
    int i;

    if (max_len < CDIO_CD_FRAMESIZE_RAW)
        return -1;

    if ((p->sector < p->start_sector) || (p->sector > p->end_sector)) {
        return 0;
    }

    buf = paranoia_read(p->cdp, cdparanoia_callback);
    if (!buf)
        return 0;

#if BYTE_ORDER == BIG_ENDIAN
    for (i = 0; i < CDIO_CD_FRAMESIZE_RAW / 2; i++)
        buf[i] = le2me_16(buf[i]);
#endif

    p->sector++;
    memcpy(buffer, buf, CDIO_CD_FRAMESIZE_RAW);

    for (i = 0; i < p->cd->tracks; i++) {
        if (p->cd->disc_toc[i].dwStartSector == p->sector - 1) {
            print_track_info(s, i + 1);
            break;
        }
    }

    return CDIO_CD_FRAMESIZE_RAW;
}

static int seek(stream_t *s, int64_t newpos)
{
    cdda_priv *p = (cdda_priv *)s->priv;
    int sec;
    int current_track = 0, seeked_track = 0;
    int seek_to_track = 0;
    int i;

    sec = newpos / CDIO_CD_FRAMESIZE_RAW;
    if (newpos < 0 || sec > p->end_sector) {
        p->sector = p->end_sector + 1;
        return 0;
    }

    for (i = 0; i < p->cd->tracks; i++) {
        if (p->sector >= p->cd->disc_toc[i].dwStartSector
            && p->sector < p->cd->disc_toc[i + 1].dwStartSector)
            current_track = i;
        if (sec >= p->cd->disc_toc[i].dwStartSector
            && sec < p->cd->disc_toc[i + 1].dwStartSector)
        {
            seeked_track = i;
            seek_to_track = sec == p->cd->disc_toc[i].dwStartSector;
        }
    }
    if (current_track != seeked_track && !seek_to_track)
        print_track_info(s, seeked_track + 1);

    p->sector = sec;

    paranoia_seek(p->cdp, sec, SEEK_SET);
    return 1;
}

static void close_cdda(stream_t *s)
{
    cdda_priv *p = (cdda_priv *)s->priv;
    paranoia_free(p->cdp);
    cdda_close(p->cd);
    free(p);
}

static int get_track_by_sector(cdda_priv *p, unsigned int sector)
{
    int i;
    for (i = p->cd->tracks; i >= 0; --i)
        if (p->cd->disc_toc[i].dwStartSector <= sector)
            break;
    return i;
}

static int control(stream_t *stream, int cmd, void *arg)
{
    cdda_priv *p = stream->priv;
    switch (cmd) {
    case STREAM_CTRL_GET_NUM_TITLES:
    {
      *(unsigned int *)arg = p->cd->tracks;
      return STREAM_OK;
    }
    case STREAM_CTRL_GET_NUM_CHAPTERS:
    {
        int start_track = get_track_by_sector(p, p->start_sector);
        int end_track = get_track_by_sector(p, p->end_sector);
        if (start_track == -1 || end_track == -1)
            return STREAM_ERROR;
        *(unsigned int *)arg = end_track + 1 - start_track;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_CHAPTER_TIME:
    {
        int track = *(double *)arg;
        int start_track = get_track_by_sector(p, p->start_sector);
        int end_track = get_track_by_sector(p, p->end_sector);
        track += start_track + 1;
        if (track > end_track)
            return STREAM_ERROR;
        int64_t sector = p->cd->disc_toc[track].dwStartSector;
        int64_t pos = sector * (CDIO_CD_FRAMESIZE_RAW + 1) - 1;
        // Assume standard audio CD: 44.1khz, 2 channels, s16 samples
        *(double *)arg = pos / (44100.0 * 2 * 2);
        return STREAM_OK;
    }
    }
    return STREAM_UNSUPPORTED;
}

static int open_cdda(stream_t *st, int m)
{
    cdda_priv *priv = st->priv;
    cdda_priv *p = priv;
    int mode = p->paranoia_mode;
    int offset = p->toc_offset;
    cdrom_drive_t *cdd = NULL;
    int last_track;

    if (m != STREAM_READ) {
        return STREAM_UNSUPPORTED;
    }

    if (!p->device) {
        if (cdrom_device)
            p->device = talloc_strdup(NULL, cdrom_device);
        else
            p->device = talloc_strdup(NULL, DEFAULT_CDROM_DEVICE);
    }

#if defined(__NetBSD__)
    cdd = cdda_identify_scsi(p->device, p->device, 0, NULL);
#else
    cdd = cdda_identify(p->device, 0, NULL);
#endif

    if (!cdd) {
        MP_ERR(st, "Can't open CDDA device.\n");
        return STREAM_ERROR;
    }

    cdda_verbose_set(cdd, CDDA_MESSAGE_FORGETIT, CDDA_MESSAGE_FORGETIT);

    if (p->sector_size)
        cdd->nsectors = p->sector_size;

    if (cdda_open(cdd) != 0) {
        MP_ERR(st, "Can't open disc.\n");
        cdda_close(cdd);
        return STREAM_ERROR;
    }

    priv = malloc(sizeof(cdda_priv));
    memset(priv, 0, sizeof(cdda_priv));
    priv->cd = cdd;

    if (p->toc_bias)
        offset -= cdda_track_firstsector(cdd, 1);

    if (offset) {
        for (int n = 0; n < cdd->tracks + 1; n++)
            cdd->disc_toc[n].dwStartSector += offset;
    }

    if (p->speed > 0)
        cdda_speed_set(cdd, p->speed);

    last_track = cdda_tracks(cdd);
    if (p->span[0] > last_track)
        p->span[0] = last_track;
    if (p->span[1] < p->span[0])
        p->span[1] = p->span[0];
    if (p->span[1] > last_track)
        p->span[1] = last_track;
    if (p->span[0])
        priv->start_sector = cdda_track_firstsector(cdd, p->span[0]);
    else
        priv->start_sector = cdda_disc_firstsector(cdd);

    if (p->span[1])
        priv->end_sector = cdda_track_lastsector(cdd, p->span[1]);
    else
        priv->end_sector = cdda_disc_lastsector(cdd);

    priv->cdp = paranoia_init(cdd);
    if (priv->cdp == NULL) {
        cdda_close(cdd);
        free(priv);
        return STREAM_ERROR;
    }

    if (mode == 0)
        mode = PARANOIA_MODE_DISABLE;
    else if (mode == 1)
        mode = PARANOIA_MODE_OVERLAP;
    else
        mode = PARANOIA_MODE_FULL;

    if (p->no_skip)
        mode |= PARANOIA_MODE_NEVERSKIP;
    else
        mode &= ~PARANOIA_MODE_NEVERSKIP;

    if (p->search_overlap > 0)
        mode |= PARANOIA_MODE_OVERLAP;
    else if (p->search_overlap == 0)
        mode &= ~PARANOIA_MODE_OVERLAP;

    paranoia_modeset(priv->cdp, mode);

    if (p->search_overlap > 0)
        paranoia_overlapset(priv->cdp, p->search_overlap);

    paranoia_seek(priv->cdp, priv->start_sector, SEEK_SET);
    priv->sector = priv->start_sector;

    st->priv = priv;
    st->start_pos = priv->start_sector * CDIO_CD_FRAMESIZE_RAW;
    st->end_pos = (priv->end_sector + 1) * CDIO_CD_FRAMESIZE_RAW;
    st->sector_size = CDIO_CD_FRAMESIZE_RAW;

    st->fill_buffer = fill_buffer;
    st->seek = seek;
    st->control = control;
    st->close = close_cdda;

    st->demuxer = "rawaudio";

    print_cdtext(st, 0);

    return STREAM_OK;
}

const stream_info_t stream_info_cdda = {
    .name = "cdda",
    .open = open_cdda,
    .protocols = (const char*[]){"cdda", NULL },
    .priv_size = sizeof(cdda_priv),
    .priv_defaults = &cdda_dflts,
    .options = cdda_params_fields,
    .url_options = (const char*[]){
        "hostname=span",
        "port=speed",
        "filename=device",
        NULL
    },
};
