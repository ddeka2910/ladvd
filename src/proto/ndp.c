/*
 $Id$
*/

#include "common.h"
#include "util.h"
#include "proto/ndp.h"
#include "proto/tlv.h"


size_t ndp_packet(void *packet, struct netif *netif, struct sysinfo *sysinfo) {

    struct ether_hdr ether;
    struct ether_llc llc;
    struct ndp_header ndp;

    uint8_t *pos = packet;

    struct netif *master;

    const uint8_t ndp_dst[] = NDP_MULTICAST_ADDR;
    const uint8_t llc_org[] = LLC_ORG_NORTEL;

    // fixup master netif
    if (netif->master != NULL)
	master = netif->master;
    else
	master = netif;

    // ethernet header
    memcpy(ether.dst, ndp_dst, ETHER_ADDR_LEN);
    memcpy(ether.src, netif->hwaddr, ETHER_ADDR_LEN);
    pos += sizeof(struct ether_hdr);

    // llc snap header
    llc.dsap = llc.ssap = 0xaa;
    llc.control = 0x03;
    memcpy(llc.org, llc_org, sizeof(llc.org));
    llc.protoid = htons(LLC_PID_NDP_HELLO);
    memcpy(pos, &llc, sizeof(struct ether_llc));
    pos += sizeof(struct ether_llc);

    // ndp header
    memset(&ndp, 0, sizeof(struct ndp_header));
    ndp.addr = master->ipaddr4;
    ndp.seg[2] = netif->index;
    ndp.chassis = NDP_CHASSIS_OTHER;
    ndp.backplane = NDP_BACKPLANE_ETH_FE_GE;
    ndp.links = sysinfo->physif_count;
    ndp.state = NDP_TOPOLOGY_NEW;
    memcpy(pos, &ndp, sizeof(struct ndp_header));
    pos += sizeof(struct ndp_header);


    // ethernet header
    ether.length = htons(VOIDP_DIFF(pos, packet + sizeof(struct ether_hdr)));
    memcpy(packet, &ether, sizeof(struct ether_hdr));

    // packet length
    return(VOIDP_DIFF(pos, packet));
}

char * ndp_check(void *packet, size_t length) {
    struct ether_hdr ether;
    struct ether_llc llc;
    const uint8_t ndp_dst[] = NDP_MULTICAST_ADDR;
    const uint8_t ndp_org[] = LLC_ORG_NORTEL;

    assert(packet);
    assert(length > (sizeof(ether) + sizeof(llc)));
    assert(length <= ETHER_MAX_LEN);

    memcpy(&ether, packet, sizeof(ether));
    memcpy(&llc, packet + sizeof(ether), sizeof(llc));

    if ((memcmp(ether.dst, ndp_dst, ETHER_ADDR_LEN) == 0) &&
	(memcmp(llc.org, ndp_org, sizeof(llc.org)) == 0) &&
	(llc.protoid == htons(LLC_PID_NDP_HELLO))) {
	    return(packet + sizeof(ether) + sizeof(llc));
    } 
    return(NULL);
}

size_t ndp_peer(struct master_msg *msg) {
    char *packet = NULL;
    size_t length;
    struct ndp_header ndp;

    char *tlv;
    char *pos;

    uint16_t tlv_type;
    uint16_t tlv_length;

    char *hostname = NULL;

    assert(msg);

    packet = msg->msg;
    length = msg->len;

    assert(packet);
    assert((pos = ndp_check(packet, length)) != NULL);
    length -= VOIDP_DIFF(pos, packet);
    if (length < sizeof(ndp)) {
	my_log(INFO, "missing NDP header");
	return 0;
    }

    memcpy(&ndp, pos, sizeof(ndp));
    if (!inet_ntop(AF_INET, ndp.addr, msg->peer, IFDESCRSIZE)) {
	my_log(INFO, "failed to copy peer addr");
	return 0;
    }
    // XXX: this should be improved
    msg->ttl = LADVD_TTL;


    // update tlv counters
    pos += sizeof(ndp);
    length -= sizeof(ndp);

    // return the packet length
    return(VOIDP_DIFF(pos, packet));
}