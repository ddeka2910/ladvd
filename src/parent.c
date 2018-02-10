/*
 * $Id$
 *
 * Copyright (c) 2008, 2009, 2010, 2011
 *      Sten Spans <sten@blinkenlights.nl>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "common.h"
#include "util.h"
#include "proto/protos.h"
#include "parent.h"
#include <sys/select.h>
#include <sys/wait.h>
#include <ctype.h>

#ifdef HAVE_LIBCAP_NG
#include <cap-ng.h>
#elif defined(HAVE_LIBCAP)
#include <sys/prctl.h>
#include <sys/capability.h>
#endif

#ifdef HAVE_NETPACKET_PACKET_H
#include <netpacket/packet.h>
#endif /* HAVE_NETPACKET_PACKET_H */
#ifdef HAVE_NET_IF_DL_H
#include <net/if_dl.h>
#endif /* HAVE_NET_IF_DL_H */

#if HAVE_LINUX_SOCKIOS_H
#include <linux/sockios.h>
#endif /* HAVE_LINUX_SOCKIOS_H */

#if HAVE_ASM_TYPES_H
#include <asm/types.h>
#endif /* HAVE_ASM_TYPES_H */

#if HAVE_LINUX_ETHTOOL_H
#include <linux/ethtool.h>
#endif /* HAVE_LINUX_ETHTOOL_H */
#ifdef HAVE_LIBTEAM
#include <team.h>
#endif /* HAVE_LIBTEAM */

#ifdef HAVE_PCI_PCI_H
#include <pci/pci.h>
#endif /* HAVE_PCI_PCI_H */

#include "filter.h"

#ifdef HAVE_SYSFS
#define SYSFS_CLASS_NET		"/sys/class/net"
#define SYSFS_PCI_DEVICE	"device/device"
#define SYSFS_PCI_VENDOR	"device/vendor"
#define SYSFS_USB_DEVICE	"device/../product"
#define SYSFS_USB_VENDOR	"device/../manufacturer"
#define SYSFS_PATH_MAX		256
#endif /* HAVE_SYSFS */

struct rfdhead rawfds;

static int sock = -1;
int mfd = -1;
int dfd = -1;

extern struct proto protos[];

void parent_init(int reqfd, int msgfd, pid_t child) {

    // events
    struct event ev_cmd, ev_msg;
    struct event ev_sigchld, ev_sigint, ev_sigterm,  ev_sighup;

#ifdef HAVE_LIBCAP
    // capabilities
    cap_t caps;
#endif /* HAVE_LIBCAP */

    // init the queues
    TAILQ_INIT(&rawfds);

    // proctitle
    setproctitle("parent [priv]");

    // setup global sockets
    sock = my_socket(AF_INET, SOCK_DGRAM, 0);
    mfd = msgfd;

    // debug
    if (options & OPT_DEBUG) {
	dfd = STDOUT_FILENO;
	my_pcap_init(dfd);
#if HAVE_LIBCAP_NG
    } else {
	capng_clear(CAPNG_SELECT_BOTH);
	capng_updatev(CAPNG_ADD, CAPNG_EFFECTIVE|CAPNG_PERMITTED, CAP_KILL,
		    CAP_NET_ADMIN, CAP_NET_RAW, CAP_NET_BROADCAST, -1);
	if (capng_apply(CAPNG_SELECT_BOTH) == -1)
	    my_fatal("unable to set capabilities");
#elif HAVE_LIBCAP
    } else {
	// keep CAP_NET_ADMIN
	caps = cap_from_text("cap_net_admin=ep cap_net_raw=ep "
		"cap_net_broadcast=ep cap_kill=ep");

	if (caps == NULL)
	    my_fatale("unable to create capabilities");

	if (cap_set_proc(caps) == -1)
	    my_fatale("unable to set capabilities");

	(void) cap_free(caps);
#endif /* HAVE_LIBCAP */
    }

    // initalize the event library
    event_init();

    // listen for request and messages from the child
    event_set(&ev_cmd, reqfd, EV_READ|EV_PERSIST, (void *)parent_req, NULL);
    event_set(&ev_msg, msgfd, EV_READ|EV_PERSIST, (void *)parent_send, NULL);
    event_add(&ev_cmd, NULL);
    event_add(&ev_msg, NULL);

    // handle signals
    signal_set(&ev_sigchld, SIGCHLD, parent_signal, &child);
    signal_set(&ev_sigint, SIGINT, parent_signal, &child);
    signal_set(&ev_sigterm, SIGTERM, parent_signal, &child);
    signal_set(&ev_sighup, SIGHUP, parent_signal, NULL);
    signal_add(&ev_sigchld, NULL);
    signal_add(&ev_sigint, NULL);
    signal_add(&ev_sigterm, NULL);
    signal_add(&ev_sighup, NULL);

    // make sure the child is still running
    if (waitpid(child, NULL, WNOHANG) != 0)
	    exit(EXIT_FAILURE);

    // wait for events
    event_dispatch();

    // not reached
    my_fatal("parent event-loop failed");
}


void parent_signal(int sig, short event, void *pid) {
    int status;

    switch (sig) {
	case SIGINT:
	case SIGTERM:
	case SIGCHLD:
	    if (waitpid(*(pid_t *)pid, &status, WNOHANG) > 0) {
		if (WIFEXITED(status))
		    my_log(CRIT, "child exited with return code %d", 
			    WEXITSTATUS(status));
		if (WIFSIGNALED(status))
		    my_log(CRIT, "child exited on signal %d%s",
			    WTERMSIG(status),
			    WCOREDUMP(status) ? " (core dumped)" : "");
	    } else {
		kill(*(pid_t *)pid, sig);
	    }
	    if (options & OPT_DEBUG)
		my_pcap_close();
	    rfd_closeall(&rawfds);
	    unlink(PACKAGE_SOCKET);
	    my_log(CRIT, "quitting");
	    exit(EXIT_SUCCESS);
	    break;
	case SIGHUP:
	    break;
	default:
	    my_fatal("unexpected signal");
    }
}


void parent_req(int reqfd, short event) {
    struct parent_req mreq = {};
    struct rawfd *rfd;
    ssize_t len;

    // receive request
    len = read(reqfd, &mreq, PARENT_REQ_MAX);

    // check request size
    if (len < PARENT_REQ_MIN || len != PARENT_REQ_LEN(mreq.len))
	my_fatal("invalid request received");

    // validate ifindex
    if (if_indextoname(mreq.index, mreq.name) == NULL) {
	mreq.len = 0;
	goto out;
    }

    // validate request
    if (parent_check(&mreq) != EXIT_SUCCESS)
	my_fatal("invalid request supplied");

    switch (mreq.op) {
	// open socket
	case PARENT_OPEN:
	    if ((rfd = rfd_byindex(&rawfds, mreq.index)) == NULL)
		parent_open(mreq.index, mreq.name);
	    break;
	// close socket
	case PARENT_CLOSE:
	    if ((rfd = rfd_byindex(&rawfds, mreq.index)) != NULL)
		parent_close(rfd);
	    break;
#if HAVE_LINUX_ETHTOOL_H
	// fetch ethtool details
	case PARENT_ETHTOOL_GSET:
	case PARENT_ETHTOOL_GDRV:
	    mreq.len = parent_ethtool(&mreq);
	    break;
#endif /* HAVE_LINUX_ETHTOOL_H */
	// manage interface description
	case PARENT_DESCR:
	case PARENT_ALIAS:
	    mreq.len = parent_descr(&mreq);
	    break;
#ifdef HAVE_SYSFS
	case PARENT_DEVICE:
	    mreq.len = parent_device(&mreq);
	    break;
#endif /* HAVE_SYSFS */
#if defined(HAVE_SYSFS) && defined(HAVE_PCI_PCI_H)
	case PARENT_DEVICE_ID:
	    mreq.len = parent_device_id(&mreq);
	    break;
#endif /* HAVE_SYSFS && HAVE_PCI_PCI_H */
#if defined(HAVE_LIBTEAM)
	case PARENT_TEAMNL:
	    mreq.len = parent_libteam(&mreq);
	    break;
#endif /* HAVE_LIBTEAM */
	// invalid request
	default:
	    my_fatal("invalid request received");
    }

out:
    len = write(reqfd, &mreq, PARENT_REQ_LEN(mreq.len));
    if (len != PARENT_REQ_LEN(mreq.len))
	    my_fatal("failed to return request to child");
}


int parent_check(struct parent_req *mreq) {

    assert(mreq);
    assert(mreq->len <= ETHER_MAX_LEN);
    assert(mreq->op < PARENT_MAX);

    switch (mreq->op) {
	case PARENT_OPEN:
	    return(EXIT_SUCCESS);
	case PARENT_CLOSE:
	    return(EXIT_SUCCESS);
#if HAVE_LINUX_ETHTOOL_H
	case PARENT_ETHTOOL_GSET:
	    assert(mreq->len == sizeof(struct ethtool_cmd));
	    return(EXIT_SUCCESS);
	case PARENT_ETHTOOL_GDRV:
	    assert(mreq->len == sizeof(struct ethtool_drvinfo));
	    return(EXIT_SUCCESS);
#endif /* HAVE_LINUX_ETHTOOL_H */
#if HAVE_LIBTEAM
	case PARENT_TEAMNL:
	    return(EXIT_SUCCESS);
#endif /* HAVE_LIBTEAM */
#if defined(SIOCSIFDESCR) || defined(HAVE_SYSFS)
	case PARENT_DESCR:
	    assert(mreq->len <= IFDESCRSIZE);
	    return(EXIT_SUCCESS);
#endif /* SIOCSIFDESCR */
#ifdef HAVE_SYSFS
	case PARENT_ALIAS:
	case PARENT_DEVICE:
	    return(EXIT_SUCCESS);
#endif /* HAVE_SYSFS */
#if defined(HAVE_SYSFS) && defined(HAVE_PCI_PCI_H)
	case PARENT_DEVICE_ID:
	    return(EXIT_SUCCESS);
#endif /* HAVE_SYSFS && HAVE_PCI_PCI_H */
	default:
	    return(EXIT_FAILURE);
    }

    return(EXIT_FAILURE);
}


void parent_send(int msgfd, short event) {
    struct parent_msg msend = {};
    struct rawfd *rfd = NULL;
    ssize_t len;

    // receive request
    len = read(msgfd, &msend, PARENT_MSG_MAX);

    // check request size
    if (len < PARENT_MSG_MIN || len != PARENT_MSG_LEN(msend.len))
	return;

    if (if_indextoname(msend.index, msend.name) == NULL) {
	my_log(CRIT, "invalid ifindex supplied");
	return;
    }

    assert(msend.len >= (ETHER_MIN_LEN - ETHER_VLAN_ENCAP_LEN));
    assert(msend.proto < PROTO_MAX);
    assert(protos[msend.proto].check(msend.msg, msend.len) != NULL);

    // debug
    if (options & OPT_DEBUG) {
	my_pcap_write(&msend);
	return;
    }

    // create rfd if needed
    if (rfd_byindex(&rawfds, msend.index) == NULL)
	// bail if that fails
	if (parent_open(msend.index, msend.name) < 0)
	    return;

    assert((rfd = rfd_byindex(&rawfds, msend.index)) != NULL);
    len = write(rfd->fd, msend.msg, msend.len);

    // close the socket if the device vanished
    // if needed a new socket will be created on the next run
    if ((len == -1) && ((errno == ENODEV) || (errno == EIO)))
	parent_close(rfd);

    if (len != msend.len)
	my_loge(WARN, "only %zi bytes written", len);

    return;
}


int parent_open(const uint32_t index, const char *name) {
    struct rawfd *rfd = NULL;

    assert(name != NULL);

    rfd = my_malloc(sizeof(struct rawfd));

    rfd->index = index;
    strlcpy(rfd->name, name, IFNAMSIZ);

    rfd->fd = parent_socket(rfd);
    if (rfd->fd < 0) {
	my_log(CRIT, "opening raw socket failed");
	free(rfd);
	return(-1);
    }

    TAILQ_INSERT_TAIL(&rawfds, rfd, entries);

    if (!(options & OPT_RECV) || (options & OPT_DEBUG))
	return(0);

    // register multicast membership
    parent_multi(rfd, protos, 1);

    // listen for received packets
    event_set(&rfd->event, rfd->fd, EV_READ|EV_PERSIST,
	(void *)parent_recv, rfd);
    event_add(&rfd->event, NULL);

    return(0);
}

void parent_close(struct rawfd *rfd) {
    assert(rfd != NULL);

    if ((options & OPT_RECV) && !(options & OPT_DEBUG)) {
	// unregister multicast membership
	parent_multi(rfd, protos, 0);
	// delete event
	event_del(&rfd->event);
    }

    // cleanup
    TAILQ_REMOVE(&rawfds, rfd, entries);
    if (rfd->p_handle)
	pcap_close(rfd->p_handle);
    free(rfd);

    return;
}

#if HAVE_LINUX_ETHTOOL_H
ssize_t parent_ethtool(struct parent_req *mreq) {
    struct ifreq ifr = {};
    struct ethtool_cmd ecmd = {};
    struct ethtool_drvinfo edrvinfo = {};

    assert(mreq != NULL);

    // prepare ifr struct
    strlcpy(ifr.ifr_name, mreq->name, IFNAMSIZ);

    if (mreq->op == PARENT_ETHTOOL_GSET) { 
	ecmd.cmd = ETHTOOL_GSET;
	ifr.ifr_data = (caddr_t)&ecmd;

	if (ioctl(sock, SIOCETHTOOL, &ifr) == -1)
	    return(0);
	memcpy(mreq->buf, &ecmd, sizeof(ecmd));
	return(sizeof(ecmd));
    } else if (mreq->op == PARENT_ETHTOOL_GDRV) { 
	edrvinfo.cmd = ETHTOOL_GDRVINFO;
	ifr.ifr_data = (caddr_t)&edrvinfo;

	if (ioctl(sock, SIOCETHTOOL, &ifr) == -1)
	    return(0);
	memcpy(mreq->buf, &edrvinfo, sizeof(edrvinfo));
	return(sizeof(edrvinfo));
    }

    return(0);
}
#endif /* HAVE_LINUX_ETHTOOL_H */

#if HAVE_LIBTEAM
ssize_t parent_libteam(struct parent_req *mreq) {
    struct team_handle *th;
    struct team_port *port;
    int err, cnt = 0;
    char *mode_name;
    struct parent_team_info pt_info = {};

    th = team_alloc();
    if (!th) {
	my_loge(CRIT, "Team alloc failed for %s", mreq->name);
	return 0;
    }

    err = team_init(th, mreq->index);
    if (err) {
	my_loge(CRIT, "Team init failed for %s", mreq->name);
	goto team_free;
    }

    err = team_get_mode_name(th, &mode_name);
    if (err) {
	my_loge(CRIT, "Team get mode failed for %s", mreq->name);
	goto team_free;
    }

    pt_info.mode = NETIF_BONDING_OTHER;
    if (strcmp("activebackup", mode_name) == 0) {
	pt_info.mode = NETIF_BONDING_FAILOVER;
	team_get_active_port(th, &pt_info.netif_active);
    }

    team_for_each_port(port, th) {
	pt_info.netifs[cnt++] = team_get_port_ifindex(port);
	if (cnt == TEAM_NETIF_CNT)
	    break;
    }

    pt_info.cnt = cnt;
    memcpy(mreq->buf, &pt_info, sizeof(pt_info));

team_free:
    team_free(th);

    return sizeof(pt_info);
}
#endif /* HAVE_LIBTEAM */

ssize_t parent_descr(struct parent_req *mreq) {
#ifdef HAVE_SYSFS
    char path[SYSFS_PATH_MAX];
    struct stat sb;
    ssize_t ret = 0;

    mreq->buf[IFDESCRSIZE] = 0;

    ret = snprintf(path, SYSFS_PATH_MAX,
	    SYSFS_CLASS_NET "/%s/ifalias", mreq->name);

    if (!(ret > 0) || (stat(path, &sb) != 0))
	return(0);

    if (mreq->op == PARENT_DESCR)
	return write_line(path, mreq->buf, strlen(mreq->buf));
    else if (mreq->op == PARENT_ALIAS)
	return read_line(path, mreq->buf, IFDESCRSIZE);

    return(0);
#elif defined(SIOCGIFDESCR)
    struct ifreq ifr = {};
    ssize_t ret = 0;

    assert(mreq != NULL);
    mreq->buf[IFDESCRSIZE] = 0;

    // prepare ifr struct
    strlcpy(ifr.ifr_name, mreq->name, IFNAMSIZ);
#ifndef __FreeBSD__
    ifr.ifr_data = (caddr_t)&mreq->buf;
#else
    ifr.ifr_buffer.buffer = &mreq->buf;
    ifr.ifr_buffer.length = mreq->len;
#endif

    if (ioctl(sock, SIOCSIFDESCR, &ifr) >= 0)
	ret = mreq->len;
    return(ret);
#endif /* SIOCSIFDESCR */

    return(0);
}

#ifdef HAVE_SYSFS
ssize_t parent_device(struct parent_req *mreq) {
    char path[SYSFS_PATH_MAX];
    struct stat sb;
    ssize_t ret = 0;

    ret = snprintf(path, SYSFS_PATH_MAX,
	    SYSFS_CLASS_NET "/%s/device", mreq->name);

    if ((ret > 0) && (stat(path, &sb) == 0))
	return(1);
    else
	return(0);
}
#endif /* HAVE_SYSFS */

#if defined(HAVE_SYSFS) && defined(HAVE_PCI_PCI_H)
ssize_t parent_device_id(struct parent_req *mreq) {
    struct stat sb;
    uint16_t device_id = 0, vendor_id = 0;
    static struct pci_access *pacc = NULL;
    char path[SYSFS_PATH_MAX], id_str[16];
    char sub_path[SYSFS_PATH_MAX] = {}, *sub_base = NULL;
    char vendor_str[32], device_str[32], *s = NULL;
    ssize_t ret = 0;

    // prevent libpci breakage
    if (stat("/proc/bus/pci", &sb) != 0)
	return(0);

    if (!pacc) {
	pacc = pci_alloc();
       if (!pacc)
            return(0);
        pci_init(pacc);
    }

    ret = snprintf(path, SYSFS_PATH_MAX,
	    SYSFS_CLASS_NET "/%s/device/subsystem", mreq->name);

    if (ret == -1 || readlink(path, sub_path, sizeof(sub_path) - 1) == -1)
	return(0);
    if ((sub_base = strrchr(sub_path, '/')) == NULL)
	return(0);

    // For USB devices we use the manufacturer and product strings
    if (strcmp(sub_base, "/usb") == 0) {
	ret = snprintf(path, SYSFS_PATH_MAX,
	    SYSFS_CLASS_NET "/%s/" SYSFS_USB_VENDOR, mreq->name);
	if (ret == -1 || !read_line(path, vendor_str, sizeof(vendor_str)))
	    return(0);

	ret = snprintf(path, SYSFS_PATH_MAX,
	    SYSFS_CLASS_NET "/%s/" SYSFS_USB_DEVICE, mreq->name);
	if (ret == -1 || !read_line(path, device_str, sizeof(device_str)))
	    return(0);

	// these strings seem to have trailing spaces
	s = vendor_str + strlen(vendor_str);
	while (s != vendor_str && s-- && isspace(*s))
	    *s = '\0';
	s = device_str + strlen(device_str);
	while (s != device_str && s-- && isspace(*s))
	    *s = '\0';

	if (snprintf(mreq->buf, sizeof(mreq->buf), "%s (%s)",
		    device_str, vendor_str) < 0)
	    return(0);
	return(strlen(mreq->buf));
    }

    ret = snprintf(path, SYSFS_PATH_MAX,
	    SYSFS_CLASS_NET "/%s/" SYSFS_PCI_VENDOR, mreq->name);
    if (ret == -1 || !read_line(path, id_str, sizeof(id_str)))
	return(0);

    vendor_id = strtoul(id_str, NULL, 16);

    ret = snprintf(path, SYSFS_PATH_MAX,
	    SYSFS_CLASS_NET "/%s/" SYSFS_PCI_DEVICE, mreq->name);
    if (ret == -1 || !read_line(path, id_str, sizeof(id_str)))
	return(0);

    device_id = strtoul(id_str, NULL, 16);

    memset(mreq->buf, 0, sizeof(mreq->buf));
    pci_lookup_name(pacc, mreq->buf, sizeof(mreq->buf),
	    PCI_LOOKUP_VENDOR | PCI_LOOKUP_DEVICE, vendor_id, device_id);

    return(strlen(mreq->buf));
}
#endif /* HAVE_SYSFS && HAVE_PCI_PCI_H */

int parent_socket(struct rawfd *rfd) {
    pcap_t *p_handle = NULL;
    char p_errbuf[PCAP_ERRBUF_SIZE] = {};
    struct bpf_program fprog = {};

    if (options & OPT_DEBUG)
	return(dup(STDIN_FILENO));

    // newer libpcap versions need immediate_mode to work
    // so we use pcap_create/pcap_activate to set this up
#if defined(HAVE_PCAP_CREATE)
    p_handle = pcap_create(rfd->name, p_errbuf);
    if (!p_handle)
	my_fatal("pcap_open for %s failed: %s", rfd->name, p_errbuf);

    if (pcap_set_timeout(p_handle, 0) != 0)
	my_fatal("unable to timeout for %s", rfd->name);

    if (pcap_set_snaplen(p_handle, ETHER_MAX_LEN) != 0)
	my_fatal("unable to configure snaplen for %s", rfd->name);

#if defined(HAVE_PCAP_IMMEDIATE_MODE)
    if (pcap_set_immediate_mode(p_handle, 1) != 0)
	my_fatal("pcap_set_immediate_mode for %s failed", rfd->name);
#endif

    if (pcap_activate(p_handle) != 0) {
	my_log(CRIT, "pcap_activate for %s failed", rfd->name);
	goto failed;
    }

    // on older versions we use pcap_open_live
#else
    p_handle = pcap_open_live(rfd->name, ETHER_MAX_LEN, 0, 10, p_errbuf);
    if (!p_handle) {
	my_log(CRIT, "pcap_open for %s failed: %s", rfd->name, p_errbuf);
	return(-1);
    }
#endif

    // setup bpf receive
    if (options & OPT_RECV) {
	fprog.bf_insns = proto_filter; 
	fprog.bf_len = sizeof(proto_filter) / sizeof(struct bpf_insn);
    } else {
	fprog.bf_insns = reject_filter; 
	fprog.bf_len = sizeof(reject_filter) / sizeof(struct bpf_insn);
    }

    // install bpf filter
    if (pcap_setfilter(p_handle, &fprog) != 0) {
	my_log(CRIT, "unable to configure socket filter for %s", rfd->name);
	goto failed;
    }

    if (pcap_setnonblock(p_handle, 1, p_errbuf) < 0) {
	my_log(CRIT, "pcap_setnonblock for %s failed: %s", rfd->name, p_errbuf);
	goto failed;
    }

    if (pcap_setdirection(p_handle, PCAP_D_IN) < 0) {
	my_log(CRIT, "pcap_setdirection for %s failed", rfd->name);
	goto failed;
    }

#if defined(__OpenBSD__) || defined(__FreeBSD__)
    // we enable immediate mode to receive packets timely
    int v = 1;
    ioctl(pcap_get_selectable_fd(p_handle), BIOCIMMEDIATE, &v);
#endif

    rfd->p_handle = p_handle;

    return(pcap_get_selectable_fd(p_handle));

failed:
    pcap_close(p_handle);
    return(-1);
}


void parent_multi(struct rawfd *rfd, struct proto *protos, int op) {

#ifdef AF_PACKET
    struct packet_mreq mreq = {};
#endif
#ifdef __FreeBSD__
    struct sockaddr_dl *saddrdl;
#endif
    struct ifreq ifr = {};

    if (options & OPT_DEBUG)
	return;

    strlcpy(ifr.ifr_name, rfd->name, IFNAMSIZ);

    for (int p = 0; protos[p].name != NULL; p++) {

	// only enabled protos
	if ((protos[p].enabled == 0) && !(options & OPT_AUTO))
	    continue;

#ifdef AF_PACKET
	// prepare a packet_mreq struct
	mreq.mr_ifindex = rfd->index;
	mreq.mr_type = PACKET_MR_MULTICAST;
	mreq.mr_alen = ETHER_ADDR_LEN;
	memcpy(mreq.mr_address, protos[p].dst_addr, ETHER_ADDR_LEN);

	op = (op) ? PACKET_ADD_MEMBERSHIP:PACKET_DROP_MEMBERSHIP;

	if (setsockopt(rfd->fd, SOL_PACKET, op, &mreq, sizeof(mreq)) < 0)
	    my_loge(CRIT, "unable to change %s multicast on %s",
		     protos[p].name, rfd->name);

#elif defined AF_LINK
	// too bad for EDP
	if (!ETHER_IS_MULTICAST(protos[p].dst_addr))
	    continue;

#ifdef __FreeBSD__
	saddrdl = (struct sockaddr_dl *)&ifr.ifr_addr;
	saddrdl->sdl_family = AF_LINK;
	saddrdl->sdl_index = 0;
	saddrdl->sdl_len = sizeof(struct sockaddr_dl);
	saddrdl->sdl_alen = ETHER_ADDR_LEN;
	saddrdl->sdl_nlen = 0;
	saddrdl->sdl_slen = 0;
	memcpy(LLADDR(saddrdl), protos[p].dst_addr, ETHER_ADDR_LEN);
#else
	ifr.ifr_addr.sa_family = AF_UNSPEC;
	memcpy(ifr.ifr_addr.sa_data, protos[p].dst_addr, ETHER_ADDR_LEN);
#endif
	if ((ioctl(sock, (op) ? SIOCADDMULTI: SIOCDELMULTI, &ifr) < 0) &&
	    (errno != EADDRINUSE) && (p != PROTO_CDP1))
	    my_loge(CRIT, "unable to change %s multicast on %s",
		     protos[p].name, rfd->name);
#endif
    }
}


void parent_recv(int fd, short event, struct rawfd *rfd) {
    // packet
    struct parent_msg mrecv = {};
    struct pcap_pkthdr p_pkthdr = {};
    const unsigned char *data = NULL;
    struct ether_hdr *ether;
    static unsigned int rcount = 0;
    int p;
    ssize_t len = 0;

    assert(rfd);
    assert(rfd->p_handle);

    while ((data = pcap_next(rfd->p_handle, &p_pkthdr)) != NULL) {

	// with valid sizes
	if (p_pkthdr.caplen < ETHER_MAX_LEN)
	    mrecv.len = p_pkthdr.caplen;
	else
	    mrecv.len = ETHER_MAX_LEN;

	memcpy(mrecv.msg, data, mrecv.len);

	// skip small packets
        if (mrecv.len < (ETHER_MIN_LEN - ETHER_VLAN_ENCAP_LEN))
	    continue;

	// note the ifindex
	mrecv.index = rfd->index;

	ether = (struct ether_hdr *)mrecv.msg;
	// detect the protocol
	for (p = 0; protos[p].name != NULL; p++) {
	    if (memcmp(protos[p].dst_addr, ether->dst, ETHER_ADDR_LEN) != 0)
		continue;

	    mrecv.proto = p;
	    break;
	}

	if (protos[p].name == NULL) {
	    my_log(INFO, "unknown message type received");
	    return;
	}
	my_log(INFO, "received %s message (%zu bytes)",
		protos[p].name, mrecv.len);

	len = write(mfd, &mrecv, PARENT_MSG_LEN(mrecv.len));
	if (len != PARENT_MSG_LEN(mrecv.len))
	    my_fatal("failed to send message to child");
	rcount++;
    }
}


