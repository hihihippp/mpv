/*
 * This file is part of MPlayer.
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

#ifndef MPLAYER_VCD_READ_H
#define MPLAYER_VCD_READ_H

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <libavutil/intreadwrite.h>

#include "common/msg.h"
#include "stream.h"
//=================== VideoCD ==========================

typedef struct mp_vcd_priv_st mp_vcd_priv_t;

#ifdef sun
#include <sys/cdio.h>
static int sun_vcd_read(mp_vcd_priv_t*, int*);
#elif defined(__bsdi__)
#include <dvd.h>
#else
#include <linux/cdrom.h>
#endif

struct mp_vcd_priv_st {
  stream_t *stream;
  int fd;
  struct cdrom_tocentry entry;
  char buf[VCD_SECTOR_SIZE];
  struct cdrom_tochdr tochdr;
};

static inline void vcd_set_msf(mp_vcd_priv_t* vcd, unsigned int sect){
  sect += 150;
  vcd->entry.cdte_addr.msf.frame=sect%75;
  sect=sect/75;
  vcd->entry.cdte_addr.msf.second=sect%60;
  sect=sect/60;
  vcd->entry.cdte_addr.msf.minute=sect;
}

static inline unsigned int vcd_get_msf(mp_vcd_priv_t* vcd){
  return vcd->entry.cdte_addr.msf.frame +
        (vcd->entry.cdte_addr.msf.second+
         vcd->entry.cdte_addr.msf.minute*60)*75 - 150;
}

static int vcd_seek_to_track(mp_vcd_priv_t* vcd, int track)
{
  vcd->entry.cdte_format = CDROM_MSF;
  vcd->entry.cdte_track  = track;
  if (ioctl(vcd->fd, CDROMREADTOCENTRY, &vcd->entry)) {
    MP_ERR(vcd->stream, "ioctl dif1: %s\n",strerror(errno));
    return -1;
  }
  return VCD_SECTOR_DATA*vcd_get_msf(vcd);
}

static int vcd_get_track_end(mp_vcd_priv_t* vcd,int track){
  vcd->entry.cdte_format = CDROM_MSF;
  vcd->entry.cdte_track  = track<vcd->tochdr.cdth_trk1?(track+1):CDROM_LEADOUT;
  if (ioctl(vcd->fd, CDROMREADTOCENTRY, &vcd->entry)) {
    MP_ERR(vcd->stream, "ioctl dif2: %s\n",strerror(errno));
    return -1;
  }
  return VCD_SECTOR_DATA*vcd_get_msf(vcd);
}

static mp_vcd_priv_t* vcd_read_toc(stream_t *stream, int fd){
  struct cdrom_tochdr tochdr;
  mp_vcd_priv_t* vcd;
  int i;
  if (ioctl(fd,CDROMREADTOCHDR,&tochdr)==-1) {
    MP_ERR(stream, "read CDROM toc header: %s\n",strerror(errno));
    return NULL;
  }
  for (i=tochdr.cdth_trk0 ; i<=tochdr.cdth_trk1 + 1; i++){
      struct cdrom_tocentry tocentry;

      tocentry.cdte_track  = i<=tochdr.cdth_trk1 ? i : CDROM_LEADOUT;
      tocentry.cdte_format = CDROM_MSF;

      if (ioctl(fd,CDROMREADTOCENTRY,&tocentry)==-1) {
        MP_ERR(stream, "read CDROM toc entry: %s\n",strerror(errno));
        return NULL;
      }

      if (i<=tochdr.cdth_trk1)
      MP_INFO(stream, "track %02d:  adr=%d  ctrl=%d  format=%d  %02d:%02d:%02d  mode: %d\n",
          (int)tocentry.cdte_track,
          (int)tocentry.cdte_adr,
          (int)tocentry.cdte_ctrl,
          (int)tocentry.cdte_format,
          (int)tocentry.cdte_addr.msf.minute,
          (int)tocentry.cdte_addr.msf.second,
          (int)tocentry.cdte_addr.msf.frame,
          (int)tocentry.cdte_datamode
      );

    }
  vcd = malloc(sizeof(mp_vcd_priv_t));
  vcd->stream = stream;
  vcd->fd = fd;
  vcd->tochdr = tochdr;
  return vcd;
}

static int vcd_read(mp_vcd_priv_t* vcd,char *mem){
#ifndef sun
  memcpy(vcd->buf,&vcd->entry.cdte_addr.msf,sizeof(vcd->entry.cdte_addr.msf));
  if(ioctl(vcd->fd,CDROMREADRAW,vcd->buf)==-1) return 0; // EOF?
  memcpy(mem,&vcd->buf[VCD_SECTOR_OFFS],VCD_SECTOR_DATA);
#else
  {
    int offset;
    if (sun_vcd_read(vcd, &offset) <= 0) return 0;
    memcpy(mem,&vcd->buf[offset],VCD_SECTOR_DATA);
  }
#endif

  vcd->entry.cdte_addr.msf.frame++;
  if (vcd->entry.cdte_addr.msf.frame==75){
    vcd->entry.cdte_addr.msf.frame=0;
    vcd->entry.cdte_addr.msf.second++;
    if (vcd->entry.cdte_addr.msf.second==60){
      vcd->entry.cdte_addr.msf.second=0;
      vcd->entry.cdte_addr.msf.minute++;
    }
  }

  return VCD_SECTOR_DATA;
}


#ifdef sun
#include <sys/scsi/generic/commands.h>
#include <sys/scsi/impl/uscsi.h>

#define SUN_XAREAD      1       /*fails on atapi drives*/
#define SUN_MODE2READ   2       /*fails on atapi drives*/
#define SUN_SCSIREAD    3
#define SUN_VCDREAD     SUN_SCSIREAD

static int sun_vcd_read(mp_vcd_priv_t* vcd, int *offset)
{
#if SUN_VCDREAD == SUN_XAREAD
  struct cdrom_cdxa cdxa;
  cdxa.cdxa_addr = vcd_get_msf(vcd);
  cdxa.cdxa_length = 1;
  cdxa.cdxa_data = vcd->buf;
  cdxa.cdxa_format = CDROM_XA_SECTOR_DATA;

  if(ioctl(vcd->fd,CDROMCDXA,&cdxa)==-1) {
    MP_ERR(vcd->stream, "CDROMCDXA: %s\n",strerror(errno));
    return 0;
  }
  *offset = 0;
#elif SUN_VCDREAD == SUN_MODE2READ
  struct cdrom_read cdread;
  cdread.cdread_lba = 4*vcd_get_msf(vcd);
  cdread.cdread_bufaddr = vcd->buf;
  cdread.cdread_buflen = 2336;

  if(ioctl(vcd->fd,CDROMREADMODE2,&cdread)==-1) {
    MP_ERR(vcd->stream, "CDROMREADMODE2: %s\n",strerror(errno));
    return 0;
  }
  *offset = 8;
#elif SUN_VCDREAD == SUN_SCSIREAD
  struct uscsi_cmd sc;
  union scsi_cdb cdb;
  int lba = vcd_get_msf(vcd);
  int blocks = 1;

  memset(&cdb, 0, sizeof(cdb));
  memset(&sc, 0, sizeof(sc));
  cdb.scc_cmd = 0xBE;
  cdb.cdb_opaque[1] = 5 << 2; // mode2 / form2
  AV_WB32(&cdb.cdb_opaque[2], lba);
  AV_WB24(&cdb.cdb_opaque[6], blocks);
  cdb.cdb_opaque[9] = 1 << 4; // user data only
  cdb.cdb_opaque[10] = 0;     // subchannel

  sc.uscsi_cdb = (caddr_t)&cdb;
  sc.uscsi_cdblen = 12;
  sc.uscsi_bufaddr = vcd->buf;
  sc.uscsi_buflen = 2336;
  sc.uscsi_flags = USCSI_ISOLATE | USCSI_READ;
  sc.uscsi_timeout = 20;
  if (ioctl(vcd->fd, USCSICMD, &sc)) {
      MP_ERR(vcd->stream, "USCSICMD: READ CD: %s\n",strerror(errno));
      return -1;
  }
  if (sc.uscsi_status) {
      MP_ERR(vcd->stream, "scsi command failed with status %d\n", sc.uscsi_status);
      return -1;
  }
  *offset = 0;
  return 1;
#else
#error SUN_VCDREAD
#endif
}
#endif  /*sun*/

#endif /* MPLAYER_VCD_READ_H */
