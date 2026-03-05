
/*
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    Copyright (c) 2021, Aon plc
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *
 *    modem_main.c  --  modem main func.
 *
 *    Author: Sasha K (sashak@smlink.com)
 *
 *
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <signal.h>
#include <limits.h>
#include <grp.h>
#include <pwd.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define ENOIOCTLCMD 515

#ifdef SUPPORT_ALSA
#define ALSA_PCM_NEW_HW_PARAMS_API 1
#define ALSA_PCM_NEW_SW_PARAMS_API 1
#include <alsa/asoundlib.h>
/* buffer size in periods */
#define BUFFER_PERIODS		12
#define SHORT_BUFFER_PERIODS	4
#endif

#include <modem.h>
#include <modem_debug.h>

#define INFO(fmt,args...) fprintf(stderr, fmt , ##args );
#define ERR(fmt,args...) fprintf(stderr, "error: " fmt , ##args );

#define DBG(fmt,args...) dprintf("main: " fmt, ##args)


#define SLMODEMD_USER "nobody"
#define LOCKED_MEM_MIN_KB (8UL * 1024)
#define LOCKED_MEM_MIN    (LOCKED_MEM_MIN_KB * 1024)

#define CLOSE_COUNT_MAX 100

static unsigned long jitter_warn_us = 10000;
static unsigned int io_canary_streak = 3;

static unsigned long time_diff_us(const struct timeval *now,
				  const struct timeval *prev)
{
	long sec = now->tv_sec - prev->tv_sec;
	long usec = now->tv_usec - prev->tv_usec;
	return (unsigned long)(sec * 1000000L + usec);
}

static void load_runtime_trace_config(void)
{
	const char *level_env = getenv("DMODEM_LOG_LEVEL");
	const char *jitter_env = getenv("SLMODEM_JITTER_WARN_US");
	const char *streak_env = getenv("SLMODEM_IO_CANARY_STREAK");
	char *endptr;
	unsigned long val;

	if (level_env && *level_env) {
		val = strtoul(level_env, &endptr, 10);
		if (*endptr == '\0')
			modem_debug_level = (unsigned int)val;
	}
	if (jitter_env && *jitter_env) {
		val = strtoul(jitter_env, &endptr, 10);
		if (*endptr == '\0' && val > 0)
			jitter_warn_us = val;
	}
	if (streak_env && *streak_env) {
		val = strtoul(streak_env, &endptr, 10);
		if (*endptr == '\0' && val > 0)
			io_canary_streak = (unsigned int)val;
	}
}


/* modem init externals : FIXME remove it */
extern int  dp_dummy_init(void);
extern void dp_dummy_exit(void);
extern int  dp_sinus_init(void);
extern void dp_sinus_exit(void);
extern int  prop_dp_init(void);
extern void prop_dp_exit(void);
extern int datafile_load_info(char *name,struct dsp_info *info);
extern int datafile_save_info(char *name,struct dsp_info *info);
extern int modem_ring_detector_start(struct modem *m);

/* global config data */
extern const char *modem_dev_name;
extern unsigned int ring_detector;
extern unsigned int need_realtime;
extern const char *modem_group;
extern mode_t modem_perm;
extern unsigned int use_short_buffer;
extern const char *modem_exec;


struct device_struct {
	int num;
	int fd;
	pid_t child_pid;
#ifdef SUPPORT_ALSA
	snd_pcm_t *phandle;
	snd_pcm_t *chandle;
	snd_mixer_t *mhandle;
	snd_mixer_elem_t *hook_off_elem;
	snd_mixer_elem_t *cid_elem;
	snd_mixer_elem_t *speaker_elem;
	unsigned int period;
	unsigned int buf_periods;
	unsigned int started;
#endif
	int delay;
};


static char  inbuf[4096];
static char outbuf[4096];

static void log_fd_errno(const char *op, int fd, size_t requested,
			 ssize_t transferred, int errnum)
{
	ERR("event=syscall_error op=%s fd=%d requested=%zu transferred=%zd errno=%d err=\"%s\"\n",
	    op, fd, requested, transferred, errnum, strerror(errnum));
}


/*
 *    ALSA 'driver'
 *
 */

#ifdef SUPPORT_ALSA

#define INTERNAL_DELAY 40 /* internal device tx/rx delay: should be selfdetectible */

extern unsigned use_alsa;
static snd_output_t *dbg_out = NULL;

static int alsa_mixer_setup(struct device_struct *dev, const char *dev_name)
{
	char card_name[32];
	int card_num = 0;
	const char *p;
	snd_mixer_elem_t *elem;
	int err;

	if((p = strchr(dev_name, ':')))
		card_num = strtoul(p+1, NULL, 0);
	snprintf(card_name, sizeof(card_name), "hw:%d", card_num);
	
	err = snd_mixer_open(&dev->mhandle, 0);
	if(err < 0) {
		DBG("mixer setup: cannot open: %s\n", snd_strerror(err));
		return err;
	}
	err = snd_mixer_attach(dev->mhandle, card_name);
	if (err < 0) {
		ERR("mixer setup: attach %s error: %s\n", card_name, snd_strerror(err));
		goto error;
	}
	err = snd_mixer_selem_register(dev->mhandle, NULL, NULL);
	if (err <0) {
		ERR("mixer setup: register %s error: %s\n", card_name, snd_strerror(err));
		goto error;
	}
	err = snd_mixer_load(dev->mhandle);
	if (err < 0) {
		ERR("mixer setup: load %s error: %s\n", card_name, snd_strerror(err));
		goto error;
	}
	
	for (elem = snd_mixer_first_elem(dev->mhandle) ; elem; elem = snd_mixer_elem_next(elem)) {
		if(strcmp(snd_mixer_selem_get_name(elem),"Off-hook") == 0)
			dev->hook_off_elem = elem;
		else if(strcmp(snd_mixer_selem_get_name(elem),"Caller ID") == 0)
			dev->cid_elem = elem;
		else if(strcmp(snd_mixer_selem_get_name(elem),"Modem Speaker") == 0)
			dev->speaker_elem = elem;
	}

	if(dev->hook_off_elem)
		return 0;

error:
	snd_mixer_close(dev->mhandle);
	dev->mhandle = NULL;
	if (!err) {
		ERR("mixer setup: Off-hook switch not found for card %s\n", card_name);
		err = -ENODEV;
	}
	return err;
}

static int alsa_device_setup(struct device_struct *dev, const char *dev_name)
{
	struct pollfd pfd;
	int ret;
	memset(dev,0,sizeof(*dev));

	ret = alsa_mixer_setup(dev, dev_name);
	if(ret < 0)
		DBG("alsa setup: cannot setup mixer: %s\n", snd_strerror(ret));

	ret = snd_pcm_open(&dev->phandle, dev_name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if(ret < 0) {
		ERR("alsa setup: cannot open playback device '%s': %s\n",
		    dev_name, snd_strerror(ret));
		return -1;
	}
	ret = snd_pcm_open(&dev->chandle, dev_name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
	if(ret < 0) {
		ERR("alsa setup: cannot open playback device '%s': %s\n",
		    dev_name, snd_strerror(ret));
		return -1;
	}
	ret = snd_pcm_poll_descriptors(dev->chandle, &pfd, 1);
	if(ret <= 0) {
		ERR("alsa setup: cannot get poll descriptors of '%s': %s\n",
		    dev_name, snd_strerror(ret));
		return -1;
	}
	dev->fd = pfd.fd;
	dev->num = 0; /* <-- FIXME */

	if(modem_debug_level > 0)
		snd_output_stdio_attach(&dbg_out,stderr,0);

	return 0;
}

static int alsa_device_release(struct device_struct *dev)
{
	snd_pcm_close (dev->phandle);
	snd_pcm_close (dev->chandle);
	if (dev->mhandle) {
		if (dev->hook_off_elem)
			snd_mixer_selem_set_playback_switch_all(dev->hook_off_elem, 0);
		if (dev->cid_elem)
			snd_mixer_selem_set_playback_switch_all(dev->cid_elem, 0);
		if (dev->speaker_elem)
			snd_mixer_selem_set_playback_switch_all(dev->speaker_elem, 0);
		snd_mixer_close(dev->mhandle);
	}
	return 0;
}


static int alsa_xrun_recovery(struct device_struct *dev)
{
	int err;
	int len;
	DBG("alsa xrun: try to recover...\n");
	err = snd_pcm_prepare(dev->phandle);
	if (err < 0) {
		ERR("xrun recovery: cannot prepare playback: %s\n", snd_strerror(err));
		return err;
	}
	len = dev->delay - INTERNAL_DELAY;
	snd_pcm_format_set_silence(SND_PCM_FORMAT_S16_LE, outbuf, len);
	err = snd_pcm_writei(dev->phandle, outbuf, len);
	if (err < 0) {
		ERR("xrun recovery: write error: %s\n", snd_strerror(err));
		return err;
	}
	err = snd_pcm_start(dev->chandle);
	if(err < 0) {
		ERR("xrun recovcery snd_pcm_start error: %s\n", snd_strerror(err));
		return err;
	}
	DBG("alsa xrun: recovered.\n");
	return 0;
}


static int alsa_device_read(struct device_struct *dev, char *buf, int count)
{
	int ret;
	do {
		ret = snd_pcm_readi(dev->chandle,buf,count);
		if (ret == -EPIPE) {
			ret = alsa_xrun_recovery(dev);
			break;
		}
	} while (ret == -EAGAIN);
#if 0
	if(ret != dev->period)
		DBG("alsa_device_read (%d): %d ...\n",count,ret);
#endif
	return ret ;
}

static int alsa_device_write(struct device_struct *dev, const char *buf, int count)
{
	int written = 0;
	if(!dev->started)
		return 0;
	while(count > 0) {
		int ret = snd_pcm_writei(dev->phandle,buf,count);
		if(ret < 0) {
			if (ret == -EAGAIN)
				continue;
			if (ret == -EPIPE) {
			    	ret = alsa_xrun_recovery(dev);
			}
			written = ret;
			break;
		}
		count -= ret;
		buf += ret;
		written += ret;
	}
#if 0
	if(written != dev->period)
		DBG("alsa_device_write (%d): %d...\n",asked,written);
#endif
	return written;
}


static snd_pcm_format_t mdm2snd_format(unsigned mdm_format)
{
	if(mdm_format == MFMT_S16_LE)
		return SND_PCM_FORMAT_S16_LE;
	return SND_PCM_FORMAT_UNKNOWN;
}


static int setup_stream(snd_pcm_t *handle, struct modem *m, const char *stream_name)
{
	const struct device_struct *dev = m->dev_data;
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_sw_params_t *sw_params;
	snd_pcm_format_t format;
	unsigned int rate, rrate;
	snd_pcm_uframes_t size, rsize;
	int err;

	err = snd_pcm_hw_params_malloc(&hw_params);
	if (err < 0) {
		ERR("cannot alloc hw params for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	err = snd_pcm_hw_params_any(handle,hw_params);
	if (err < 0) {
		ERR("cannot init hw params for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0) {
		ERR("cannot set access for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	format = mdm2snd_format(m->format);
	if(format == SND_PCM_FORMAT_UNKNOWN) {
		ERR("unsupported format for %s\n",stream_name);
		return -1;
	}
	err = snd_pcm_hw_params_set_format(handle, hw_params, format);
	if (err < 0) {
		ERR("cannot set format for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
        err = snd_pcm_hw_params_set_channels(handle, hw_params, 1);
	if (err < 0) {
		ERR("cannot set channels for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	rrate = rate = m->srate;
	err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rrate, 0);
	if (err < 0) {
		ERR("cannot set rate for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	if ( rrate != rate ) {
		ERR("rate %u is not supported by %s (%u).\n",
		    rate, stream_name, rrate);
		return -1;
	}
	rsize = size = dev->period ;
	err = snd_pcm_hw_params_set_period_size_near(handle, hw_params, &rsize, NULL);
	if (err < 0) {
		ERR("cannot set periods for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	if ( rsize < size ) {
		ERR("period size %ld is not supported by %s (%ld).\n",
		    size, stream_name, rsize);
		return -1;		
	}
	rsize = size = use_short_buffer ? rsize * dev->buf_periods : rsize * 32;
	err = snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &rsize);
	if (err < 0) {
		ERR("cannot set buffer for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	if ( rsize != size ) {
		DBG("buffer size for %s is changed %ld -> %ld\n",
		    stream_name, size, rsize);
	}
	err = snd_pcm_hw_params(handle, hw_params);
	if (err < 0) {
		ERR("cannot setup hw params for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	err = snd_pcm_prepare(handle);
	if (err < 0) {
		ERR("cannot prepare %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	snd_pcm_hw_params_free(hw_params);

	err = snd_pcm_sw_params_malloc(&sw_params);
	if (err < 0) {
		ERR("cannot alloc sw params for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	err = snd_pcm_sw_params_current(handle,sw_params);
	if (err < 0) {
		ERR("cannot get sw params for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	err = snd_pcm_sw_params_set_start_threshold(handle, sw_params, INT_MAX);
	if (err < 0) {
		ERR("cannot set start threshold for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	err = snd_pcm_sw_params_set_avail_min(handle, sw_params, 4);
	if (err < 0) {
		ERR("cannot set avail min for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	err = snd_pcm_sw_params_set_xfer_align(handle, sw_params, 4);
	if (err < 0) {
		ERR("cannot set align for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	err = snd_pcm_sw_params(handle, sw_params);
	if (err < 0) {
		ERR("cannot set sw params for %s: %s\n", stream_name, snd_strerror(err));
		return err;
	}
	snd_pcm_sw_params_free(sw_params);

	if(modem_debug_level > 0)
		snd_pcm_dump(handle,dbg_out);
	return 0;
}

static int alsa_start (struct modem *m)
{
	struct device_struct *dev = m->dev_data;
	int err, len;
	DBG("alsa_start...\n");
	dev->period = m->frag;
	dev->buf_periods = use_short_buffer ? SHORT_BUFFER_PERIODS : BUFFER_PERIODS;
	err = setup_stream(dev->phandle, m, "playback");
	if(err < 0)
		return err;
	err = setup_stream(dev->chandle, m, "capture");
	if(err < 0)
		return err;
	dev->delay = 0;
	len = use_short_buffer ? dev->period * dev->buf_periods : 384;
	DBG("startup write: %d...\n",len);
	err = snd_pcm_format_set_silence(SND_PCM_FORMAT_S16_LE, outbuf, len);
	if(err < 0) {
		ERR("silence error\n");
		return err;
	}
	
	err = snd_pcm_writei(dev->phandle,outbuf,len);
	if(err < 0) {
		ERR("startup write error\n");
		return err;
	}
	dev->delay = err;
	dev->delay += INTERNAL_DELAY; /* <-- fixme: delay detection is needed */
	err = snd_pcm_link(dev->chandle, dev->phandle);
	if(err < 0) {
		ERR("snd_pcm_link error: %s\n", snd_strerror(err));
		return err;
	}
	err = snd_pcm_start(dev->chandle);
	if(err < 0) {
		ERR("snd_pcm_start error: %s\n", snd_strerror(err));
		return err;
	}
	dev->started = 1;
	return 0;
}

static int alsa_stop (struct modem *m)
{
	struct device_struct *dev = m->dev_data;
	DBG("alsa_stop...\n");
	dev->started = 0;
	snd_pcm_drop(dev->chandle);
	snd_pcm_nonblock(dev->phandle, 0);
	snd_pcm_drain(dev->phandle);
	snd_pcm_nonblock(dev->phandle, 1);
	snd_pcm_unlink(dev->chandle);
	snd_pcm_hw_free(dev->phandle);
	snd_pcm_hw_free(dev->chandle);
	return 0;
}

static int alsa_ioctl(struct modem *m, unsigned int cmd, unsigned long arg)
{
	/* TODO */
	struct device_struct *dev = m->dev_data;
	DBG("alsa_ioctl: cmd %x, arg %lx...\n",cmd,arg);
	switch(cmd) {
        case MDMCTL_CAPABILITIES:
                return -EINVAL;
        case MDMCTL_HOOKSTATE:
		return (dev->hook_off_elem) ?
			snd_mixer_selem_set_playback_switch_all(
				dev->hook_off_elem,
				(arg == MODEM_HOOK_OFF) ) : 0 ;
	case MDMCTL_SPEAKERVOL:
		return (dev->speaker_elem) ?
			snd_mixer_selem_set_playback_volume_all(
					dev->speaker_elem, arg) : 0 ;
        case MDMCTL_CODECTYPE:
                return CODEC_SILABS;
        case MDMCTL_IODELAY:
		DBG("delay = %d\n", dev->delay);
		return dev->delay;
	default:
		return 0;
	}
	return -EINVAL;
}


struct modem_driver alsa_modem_driver = {
        .name = "alsa modem driver",
        .start = alsa_start,
        .stop = alsa_stop,
        .ioctl = alsa_ioctl,
};


#endif


/*
 *    Socket driver — used when slmodemd runs as a softmodem with an
 *    external bridge process (slmodem-asterisk-bridge) for audio I/O.
 */

static int socket_stop (struct modem *m)
{
	/* 
	 * To keep the WebSocket connection persistent, we do NOT 
	 * kill the bridge helper on hangup. The bridge itself 
	 * handles the end of media and waits for the next call.
	 */
	DBG("socket_stop (no-op for persistence)...\n");
	return 0;
}

static int socket_start(struct modem *m)
{
	struct device_struct *dev = m->dev_data;
	int ret;
	DBG("socket_start...\n");

	if (dev->fd >= 0) {
		DBG("socket_start: already running (fd %d, pid %d)\n", dev->fd, dev->child_pid);
		return 0;
	}

	/*
	 * Only spawn the bridge when we have an actual dial string (ATDT).
	 * modem_reset() calls modem_start() on every ATZ, but there is
	 * nothing to dial yet — spawning the bridge with an empty dial
	 * string causes it to reject and disconnect, producing NO CARRIER.
	 */
	if (!m->dial_string[0]) {
		DBG("socket_start: no dial string, deferring bridge spawn\n");
		return 0;
	}

	int sockets[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1) {
		log_fd_errno("socketpair", -1, 0, 0, errno);
		return -1;
	}

	/* Bail early if either FD is too high for select()/FD_SET */
	if (sockets[0] >= FD_SETSIZE || sockets[1] >= FD_SETSIZE) {
		ERR("socketpair fd %d/%d >= FD_SETSIZE (%d), too many open fds\n",
		    sockets[0], sockets[1], FD_SETSIZE);
		close(sockets[0]);
		close(sockets[1]);
		return -1;
	}
	pid_t pid = fork();
	if (pid == -1) {
		log_fd_errno("fork", -1, 0, 0, errno);
		close(sockets[0]);
		close(sockets[1]);
		return -1;
	}
	if (pid == 0) { // child
		char str[16];
		snprintf(str,sizeof(str),"%d",sockets[0]);
		close(sockets[1]);
		execl(modem_exec,modem_exec,m->dial_string,str,NULL);
		/* execl only returns on failure */
		perror("execl");
		_exit(127);
	} else {
		char handshake_buf[1];
		close(sockets[0]);
		dev->fd = sockets[1];
		dev->child_pid = pid;

		/*
		 * Wait for the bridge to signal it's connected to Asterisk.
		 * This blocks slmodemd until the WebSocket is actually up.
		 */
		DBG("waiting for bridge handshake...\n");
		if (read(dev->fd, handshake_buf, 1) <= 0) {
			ERR("bridge failed to signal readiness before disconnect\n");
			goto start_fail;
		}
		DBG("bridge handshake received: %c\n", handshake_buf[0]);

		/* Send dial string to the bridge over the socket */
		char dial_cmd[256];
		snprintf(dial_cmd, sizeof(dial_cmd), "DIAL:%s\n", m->dial_string);
		if (write(dev->fd, dial_cmd, strlen(dial_cmd)) < 0) {
			ERR("failed to send dial string to bridge: %s\n", strerror(errno));
			goto start_fail;
		}
		DBG("dial command sent to bridge: %s", dial_cmd);

		dev->delay = 0;
		ret = 192*2;
		memset(outbuf, 0 , ret);
		ret = write(dev->fd, outbuf, ret);
		DBG("done delay thing\n");
		if (ret < 0) {
			log_fd_errno("write", dev->fd, 192U * 2U, 0, errno);
			goto start_fail;
		}
		dev->delay = ret/2;
		return 0;

	start_fail:
		/* Clean up socket and reap child on any failure path. */
		close(dev->fd);
		dev->fd = -1;
		if (dev->child_pid > 0) {
			kill(dev->child_pid, SIGTERM);
			waitpid(dev->child_pid, NULL, 0);
			dev->child_pid = -1;
		}
		return -1;
	}
}

/*
 * socket_ioctl — ioctl handler for the socket-based (softmodem) driver.
 *
 * The driver ioctl interface passes `arg` as a plain integer for most
 * commands (HOOKSTATE, SPEED, SETFRAG, SPEAKERVOL, etc.).  Only GETSTAT
 * passes a *pointer* to an `unsigned` via `(unsigned long)&stat`.
 *
 * A previous version incorrectly cast `arg` to `(unsigned *)` for all
 * grouped commands, causing a SIGSEGV when HOOKSTATE was called with
 * arg=1 (MODEM_HOOK_OFF) — it wrote to memory address 0x1.
 */
static int socket_ioctl(struct modem *m, unsigned int cmd, unsigned long arg)
{
	struct device_struct *dev = m->dev_data;
	DBG("socket_ioctl: cmd=0x%x arg=0x%lx\n", cmd, arg);

	switch (cmd) {
	case MDMCTL_CAPABILITIES:
		return -EINVAL;
	case MDMCTL_CODECTYPE:
		return 4; /* CODEC_STLC7550 */
	case MDMCTL_IODELAY:
		return 0;
	case MDMCTL_SPEAKERVOL:
		return 0;

	/* Commands that receive a plain integer value — accept and ignore. */
	case MDMCTL_HOOKSTATE:
	case MDMCTL_SPEED:
	case MDMCTL_GETFMTS:
	case MDMCTL_SETFMT:
	case MDMCTL_SETFRAGMENT:
	case MDMCTL_START:
	case MDMCTL_STOP:
		return 0;

	case MDMCTL_PULSEDIAL:
		if (dev->fd >= 0) {
			const char *dial = (const char *)arg;
			char dial_cmd[256];
			snprintf(dial_cmd, sizeof(dial_cmd), "DIAL:%s\n", dial);
			if (write(dev->fd, dial_cmd, strlen(dial_cmd)) < 0) return -1;
			DBG("dial command sent to bridge via ioctl: %s", dial_cmd);
		}
		return 0;

	/* GETSTAT is the one command where arg is a pointer to unsigned. */
	case MDMCTL_GETSTAT: {
		unsigned *stat_out = (unsigned *)(uintptr_t)arg;
		assert(stat_out != NULL && "GETSTAT requires non-NULL pointer");
		*stat_out = 0; /* no error, no ring for socket driver */
		return 0;
	}

	default:
		ERR("socket_ioctl: unhandled cmd=0x%x arg=0x%lx\n", cmd, arg);
		return -ENOIOCTLCMD;
	}
}

struct modem_driver socket_modem_driver = {
        .name = "socket driver",
        .start = socket_start,
        .stop = socket_stop,
        .ioctl = socket_ioctl,
};

static int mdm_device_read(struct device_struct *dev, char *buf, int size)
{
	int ret;
	size_t requested = (size_t)size * 2;

	for (;;) {
		ret = read(dev->fd, buf, requested);
		if (ret >= 0)
			break;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return -EAGAIN;
		log_fd_errno("read", dev->fd, requested, 0, errno);
		return -1;
	}
	if (ret == 0) {
		DBG("event=peer_closed op=read fd=%d requested=%zu\n", dev->fd, requested);
		return 0;
	}
	if (ret & 1) {
		ERR("event=unaligned_samples op=read fd=%d bytes=%d\n", dev->fd, ret);
	}
	if (ret > 0) ret /= 2;
	return ret;
}

static int mdm_device_write(struct device_struct *dev, const char *buf, int size)
{
	int ret;
	size_t requested = (size_t)size * 2;

	for (;;) {
		ret = write(dev->fd, buf, requested);
		if (ret >= 0)
			break;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return -EAGAIN;
		log_fd_errno("write", dev->fd, requested, 0, errno);
		return -1;
	}
	if (ret == 0) {
		ERR("event=short_write op=write fd=%d requested=%zu transferred=0\n",
		    dev->fd, requested);
		return 0;
	}
	if (ret & 1) {
		ERR("event=unaligned_samples op=write fd=%d bytes=%d\n", dev->fd, ret);
	}
	if (ret > 0) ret /= 2;
	return ret;
}
static int mdm_device_release(struct device_struct *dev)
{
	if (dev->fd >= 0) {
		close(dev->fd);
		dev->fd = -1;
	}
	/* Reap the bridge child process to avoid zombie accumulation. */
	if (dev->child_pid > 0) {
		kill(dev->child_pid, SIGTERM);
		waitpid(dev->child_pid, NULL, 0);
		dev->child_pid = -1;
	}
	return 0;
}

static int socket_device_setup(struct device_struct *dev, const char *dev_name)
{
	memset(dev,0,sizeof(*dev));
	/* fd=0 (from memset) is stdin — use -1 to mean "no socket yet". */
	dev->fd = -1;
	dev->child_pid = -1;
	return 0;
}


/*
 *    PTY creation (or re-creation)
 *
 */

static char link_name[PATH_MAX];

int create_pty(struct modem *m)
{
	struct termios termios;
	const char *pty_name;
	int pty, ret;

	if(m->pty)
		close(m->pty);

        pty  = getpt();
        if (pty < 0 || grantpt(pty) < 0 || unlockpt(pty) < 0) {
                ERR("getpt: %s\n", strerror(errno));
                return -1;
        }

	if(m->pty) {
		termios = m->termios;
	}
	else {
		ret = tcgetattr(pty, &termios);
		if (ret) {
			ERR("tcgetattr: %s\n", strerror(errno));
			return -1;
		}
		/* non canonical raw tty */
		cfmakeraw(&termios);
		cfsetispeed(&termios, B115200);
		cfsetospeed(&termios, B115200);
	}

        ret = tcsetattr(pty, TCSANOW, &termios);
        if (ret) {
                ERR("tcsetattr: %s\n",strerror(errno));
                return -1;
        }

	fcntl(pty,F_SETFL,O_NONBLOCK);

	pty_name = ptsname(pty);

	m->pty = pty;
	m->pty_name = pty_name;

	modem_update_termios(m,&termios);

	if(modem_group && *modem_group) {
		struct group *grp = getgrnam(modem_group);
		if(!grp) {
			ERR("cannot find group '%s': %s\n", modem_group,
			    strerror(errno));
		}
		else {
			ret = chown(pty_name, -1, grp->gr_gid);
			if(ret < 0) {
				ERR("cannot chown '%s' to ':%s': %s\n",
				    pty_name, modem_group, strerror(errno));
			}
		}
	}

	ret = chmod(pty_name, modem_perm);
	if (ret < 0) {
		ERR("cannot chmod '%s' to %o: %s\n",
		    pty_name, modem_perm, strerror(errno));
	}

	if(*link_name) {
		unlink(link_name);
		if(symlink(pty_name,link_name)) {
			ERR("cannot create symbolink link `%s' -> `%s': %s\n",
			    link_name,pty_name,strerror(errno));
			*link_name = '\0';
		}
		else {
			INFO("symbolic link `%s' -> `%s' created.\n",
			     link_name, pty_name);
		}
	}

	return 0;
}


/*
 *    main run cycle
 *
 */

static int (*device_setup)(struct device_struct *dev, const char *dev_name);
static int (*device_release)(struct device_struct *dev);
static int (*device_read)(struct device_struct *dev, char *buf, int size);
static int (*device_write)(struct device_struct *dev, const char *buf, int size);
static struct modem_driver *modem_driver;

static volatile sig_atomic_t keep_running = 1;

void mark_termination(int signum)
{
	DBG("signal %d: mark termination.\n",signum);
	keep_running = 0;
}


static int modem_run(struct modem *m, struct device_struct *dev)
{
	struct timeval tmo;
	struct timeval now, last_loop = {0, 0};
	fd_set rset,eset;
	struct termios termios;
	unsigned pty_closed = 0, close_count = 0;
	unsigned read_empty_streak = 0, write_full_streak = 0;
	unsigned long jitter_events = 0, max_loop_gap_us = 0;
	int max_fd;
	int ret, count;
	char *in;

	while(keep_running) {
		assert(dev != NULL);
		assert(m != NULL);
		gettimeofday(&now, NULL);
		if (last_loop.tv_sec || last_loop.tv_usec) {
			unsigned long gap = time_diff_us(&now, &last_loop);
			if (gap > max_loop_gap_us)
				max_loop_gap_us = gap;
			if (m->started && gap > jitter_warn_us) {
				jitter_events++;
				if (jitter_events <= 5 || (jitter_events % 100) == 0) {
					DBG("event=jitter gap_us=%lu warn_us=%lu max_gap_us=%lu count=%lu state=0x%x\n",
					    gap, jitter_warn_us, max_loop_gap_us, jitter_events,
					    m->state);
				}
			}
		}
		last_loop = now;

		if(m->event)
			modem_event(m);

#ifdef MODEM_CONFIG_RING_DETECTOR
		if(ring_detector && !m->started)
			modem_ring_detector_start(m);
#endif

                tmo.tv_sec = 1;
                tmo.tv_usec= 0;
                FD_ZERO(&rset);
		FD_ZERO(&eset);
		/*
		 * Guard against dev->fd exceeding FD_SETSIZE. FD_SET on an
		 * out-of-range fd corrupts the fd_set and is undefined behavior.
		 */
		if(dev->fd >= FD_SETSIZE) {
			ERR("dev->fd %d >= FD_SETSIZE (%d)\n", dev->fd, FD_SETSIZE);
			return -1;
		}

		/*
		 * When dev->fd < 0, the bridge socket is not yet established
		 * (bridge not spawned or startup failed). We must still watch
		 * the PTY for AT commands — otherwise the modem becomes deaf
		 * to user input until a bridge connects. The bridge will start
		 * naturally when modem_start() is called during ATDT processing.
		 */
		max_fd = -1;

		if(dev->fd >= 0 && m->started) {
			FD_SET(dev->fd,&rset);
			if(dev->fd > max_fd) max_fd = dev->fd;
		}

		if (dev->fd >= 0 && modem_driver != &socket_modem_driver) {
			FD_SET(dev->fd,&eset);
		}

		if(pty_closed && close_count > 0) {
			if(!m->started ||
				++close_count > CLOSE_COUNT_MAX )
				close_count = 0;
		}
		else if(m->xmit.size - m->xmit.count > 0) {
			if(m->pty >= 0 && m->pty < FD_SETSIZE) {
				FD_SET(m->pty,&rset);
				if(m->pty > max_fd) max_fd = m->pty;
			}
		}

		/* Nothing to watch — sleep briefly to avoid busy-spinning. */
		if(max_fd < 0) {
			tmo.tv_sec = 0;
			tmo.tv_usec = 100000;
			select(0, NULL, NULL, NULL, &tmo);
			continue;
		}

                ret = select(max_fd + 1,&rset,NULL,&eset,&tmo);

                if (ret < 0) {
			if (errno == EINTR)
				continue;
                        ERR("select: %s\n",strerror(errno));
                        return ret;
                }

		if ( ret == 0 )
			continue;

		if(dev->fd >= 0 && FD_ISSET(dev->fd, &eset)) {
			unsigned stat;
			DBG("event=dev_exception fd=%d modem_state=0x%x started=%u\n",
			    dev->fd, m->state, m->started);
#ifdef SUPPORT_ALSA
			if(use_alsa) {
				DBG("dev exception...\n");
				continue;
			}
#endif
			if (modem_driver == &socket_modem_driver) {
				ret = m->driver.ioctl(m, MDMCTL_GETSTAT, (unsigned long)&stat);
			} else {
				ret = ioctl(dev->fd, 100000+MDMCTL_GETSTAT, &stat);
			}
			if(ret < 0) {
				log_fd_errno("ioctl", dev->fd, 0, 0, errno);
				return -1;
			}
			if(stat&MDMSTAT_ERROR) modem_error(m);
			if(stat&MDMSTAT_RING)  modem_ring(m);
			continue;
		}
		if(dev->fd >= 0 && FD_ISSET(dev->fd, &rset)) {
			count = device_read(dev,inbuf,sizeof(inbuf)/2);
			if(count == -EAGAIN) {
				read_empty_streak++;
				if (read_empty_streak >= io_canary_streak) {
					DBG("event=io_canary kind=buffer_empty fd=%d streak=%u state=0x%x\n",
					    dev->fd, read_empty_streak, m->state);
				}
				continue;
			}
			read_empty_streak = 0;
			if(count <= 0) {
				if (count == 0) {
					DBG("event=peer_closed op=read fd=%d modem_state=0x%x\n",
					    dev->fd, m->state);
				} else {
					ERR("event=read_failed fd=%d ret=%d modem_state=0x%x\n",
					    dev->fd, count, m->state);
				}
				// hack to force hangup
				modem_hangup(m); // sets sample_timer_func to run_modem_stop()
				m->sample_timer_func(m);
				m->sample_timer = 0;
				m->sample_timer_func = NULL;
				continue;
			}
			in = inbuf;
			if(m->update_delay < 0) {
				if ( -m->update_delay >= count) {
					DBG("change delay -%d...\n", count);
					dev->delay -= count;
					m->update_delay += count;
					continue;
				}
				DBG("change delay %d...\n", m->update_delay);
				in -= m->update_delay;
				count += m->update_delay;
				dev->delay += m->update_delay;
				m->update_delay = 0;
			}

			modem_process(m,inbuf,outbuf,count);
			if (dev->fd == -1) {
				DBG("closed connection to child socket process\n");
				continue;
			}
			count = device_write(dev,outbuf,count);
			if (count == -EAGAIN) {
				write_full_streak++;
				if (write_full_streak >= io_canary_streak) {
					DBG("event=io_canary kind=buffer_full fd=%d streak=%u state=0x%x\n",
					    dev->fd, write_full_streak, m->state);
				}
				continue;
			}
			write_full_streak = 0;
			if(count < 0) {
				log_fd_errno("write", dev->fd, sizeof(outbuf), 0, errno);
				return -1;
			}
			else if (count == 0) {
				DBG("event=zero_progress op=write fd=%d modem_state=0x%x\n",
				    dev->fd, m->state);
			}

			if(m->update_delay > 0) {
				DBG("change delay +%d...\n", m->update_delay);
				memset(outbuf, 0, m->update_delay*2);
				count = device_write(dev,outbuf,m->update_delay);
				if (count == -EAGAIN) {
					write_full_streak++;
					if (write_full_streak >= io_canary_streak) {
						DBG("event=io_canary kind=buffer_full fd=%d streak=%u state=0x%x\n",
						    dev->fd, write_full_streak, m->state);
					}
					continue;
				}
				write_full_streak = 0;
				if(count < 0) {
					log_fd_errno("write", dev->fd,
						    (size_t)m->update_delay * 2U, 0, errno);
					return -1;
				}
				if(count != m->update_delay) {
					ERR("cannot update delay: %d instead of %d.\n",
					    count, m->update_delay);
					return -1;
				}
				dev->delay += m->update_delay;
				m->update_delay = 0;
			}
		}
		if(FD_ISSET(m->pty,&rset)) {
			/* check termios */
			tcgetattr(m->pty,&termios);
			if(memcmp(&termios,&m->termios,sizeof(termios))) {
				DBG("termios changed.\n");
				modem_update_termios(m,&termios);
			}
			/* read data */
			count = m->xmit.size - m->xmit.count;
			if(count == 0)
				continue;
			if(count > sizeof(inbuf))
				count = sizeof(inbuf);
			count = read(m->pty,inbuf,count);
			if(count < 0) {
				if(errno == EAGAIN) {
					DBG("pty read, errno = EAGAIN\n");
					continue;
				}
				if(errno == EIO) { /* closed */
					if(!pty_closed) {
						DBG("pty closed.\n");
						if(termios.c_cflag&HUPCL) {
							modem_hangup(m);
							/* re-create PTM - simulate hangup */
							ret = create_pty(m);
							if (ret < 0) {
								ERR("cannot re-create PTY.\n");
								return -1;
							}
						}
						else
							pty_closed = 1;
					}
					// DBG("pty read, errno = EIO\n");
					close_count = 1;
					continue;
				}
				else
					ERR("pty read: %s\n",strerror(errno));
				return -1;
			}
			else if (count == 0) {
				DBG("pty read = 0\n");
			}
			pty_closed = 0;
			count = modem_write(m,inbuf,count);
			if(count < 0) {
				ERR("modem_write failed.\n");
				return -1;
			}
		}
	}

	return 0;
}


int modem_main(const char *dev_name)
{
	char path_name[PATH_MAX];
	struct device_struct device;
	struct modem *m;
	int pty;
	int ret = 0;
	struct passwd *pwd;
	int fd;

	/* Close any inherited file descriptors above stderr to keep
	   FD numbers low (below FD_SETSIZE) for select()/FD_SET. */
	for (fd = 3; fd < FD_SETSIZE; fd++)
		close(fd);

	modem_debug_init(basename(dev_name));

	ret = device_setup(&device, dev_name);
	if (ret) {
		ERR("cannot setup device `%s'\n", dev_name);
		exit(-1);
	}

	dp_dummy_init();
	dp_sinus_init();
	prop_dp_init();
	modem_timer_init();

	snprintf(link_name, sizeof(link_name), "/dev/ttySL%d", device.num);

	m = modem_create(modem_driver,basename(dev_name));
	m->name = basename(dev_name);
	m->dev_data = &device;
	m->dev_name = dev_name;
	
	ret = create_pty(m);
	if(ret < 0) {
		ERR("cannot create PTY.\n");
		exit(-1);
	}

	INFO("modem `%s' created. TTY is `%s'\n",
	     m->name, m->pty_name);

	snprintf(path_name, sizeof(path_name), "/var/lib/slmodem/data.%s",basename(dev_name));
	datafile_load_info(path_name,&m->dsp_info);

	if (need_realtime) {
		struct sched_param prm;
		if(mlockall(MCL_CURRENT|MCL_FUTURE)) {
			ERR("mlockall: %s (continuing without memory locking)\n",strerror(errno));
		}
		prm.sched_priority = sched_get_priority_max(SCHED_FIFO);
		if(sched_setscheduler(0,SCHED_FIFO,&prm)) {
			DBG("sched_setscheduler FIFO failed: %s, trying RR...\n",strerror(errno));
			if(sched_setscheduler(0,SCHED_RR,&prm)) {
				ERR("sched_setscheduler RR failed: %s (continuing with normal priority)\n",strerror(errno));
			} else {
				DBG("rt applied: SCHED_RR, pri %d\n",prm.sched_priority);
			}
		} else {
			DBG("rt applied: SCHED_FIFO, pri %d\n",prm.sched_priority);
		}
	}

	signal(SIGINT, mark_termination);
	signal(SIGTERM, mark_termination);

#ifdef SLMODEMD_USER
	if (need_realtime) {
		struct rlimit limit;
		if (getrlimit(RLIMIT_MEMLOCK, &limit)) {
			ERR("getrlimit failed to read RLIMIT_MEMLOCK\n");
			exit(-1);
		}
		if (limit.rlim_cur != RLIM_INFINITY &&
			limit.rlim_cur < LOCKED_MEM_MIN) {
			ERR("locked memory limit too low:\n");
			ERR("need %lu bytes, have %lu bytes\n", LOCKED_MEM_MIN,
				(unsigned long)limit.rlim_cur);
			ERR("try 'ulimit -l %lu'\n", LOCKED_MEM_MIN_KB);
			exit(-1);
		}
	}

	pwd = getpwnam(SLMODEMD_USER);
	if (!pwd) {
		ERR("getpwnam " SLMODEMD_USER ": %s\n",strerror(errno));
		exit(-1);
	}

	ret = (setgroups(1,&pwd->pw_gid) ||
	       setgid(pwd->pw_gid) ||
	       setuid(pwd->pw_uid));
	if (ret) {
		ERR("setgroups or setgid %ld or setuid %ld failed: %s\n",
		    (long)pwd->pw_gid,(long)pwd->pw_uid,strerror(errno));
		exit(-1);
	}

	if (setuid(0) != -1) {
		ERR("setuid 0 succeeded after dropping privileges!\n");
		exit(-1);
	}
	DBG("dropped privileges to %ld.%ld\n",
	    (long)pwd->pw_gid,(long)pwd->pw_uid);
#endif

	INFO("Use `%s' as modem device, Ctrl+C for termination.\n",
	     *link_name ? link_name : m->pty_name);

	/*
	 * Do NOT pre-start the bridge here. modem_start() sets up full call
	 * state (hook off, DSP, error correction) which is wrong at idle.
	 * The bridge spawns naturally when modem_start() is called during
	 * ATDT processing. This avoids the startup race where the bridge
	 * tries to connect to Asterisk ARI before Asterisk is ready.
	 */

	/* main loop here */
	ret = modem_run(m,&device);

	datafile_save_info(path_name,&m->dsp_info);

	pty = m->pty;
	modem_delete(m);

	usleep(100000);
	close(pty);
	if(*link_name)
		unlink(link_name);
	
	dp_dummy_exit();
	dp_sinus_exit();
	prop_dp_exit();

	device_release(&device);

	modem_debug_exit();

	exit(ret);
	return 0;
}




int main(int argc, char *argv[])
{
	extern void modem_cmdline(int argc, char *argv[]);
	int ret;
	modem_cmdline(argc,argv);
	load_runtime_trace_config();
	if(!modem_dev_name) modem_dev_name = "/dev/slamr0";

	device_setup = socket_device_setup;
	device_release = mdm_device_release;
	device_read = mdm_device_read;
	device_write = mdm_device_write;
	modem_driver = &socket_modem_driver;

#ifdef SUPPORT_ALSA
	if(use_alsa) {
		device_setup = alsa_device_setup;
		device_release = alsa_device_release;
		device_read = alsa_device_read;
		device_write = alsa_device_write;
		modem_driver = &alsa_modem_driver;
	}
#endif

	ret = modem_main(modem_dev_name);
	return ret;
}
